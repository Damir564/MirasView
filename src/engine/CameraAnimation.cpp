#include "CameraAnimation.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <glm/gtc/constants.hpp>
#include "Log.h"

// ============================================================
// Catmull-Rom Spline
// ============================================================
glm::vec3 CameraAnimator::catmullRom(const glm::vec3& p0, const glm::vec3& p1,
    const glm::vec3& p2, const glm::vec3& p3, float t) {
    float t2 = t * t;
    float t3 = t2 * t;

    // Catmull-Rom matrix formulation
    glm::vec3 result = 0.5f * (
        (2.0f * p1) +
        (-p0 + p2) * t +
        (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
        (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3
        );

    return result;
}

// ============================================================
// Path Management
// ============================================================
void CameraAnimator::addKeyframe(const CameraKeyframe& kf) {
    m_path.keyframes.push_back(kf);
    // Keep sorted by timestamp
    std::sort(m_path.keyframes.begin(), m_path.keyframes.end(),
        [](const CameraKeyframe& a, const CameraKeyframe& b) {
            return a.timestamp < b.timestamp;
        });
    m_path.recalculateDuration();
}

void CameraAnimator::insertKeyframe(size_t index, const CameraKeyframe& kf) {
    if (index <= m_path.keyframes.size()) {
        m_path.keyframes.insert(m_path.keyframes.begin() + index, kf);
        m_path.recalculateDuration();
    }
}

void CameraAnimator::removeKeyframe(size_t index) {
    if (index < m_path.keyframes.size()) {
        m_path.keyframes.erase(m_path.keyframes.begin() + index);
        m_path.recalculateDuration();
    }
}

void CameraAnimator::clearKeyframes() {
    m_path.keyframes.clear();
    m_path.totalDuration = 0.0f;
    stop();
}

// ============================================================
// Playback Control
// ============================================================
void CameraAnimator::play() {
    if (m_path.keyframes.size() < 2) return;
    m_playState = AnimationPlayState::Playing;
}

void CameraAnimator::pause() {
    if (m_playState == AnimationPlayState::Playing) {
        m_playState = AnimationPlayState::Paused;
    }
}

void CameraAnimator::stop() {
    m_playState = AnimationPlayState::Stopped;
    m_currentTime = 0.0f;
}

bool CameraAnimator::update(float deltaTime) {
    if (m_playState != AnimationPlayState::Playing) return false;
    if (m_path.keyframes.size() < 2) {
        stop();
        return false;
    }

    m_currentTime += deltaTime * m_playbackSpeed;

    if (m_currentTime >= m_path.totalDuration) {
        if (m_path.loop) {
            m_currentTime = fmod(m_currentTime, m_path.totalDuration);
        }
        else {
            m_currentTime = m_path.totalDuration;
            m_playState = AnimationPlayState::Stopped;
        }
    }

    if (m_currentTime < 0.0f) {
        m_currentTime = 0.0f;
    }

    return true;
}

// ============================================================
// Segment Finding
// ============================================================
size_t CameraAnimator::findSegment(float time) const {
    if (m_path.keyframes.empty()) return 0;

    for (size_t i = 0; i < m_path.keyframes.size() - 1; ++i) {
        if (time >= m_path.keyframes[i].timestamp &&
            time <= m_path.keyframes[i + 1].timestamp) {
            return i;
        }
    }

    return m_path.keyframes.size() - 2; // Last segment
}

// ============================================================
// Angle Conversion
// ============================================================
void CameraAnimator::lookTargetToAngles(const glm::vec3& from, const glm::vec3& target,
    float& yaw, float& pitch) {
    glm::vec3 dir = glm::normalize(target - from);

    yaw = glm::degrees(atan2(dir.z, dir.x));
    pitch = glm::degrees(asin(glm::clamp(dir.y, -1.0f, 1.0f)));
}

// ============================================================
// Interpolation
// ============================================================
CameraState CameraAnimator::interpolateLinear(const CameraKeyframe& a,
    const CameraKeyframe& b, float t) const {
    CameraState state;
    state.position = glm::mix(a.position, b.position, t);
    state.lookTarget = glm::mix(a.lookTarget, b.lookTarget, t);
    lookTargetToAngles(state.position, state.lookTarget, state.yaw, state.pitch);
    return state;
}

CameraState CameraAnimator::interpolateCurve(size_t segIdx, float t) const {
    const auto& kfs = m_path.keyframes;
    size_t count = kfs.size();

    // Get 4 control points for Catmull-Rom
    // p0 = previous keyframe (or mirror of p1)
    // p1 = current keyframe
    // p2 = next keyframe
    // p3 = keyframe after next (or mirror of p2)

    size_t i0 = (segIdx > 0) ? segIdx - 1 : segIdx;
    size_t i1 = segIdx;
    size_t i2 = segIdx + 1;
    size_t i3 = (segIdx + 2 < count) ? segIdx + 2 : segIdx + 1;

    // Handle loop wrapping
    if (m_path.loop) {
        i0 = (segIdx == 0) ? count - 1 : segIdx - 1;
        i3 = (segIdx + 2 >= count) ? (segIdx + 2) % count : segIdx + 2;
    }

    CameraState state;
    state.position = catmullRom(
        kfs[i0].position, kfs[i1].position,
        kfs[i2].position, kfs[i3].position, t
    );
    state.lookTarget = catmullRom(
        kfs[i0].lookTarget, kfs[i1].lookTarget,
        kfs[i2].lookTarget, kfs[i3].lookTarget, t
    );
    lookTargetToAngles(state.position, state.lookTarget, state.yaw, state.pitch);
    return state;
}

CameraState CameraAnimator::getCurrentState() const {
    return getStateAtTime(m_currentTime);
}

CameraState CameraAnimator::getStateAtTime(float time) const {
    const auto& kfs = m_path.keyframes;

    if (kfs.empty()) {
        return { glm::vec3(0), glm::vec3(0,0,-1), 0, 0 };
    }

    if (kfs.size() == 1) {
        CameraState s;
        s.position = kfs[0].position;
        s.lookTarget = kfs[0].lookTarget;
        lookTargetToAngles(s.position, s.lookTarget, s.yaw, s.pitch);
        return s;
    }

    // Clamp time
    time = glm::clamp(time, 0.0f, m_path.totalDuration);

    size_t seg = findSegment(time);

    // Calculate local t within this segment
    float segStart = kfs[seg].timestamp;
    float segEnd = kfs[seg + 1].timestamp;
    float segDuration = segEnd - segStart;
    float localT = (segDuration > 0.0001f) ? (time - segStart) / segDuration : 0.0f;
    localT = glm::clamp(localT, 0.0f, 1.0f);

    // Choose interpolation method based on keyframe setting
    if (kfs[seg].useCurve) {
        return interpolateCurve(seg, localT);
    }
    else {
        return interpolateLinear(kfs[seg], kfs[seg + 1], localT);
    }
}

// ============================================================
// Path Visualization
// ============================================================
std::vector<glm::vec3> CameraAnimator::generatePathPoints(int subdivisions) const {
    std::vector<glm::vec3> points;
    const auto& kfs = m_path.keyframes;

    if (kfs.size() < 2) {
        for (const auto& kf : kfs) {
            points.push_back(kf.position);
        }
        return points;
    }

    for (size_t seg = 0; seg < kfs.size() - 1; ++seg) {
        int numSteps = subdivisions;
        if (!kfs[seg].useCurve) {
            numSteps = 2; // Straight line only needs start and end
        }

        for (int step = 0; step <= numSteps; ++step) {
            float localT = static_cast<float>(step) / static_cast<float>(numSteps);
            float time = glm::mix(kfs[seg].timestamp, kfs[seg + 1].timestamp, localT);
            CameraState state = getStateAtTime(time);
            points.push_back(state.position);
        }
    }

    return points;
}

// ============================================================
// Keyframe Factory
// ============================================================
CameraKeyframe CameraAnimator::makeKeyframe(const glm::vec3& pos, float yaw, float pitch,
    float timestamp, bool curved) {
    CameraKeyframe kf;
    kf.position = pos;
    kf.timestamp = timestamp;
    kf.useCurve = curved;

    // Calculate look target from yaw/pitch (10 units in front)
    glm::vec3 front;
    front.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    front.y = sin(glm::radians(pitch));
    front.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
    front = glm::normalize(front);

    kf.lookTarget = pos + front * 10.0f;

    return kf;
}

// ============================================================
// Save/Load (Simple binary format)
// ============================================================
void CameraAnimator::savePath(const std::string& filepath) const {
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR("[CameraAnim] Failed to save: " << filepath << "\n");
        return;
    }

    // Magic + version
    const char magic[] = "CMAP";
    uint32_t version = 1;
    file.write(magic, 4);
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));

    // Name
    uint32_t nameLen = static_cast<uint32_t>(m_path.name.size());
    file.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
    file.write(m_path.name.data(), nameLen);

    // Loop flag
    file.write(reinterpret_cast<const char*>(&m_path.loop), sizeof(m_path.loop));

    // Keyframe count
    uint32_t count = static_cast<uint32_t>(m_path.keyframes.size());
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    // Keyframes
    for (const auto& kf : m_path.keyframes) {
        file.write(reinterpret_cast<const char*>(&kf.position), sizeof(glm::vec3));
        file.write(reinterpret_cast<const char*>(&kf.lookTarget), sizeof(glm::vec3));
        file.write(reinterpret_cast<const char*>(&kf.timestamp), sizeof(float));
        file.write(reinterpret_cast<const char*>(&kf.useCurve), sizeof(bool));
    }

    LOG_INFO("[CameraAnim] Saved " << count << " keyframes to " << filepath << "\n");
}

bool CameraAnimator::loadPath(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR("[CameraAnim] Failed to load: " << filepath << "\n");
        return false;
    }

    char magic[4];
    file.read(magic, 4);
    if (std::string(magic, 4) != "CMAP") {
        LOG_ERROR("[CameraAnim] Invalid file format\n");
        return false;
    }

    uint32_t version;
    file.read(reinterpret_cast<char*>(&version), sizeof(version));

    CameraAnimationPath newPath;

    uint32_t nameLen;
    file.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
    newPath.name.resize(nameLen);
    file.read(newPath.name.data(), nameLen);

    file.read(reinterpret_cast<char*>(&newPath.loop), sizeof(newPath.loop));

    uint32_t count;
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    newPath.keyframes.resize(count);
    for (auto& kf : newPath.keyframes) {
        file.read(reinterpret_cast<char*>(&kf.position), sizeof(glm::vec3));
        file.read(reinterpret_cast<char*>(&kf.lookTarget), sizeof(glm::vec3));
        file.read(reinterpret_cast<char*>(&kf.timestamp), sizeof(float));
        file.read(reinterpret_cast<char*>(&kf.useCurve), sizeof(bool));
    }

    newPath.recalculateDuration();
    m_path = newPath;
    stop();

    LOG_INFO("[CameraAnim] Loaded " << count << " keyframes from " << filepath << "\n");
    return true;
}