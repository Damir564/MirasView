// Must match GpuDrawData / GpuTransform in engine/RenderTypes.h (std430).
struct DrawData {
    vec4 baseColor;
    uint transformIndex;
    int alphaMode;
    float metallic;
    float roughness;
    float alphaCutoff;
    float _pad0;
    float _pad1;
    float _pad2;
};

struct TransformData {
    mat4 model;
    mat4 normal;
};

layout(std430, set = 0, binding = 1) readonly buffer DrawBuffer { DrawData draws[]; };
layout(std430, set = 0, binding = 2) readonly buffer TransformBuffer { TransformData transforms[]; };
