// Sky model: a single-scattering Rayleigh + Mie atmosphere with an ozone layer, seen from a little above
// the ground. Precomputed into the sky LUT (sky_lut.frag), which the background, fog, reflections and the
// ambient irradiance all sample. The constants must match engine/Atmosphere.cpp. Needs frame_ubo.glsl.

const float PLANET_RADIUS = 6360e3;
const float ATMOSPHERE_RADIUS = 6460e3;
const float OBSERVER_ALTITUDE = 200.0;
const vec3 RAYLEIGH_SCATTERING = vec3(5.802e-6, 13.558e-6, 33.1e-6);
const float RAYLEIGH_HEIGHT = 8000.0;
const float MIE_SCATTERING = 3.996e-6;
const float MIE_EXTINCTION = 4.44e-6;
const float MIE_HEIGHT = 1200.0;
const float MIE_G = 0.8;
const vec3 OZONE_ABSORPTION = vec3(0.650e-6, 1.881e-6, 0.085e-6);
// Light scattered more than once, approximated as isotropic scattering of the directly lit air. Single
// scattering alone leaves the sky too dark and saturated.
const float MULTIPLE_SCATTERING = 0.6;

// LUT parameterization: u = azimuth, v = elevation (zenith at the top). The square root spends most
// texels around the horizon, where the sky changes fastest.
vec2 skyUv(vec3 dir) {
    float elevation = asin(clamp(dir.y, -1.0, 1.0));
    float v = 0.5 - 0.5 * sign(elevation) * sqrt(abs(elevation) / (0.5 * PI));
    float u = atan(dir.z, dir.x) / (2.0 * PI) + 0.5;
    return vec2(u, v);
}

vec3 skyDirection(vec2 uv) {
    float s = (0.5 - uv.y) * 2.0;
    float elevation = sign(s) * s * s * 0.5 * PI;
    float azimuth = (uv.x - 0.5) * 2.0 * PI;
    return vec3(cos(elevation) * cos(azimuth), sin(elevation), cos(elevation) * sin(azimuth));
}

// Mip level of the sky LUT whose blur roughly matches a GGX lobe of this roughness.
float skyLutLod(float roughness) {
    float a = roughness * roughness;
    return log2(1.0 + a * 40.0);
}

// For a ray starting at radius r whose direction has zenith cosine mu, against a sphere of radius R.
// Written so the nearly equal radii do not cancel.
float sphereDiscriminant(float r, float mu, float R) {
    return r * r * mu * mu + (R - r) * (R + r);
}

vec3 extinctionAt(float altitude, float haze) {
    float rayleigh = exp(-altitude / RAYLEIGH_HEIGHT);
    float mie = exp(-altitude / MIE_HEIGHT);
    float ozone = max(0.0, 1.0 - abs(altitude - 25e3) / 15e3);
    return RAYLEIGH_SCATTERING * rayleigh + vec3(MIE_EXTINCTION * haze * mie) + OZONE_ABSORPTION * ozone;
}

// Transmittance from radius r along zenith cosine mu to space; zero where the planet is in the way.
vec3 transmittanceToSpace(float r, float mu, float haze) {
    if (mu < 0.0 && sphereDiscriminant(r, mu, PLANET_RADIUS) >= 0.0)
        return vec3(0.0);
    float len = -r * mu + sqrt(max(sphereDiscriminant(r, mu, ATMOSPHERE_RADIUS), 0.0));
    const int STEPS = 12;
    vec3 opticalDepth = vec3(0.0);
    for (int i = 0; i < STEPS; ++i) {
        // Quadratic spacing puts most samples in the dense air near the start.
        float s0 = float(i) / float(STEPS);
        float s1 = float(i + 1) / float(STEPS);
        float t0 = len * s0 * s0;
        float t1 = len * s1 * s1;
        float t = 0.5 * (t0 + t1);
        float altitude = sqrt(r * r + t * t + 2.0 * r * mu * t) - PLANET_RADIUS;
        opticalDepth += extinctionAt(altitude, haze) * (t1 - t0);
    }
    return exp(-opticalDepth);
}

float rayleighPhase(float nu) {
    return 3.0 / (16.0 * PI) * (1.0 + nu * nu);
}

// Cornette-Shanks.
float miePhase(float nu, float g) {
    float g2 = g * g;
    return 3.0 / (8.0 * PI) * ((1.0 - g2) * (1.0 + nu * nu)) / ((2.0 + g2) * pow(1.0 + g2 - 2.0 * g * nu, 1.5));
}

// Radiance reaching the observer from direction `dir`: light the air scatters towards it, plus the sunlit
// ground below the horizon.
vec3 skyRadiance(vec3 dir, vec3 toSun, vec3 sunIrradiance, float haze, float groundAlbedo) {
    float r = PLANET_RADIUS + OBSERVER_ALTITUDE;
    float mu = dir.y;
    bool hitsGround = mu < 0.0 && sphereDiscriminant(r, mu, PLANET_RADIUS) >= 0.0;
    float len = hitsGround ? -r * mu - sqrt(sphereDiscriminant(r, mu, PLANET_RADIUS))
                           : -r * mu + sqrt(max(sphereDiscriminant(r, mu, ATMOSPHERE_RADIUS), 0.0));
    float nu = dot(dir, toSun);
    float phaseR = rayleighPhase(nu);
    float phaseM = miePhase(nu, MIE_G);

    const int STEPS = 32;
    vec3 origin = vec3(0.0, r, 0.0);
    vec3 transmittance = vec3(1.0);
    vec3 inscatter = vec3(0.0);
    for (int i = 0; i < STEPS; ++i) {
        float s0 = float(i) / float(STEPS);
        float s1 = float(i + 1) / float(STEPS);
        float t0 = len * s0 * s0;
        float t1 = len * s1 * s1;
        vec3 p = origin + dir * (0.5 * (t0 + t1));
        float pr = length(p);
        float altitude = pr - PLANET_RADIUS;
        vec3 rayleigh = RAYLEIGH_SCATTERING * exp(-altitude / RAYLEIGH_HEIGHT);
        vec3 mie = vec3(MIE_SCATTERING * haze * exp(-altitude / MIE_HEIGHT));
        vec3 scattering = rayleigh * (phaseR + MULTIPLE_SCATTERING / (4.0 * PI)) +
            mie * (phaseM + MULTIPLE_SCATTERING / (4.0 * PI));
        vec3 extinction = extinctionAt(altitude, haze);
        vec3 sunTransmittance = transmittanceToSpace(pr, dot(p, toSun) / pr, haze);
        vec3 stepTransmittance = exp(-extinction * (t1 - t0));
        // Integrates the segment analytically, so long steps near the horizon do not overshoot.
        inscatter += transmittance * scattering * sunTransmittance * (1.0 - stepTransmittance) /
            max(extinction, vec3(1e-12));
        transmittance *= stepTransmittance;
    }
    vec3 radiance = inscatter * sunIrradiance;

    if (hitsGround) {
        vec3 p = origin + dir * len;
        float pr = length(p);
        float cosSun = dot(p, toSun) / pr;
        vec3 sunTransmittance = transmittanceToSpace(pr, cosSun, haze);
        radiance += transmittance * sunIrradiance * sunTransmittance * max(cosSun, 0.0) * groundAlbedo / PI;
    }
    return radiance;
}
