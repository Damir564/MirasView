#include "VideoEncoder.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

std::string failure(const char* what, HRESULT hr)
{
    char text[128];
    snprintf(text, sizeof(text), "%s failed (HRESULT 0x%08lX)", what, static_cast<unsigned long>(hr));
    return text;
}

uint8_t clampChroma(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 16, 240));
}

// BT.709 limited range in 8-bit fixed point (coefficients * 256). Chroma is the average of each 2x2 block.
void bgraToNv12(const uint8_t* bgra, uint32_t width, uint32_t height, uint8_t* yPlane, uint8_t* uvPlane)
{
    for (uint32_t row = 0; row < height; row += 2) {
        const uint8_t* rows[2] = { bgra + size_t(row) * width * 4, bgra + size_t(row + 1) * width * 4 };
        uint8_t* yRows[2] = { yPlane + size_t(row) * width, yPlane + size_t(row + 1) * width };
        uint8_t* uv = uvPlane + size_t(row / 2) * width;
        for (uint32_t col = 0; col < width; col += 2) {
            int sumR = 0, sumG = 0, sumB = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const uint8_t* p = rows[dy] + size_t(col + dx) * 4;
                    const int b = p[0], g = p[1], r = p[2];
                    yRows[dy][col + dx] = static_cast<uint8_t>(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16);
                    sumR += r;
                    sumG += g;
                    sumB += b;
                }
            }
            // The sums are 4x the average, hence >> 10 instead of >> 8.
            uv[col] = clampChroma(((-26 * sumR - 87 * sumG + 112 * sumB + 512) >> 10) + 128);
            uv[col + 1] = clampChroma(((112 * sumR - 102 * sumG - 10 * sumB + 512) >> 10) + 128);
        }
    }
}

HRESULT setVideoFormat(IMFMediaType* type, const GUID& subtype, const VideoEncoder::Settings& settings)
{
    HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, settings.width, settings.height);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, settings.fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    return hr;
}

} // namespace

struct VideoEncoder::Impl {
    ComPtr<IMFSinkWriter> writer;
    DWORD stream = 0;
    bool comInitialized = false;
    bool mfStarted = false;
    std::vector<uint8_t> nv12;

    ~Impl()
    {
        writer.Reset();
        if (mfStarted) MFShutdown();
        if (comInitialized) CoUninitialize();
    }
};

bool VideoEncoder::supported()
{
    return true;
}

bool VideoEncoder::open(const Settings& settings, std::string& error)
{
    if (m_impl) {
        error = "The video encoder is already open";
        return false;
    }
    if (settings.width == 0 || settings.height == 0 || settings.width % 2 || settings.height % 2 || settings.fps == 0) {
        error = "Video size must be even and the frame rate above zero";
        return false;
    }

    auto impl = std::make_unique<Impl>();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(hr))
        impl->comInitialized = true;
    else if (hr != RPC_E_CHANGED_MODE) { // COM already set up differently on this thread is fine for MF.
        error = failure("CoInitializeEx", hr);
        return false;
    }
    if (FAILED(hr = MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        error = failure("MFStartup", hr);
        return false;
    }
    impl->mfStarted = true;

    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    // MP4 regardless of the file extension.
    if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    const std::wstring widePath = std::filesystem::path(settings.path).wstring();
    if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(widePath.c_str(), nullptr, attributes.Get(), &impl->writer);
    if (FAILED(hr)) {
        error = failure("Creating the video file", hr);
        return false;
    }

    ComPtr<IMFMediaType> outputType;
    hr = MFCreateMediaType(&outputType);
    if (SUCCEEDED(hr)) hr = setVideoFormat(outputType.Get(), MFVideoFormat_H264, settings);
    if (SUCCEEDED(hr)) hr = outputType->SetUINT32(MF_MT_AVG_BITRATE, settings.bitrate);
    if (SUCCEEDED(hr)) hr = outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    if (SUCCEEDED(hr)) hr = impl->writer->AddStream(outputType.Get(), &impl->stream);
    if (FAILED(hr)) {
        error = failure("Setting up the H.264 stream", hr);
        return false;
    }

    // NV12 is always top-down, unlike RGB input whose default row order Media Foundation leaves ambiguous.
    // The colour description is optional; some encoders reject it, so retry without it.
    for (const bool describeColour : { true, false }) {
        ComPtr<IMFMediaType> inputType;
        hr = MFCreateMediaType(&inputType);
        if (SUCCEEDED(hr)) hr = setVideoFormat(inputType.Get(), MFVideoFormat_NV12, settings);
        if (SUCCEEDED(hr)) hr = inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, settings.width);
        if (SUCCEEDED(hr) && describeColour) {
            hr = inputType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
            if (SUCCEEDED(hr)) hr = inputType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
            if (SUCCEEDED(hr)) hr = inputType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        }
        if (SUCCEEDED(hr)) hr = impl->writer->SetInputMediaType(impl->stream, inputType.Get(), nullptr);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) {
        error = failure("Setting the encoder input format", hr);
        return false;
    }
    if (FAILED(hr = impl->writer->BeginWriting())) {
        error = failure("Starting the video file", hr);
        return false;
    }

    impl->nv12.resize(size_t(settings.width) * settings.height * 3 / 2);
    m_impl = std::move(impl);
    m_settings = settings;
    m_framesWritten = 0;
    return true;
}

bool VideoEncoder::writeFrame(const std::vector<uint8_t>& bgra, std::string& error)
{
    if (!m_impl) {
        error = "The video encoder is not open";
        return false;
    }
    const uint32_t width = m_settings.width;
    const uint32_t height = m_settings.height;
    if (bgra.size() < size_t(width) * height * 4) {
        error = "Captured frame has the wrong size";
        return false;
    }

    std::vector<uint8_t>& nv12 = m_impl->nv12;
    bgraToNv12(bgra.data(), width, height, nv12.data(), nv12.data() + size_t(width) * height);

    const DWORD size = static_cast<DWORD>(nv12.size());
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(size, &buffer);
    BYTE* data = nullptr;
    if (SUCCEEDED(hr)) hr = buffer->Lock(&data, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(data, nv12.data(), size);
        buffer->Unlock();
        hr = buffer->SetCurrentLength(size);
    }

    // Timestamps in 100 ns units, computed from the frame number so they never drift.
    const uint64_t fps = m_settings.fps;
    const LONGLONG start = static_cast<LONGLONG>(m_framesWritten * 10'000'000ull / fps);
    const LONGLONG end = static_cast<LONGLONG>((m_framesWritten + 1) * 10'000'000ull / fps);
    ComPtr<IMFSample> sample;
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(start);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(end - start);
    if (SUCCEEDED(hr)) hr = m_impl->writer->WriteSample(m_impl->stream, sample.Get());
    if (FAILED(hr)) {
        error = failure("Encoding a video frame", hr);
        return false;
    }
    ++m_framesWritten;
    return true;
}

bool VideoEncoder::finish(std::string& error)
{
    if (!m_impl) {
        error = "The video encoder is not open";
        return false;
    }
    const HRESULT hr = m_impl->writer->Finalize();
    m_impl.reset();
    if (FAILED(hr)) {
        error = failure("Finishing the video file", hr);
        return false;
    }
    return true;
}

void VideoEncoder::abort()
{
    if (!m_impl)
        return;
    m_impl.reset();
    std::error_code ignored;
    std::filesystem::remove(m_settings.path, ignored);
}

#else // !_WIN32

struct VideoEncoder::Impl {};

bool VideoEncoder::supported()
{
    return false;
}

bool VideoEncoder::open(const Settings&, std::string& error)
{
    error = "Video export needs Windows (Media Foundation)";
    return false;
}

bool VideoEncoder::writeFrame(const std::vector<uint8_t>&, std::string& error)
{
    error = "Video export needs Windows (Media Foundation)";
    return false;
}

bool VideoEncoder::finish(std::string& error)
{
    error = "Video export needs Windows (Media Foundation)";
    return false;
}

void VideoEncoder::abort() {}

#endif

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder()
{
    abort();
}
