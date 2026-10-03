#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// H.264 .mp4 writer on Windows Media Foundation. Frames are top-down BGRA8 (sRGB encoded), converted to
// BT.709 limited-range NV12 before encoding. Not available on other platforms (supported() is false there).
class VideoEncoder {
public:
    struct Settings {
        std::string path;
        uint32_t width = 1920;  // must be even
        uint32_t height = 1080; // must be even
        uint32_t fps = 30;
        uint32_t bitrate = 16'000'000; // bits per second
    };

    VideoEncoder();
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    static bool supported();

    bool open(const Settings& settings, std::string& error);
    // bgra holds width * height * 4 bytes.
    bool writeFrame(const std::vector<uint8_t>& bgra, std::string& error);
    // Completes the file. The encoder is closed afterwards, whether or not this succeeded.
    bool finish(std::string& error);
    // Stops without completing the file and deletes it.
    void abort();

    bool isOpen() const { return m_impl != nullptr; }
    uint64_t framesWritten() const { return m_framesWritten; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    Settings m_settings;
    uint64_t m_framesWritten = 0;
};
