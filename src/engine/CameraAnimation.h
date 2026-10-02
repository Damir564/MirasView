#pragma once

#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// A single keyframe point in the camera path
struct CameraKeyframe {
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 lookTarget = glm::vec3(0.0f, 0.0f, -1.0f); // Point the camera looks at
    float timestamp = 0.0f; // Time in seconds when camera should be at this point

    // For curved segments: if true, use Catmull-Rom interpolation 
    // between this keyframe and the next
    bool useCurve = false;
};

// The full animation path
struct CameraAnimationPath {
    std::string name = "Untitled";
    std::vector<CameraKeyframe> keyframes;
    bool loop = false;
    float totalDuration = 0.0f; // Auto-calculated from last keyframe timestamp

    void recalculateDuration() {
        totalDuration = 0.0f;
        if (!keyframes.empty()) {
            totalDuration = keyframes.back().timestamp;
        }
    }
};

// Interpolation result
struct CameraState {
    glm::vec3 position;
    glm::vec3 lookTarget;
    float yaw;
    float pitch;
};

enum class AnimationPlayState {
    Stopped,
    Playing,
    Paused
};

class CameraAnimator {
public:
    CameraAnimator() = default;

    // Path management
    void addKeyframe(const CameraKeyframe& kf);
    void insertKeyframe(size_t index, const CameraKeyframe& kf);
    void removeKeyframe(size_t index);
    void clearKeyframes();

    // Set current path
    void setPath(const CameraAnimationPath& path) { m_path = path; }
    CameraAnimationPath& getPath() { return m_path; }
    const CameraAnimationPath& getPath() const { return m_path; }

    // Playback control
    void play();
    void pause();
    void stop();
    void setPlaybackSpeed(float speed) { m_playbackSpeed = speed; }
    float getPlaybackSpeed() const { return m_playbackSpeed; }

    // Update - call every frame with delta time
    // Returns true if animation is active and camera state is valid
    bool update(float deltaTime);

    // Get interpolated camera state at current time
    CameraState getCurrentState() const;

    // Get state at arbitrary time (for preview)
    CameraState getStateAtTime(float t) const;

    // Generate points along the path for visualization
    std::vector<glm::vec3> generatePathPoints(int segmentSubdivisions = 20) const;

    // State queries
    AnimationPlayState getPlayState() const { return m_playState; }
    bool isPlaying() const { return m_playState == AnimationPlayState::Playing; }
    float getCurrentTime() const { return m_currentTime; }
    float getDuration() const { return m_path.totalDuration; }

    // Helper: create keyframe from current camera state
    static CameraKeyframe makeKeyframe(const glm::vec3& pos, float yaw, float pitch,
        float timestamp, bool curved = false);

    // Saved paths
    void savePath(const std::string& filepath) const;
    bool loadPath(const std::string& filepath);

private:
    CameraAnimationPath m_path;
    AnimationPlayState m_playState = AnimationPlayState::Stopped;
    float m_currentTime = 0.0f;
    float m_playbackSpeed = 1.0f;

    // Catmull-Rom spline interpolation
    static glm::vec3 catmullRom(const glm::vec3& p0, const glm::vec3& p1,
        const glm::vec3& p2, const glm::vec3& p3, float t);

    // Find which segment we're in based on time
    // Returns the index of the keyframe BEFORE current time
    size_t findSegment(float time) const;

    // Linear interpolation between two keyframes
    CameraState interpolateLinear(const CameraKeyframe& a, const CameraKeyframe& b, float t) const;

    // Catmull-Rom interpolation using surrounding keyframes
    CameraState interpolateCurve(size_t segmentIndex, float t) const;

    // Convert lookTarget to yaw/pitch
    static void lookTargetToAngles(const glm::vec3& from, const glm::vec3& target,
        float& yaw, float& pitch);
};