// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#ifdef __SWITCH__

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "audio_core/audio_types.h"
#include "audio_core/libnx_sink.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "GBAStation/switch_libnx.h"

namespace AudioCore {

// Audout is fixed at 48000 Hz, stereo PCM16.
static constexpr unsigned int kAudoutRate     = 48000;
static constexpr std::size_t  kOutSamples     = 1024;               // stereo frames per audout buffer
static constexpr std::size_t  kOutBytes       = kOutSamples * sizeof(s16) * 2; // 4096 bytes
static constexpr std::size_t  kAlignedSize    = 0x1000;
static_assert(kOutBytes == kAlignedSize, "audout buffer must be page-aligned");
static constexpr int kNumBuffers = 3;
static constexpr u64 kWaitTimeoutNs = 33'000'000ULL; // 33ms

// s_input_buf must cover all resampling source positions:
// max index = floor((kOutSamples-1) * native/audout) + 1 = floor(697.71) + 1 = 698 → 699 slots.
// The per-callback request count is computed via Bresenham and averages kOutSamples *
// native/audout = 698.19, eliminating the steady FIFO drain that causes pitch-low.
static constexpr std::size_t kInBufSize = 699; // buffer slots (must cover max resampling index)

static AudioOutBuffer s_audio_buffers[kNumBuffers];
static void*          s_audio_data[kNumBuffers];
static s16            s_input_buf[kInBufSize * 2]; // stereo input at native_sample_rate

struct UiClip {
    std::vector<s16> samples;
};

static std::array<UiClip, 5> s_ui_clips;
static std::atomic<int> s_pending_ui_sound{-1};

u16 ReadU16(const u8* data) {
    return static_cast<u16>(data[0] | (static_cast<u16>(data[1]) << 8));
}

u32 ReadU32(const u8* data) {
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) |
           (static_cast<u32>(data[2]) << 16) | (static_cast<u32>(data[3]) << 24);
}

bool LoadUiWav(const char* path, UiClip& clip) {
    std::FILE* file = std::fopen(path, "rb");
    if (!file) {
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);
    if (size < 44 || size > 4 * 1024 * 1024) {
        std::fclose(file);
        return false;
    }
    std::vector<u8> bytes(static_cast<std::size_t>(size));
    const bool read_ok = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    if (!read_ok || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return false;
    }

    u16 format = 0;
    u16 channels = 0;
    u16 bits = 0;
    u32 sample_rate = 0;
    const u8* pcm = nullptr;
    std::size_t pcm_bytes = 0;
    for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
        const u32 chunk_size = ReadU32(bytes.data() + offset + 4);
        if (offset + 8 + chunk_size > bytes.size()) {
            break;
        }
        if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && chunk_size >= 16) {
            format = ReadU16(bytes.data() + offset + 8);
            channels = ReadU16(bytes.data() + offset + 10);
            sample_rate = ReadU32(bytes.data() + offset + 12);
            bits = ReadU16(bytes.data() + offset + 22);
        } else if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
            pcm = bytes.data() + offset + 8;
            pcm_bytes = chunk_size;
        }
        offset += 8 + chunk_size + (chunk_size & 1u);
    }
    if (format != 1 || (channels != 1 && channels != 2) || bits != 16 ||
        sample_rate == 0 || !pcm || pcm_bytes < channels * sizeof(s16)) {
        return false;
    }

    const auto* input = reinterpret_cast<const s16*>(pcm);
    const std::size_t input_frames = pcm_bytes / (channels * sizeof(s16));
    const std::size_t output_frames =
        std::max<std::size_t>(1, input_frames * kAudoutRate / sample_rate);
    clip.samples.resize(output_frames * 2);
    const double source_step = static_cast<double>(sample_rate) / kAudoutRate;
    for (std::size_t frame = 0; frame < output_frames; ++frame) {
        const double source = frame * source_step;
        const std::size_t first = std::min(static_cast<std::size_t>(source), input_frames - 1);
        const std::size_t second = std::min(first + 1, input_frames - 1);
        const float fraction = static_cast<float>(source - first);
        for (u16 channel = 0; channel < 2; ++channel) {
            const std::size_t source_channel = channels == 1 ? 0 : channel;
            const float a = input[first * channels + source_channel];
            const float b = input[second * channels + source_channel];
            clip.samples[frame * 2 + channel] =
                static_cast<s16>((a + (b - a) * fraction) * 0.70f);
        }
    }
    return true;
}

void LoadUiSounds() {
    constexpr std::array<const char*, 5> files{{
        "SeNaviFocus.wav",
        "SeBtnDecide.wav",
        "SeFooterDecideFinish.wav",
        "SeKeyErrorCursor.wav",
        "SeSliderTickOver.wav",
    }};
    for (std::size_t i = 0; i < files.size(); ++i) {
        char path[160]{};
        std::snprintf(path, sizeof(path), "romfs:/rescources/sounds/switch/%s", files[i]);
        if (!LoadUiWav(path, s_ui_clips[i])) {
            LOG_WARNING(Audio_Sink, "LibnxSink: UI sound unavailable {}", path);
        }
    }
}

LibnxSink::LibnxSink(std::string_view) {
    const LibnxResult rc = audoutInitialize();
    if (rc != 0) {
        LOG_ERROR(Audio_Sink, "audoutInitialize failed: 0x{:08x}", static_cast<unsigned>(rc));
        return;
    }

    const LibnxResult start_rc = audoutStartAudioOut();
    if (start_rc != 0) {
        LOG_ERROR(Audio_Sink, "audoutStartAudioOut failed: 0x{:08x}", static_cast<unsigned>(start_rc));
        audoutExit();
        return;
    }

    for (int i = 0; i < kNumBuffers; i++) {
        s_audio_data[i] = aligned_alloc(kAlignedSize, kAlignedSize);
        if (!s_audio_data[i]) {
            LOG_ERROR(Audio_Sink, "Failed to allocate audio buffer {}", i);
            audoutStopAudioOut();
            audoutExit();
            return;
        }
        std::memset(s_audio_data[i], 0, kAlignedSize);

        s_audio_buffers[i].next        = nullptr;
        s_audio_buffers[i].buffer      = s_audio_data[i];
        s_audio_buffers[i].buffer_size = kAlignedSize;
        s_audio_buffers[i].data_size   = kOutBytes;
        s_audio_buffers[i].data_offset = 0;

        audoutAppendAudioOutBuffer(&s_audio_buffers[i]);
    }

    LoadUiSounds();

    initialized = true;
    running.store(true, std::memory_order_release);
    audio_thread = std::thread(&LibnxSink::AudioThread, this);

    LOG_INFO(Audio_Sink, "LibnxSink: {}Hz→{}Hz, {}×{}-sample audout buffers",
             native_sample_rate, kAudoutRate, kNumBuffers, kOutSamples);
}

LibnxSink::~LibnxSink() {
    running.store(false, std::memory_order_release);
    if (audio_thread.joinable())
        audio_thread.join();

    if (initialized) {
        audoutStopAudioOut();
        audoutExit();
        for (int i = 0; i < kNumBuffers; i++) {
            free(s_audio_data[i]);
            s_audio_data[i] = nullptr;
        }
        initialized = false;
    }
    s_pending_ui_sound.store(-1, std::memory_order_release);
    for (UiClip& clip : s_ui_clips) {
        clip.samples.clear();
    }
}

void LibnxSink::SetCallback(std::function<void(s16*, std::size_t)> cb) {
    callback = std::move(cb);
}

void LibnxSink::AudioThread() {
    // Boost priority so the audio thread isn't preempted before appending its buffer.
    svcSetThreadPriority(CUR_THREAD_HANDLE, 38);
    Common::SetCurrentThreadAffinityMask(0, (1ULL << 0) | (1ULL << 1));

    // Bresenham accumulator: n_in alternates between 698 and 699 so the average is exactly
    // kOutSamples * native_sample_rate / kAudoutRate = 698.19, matching DSP production rate.
    std::size_t input_acc = 0;
    int active_ui_sound = -1;
    std::size_t ui_frame = 0;

    while (running.load(std::memory_order_acquire)) {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;

        const LibnxResult rc = audoutWaitPlayFinish(&released, &count, kWaitTimeoutNs);
        if (rc != 0 || count == 0 || released == nullptr)
            continue;

        input_acc += kOutSamples * static_cast<std::size_t>(native_sample_rate);
        const std::size_t n_in = input_acc / kAudoutRate;
        input_acc %= kAudoutRate;

        if (callback) {
            callback(s_input_buf, n_in);
        } else {
            std::memset(s_input_buf, 0, n_in * sizeof(s16) * 2);
        }

        // Linear interpolation: native_sample_rate Hz → kAudoutRate Hz
        auto* out = reinterpret_cast<s16*>(released->buffer);
        constexpr float kRatio = static_cast<float>(native_sample_rate) / static_cast<float>(kAudoutRate);
        for (std::size_t i = 0; i < kOutSamples; i++) {
            const float src  = static_cast<float>(i) * kRatio;
            const std::size_t idx0 = std::min(static_cast<std::size_t>(src), n_in - 1);
            const float frac = src - static_cast<float>(idx0);
            const std::size_t idx1 = std::min(idx0 + 1, n_in - 1);
            for (int ch = 0; ch < 2; ch++) {
                const float a = static_cast<float>(s_input_buf[idx0 * 2 + ch]);
                const float b = static_cast<float>(s_input_buf[idx1 * 2 + ch]);
                out[i * 2 + ch] = static_cast<s16>(a + frac * (b - a));
            }
        }

        const int requested_sound = s_pending_ui_sound.exchange(-1, std::memory_order_acq_rel);
        if (requested_sound >= 0 && requested_sound < static_cast<int>(s_ui_clips.size()) &&
            !s_ui_clips[requested_sound].samples.empty()) {
            active_ui_sound = requested_sound;
            ui_frame = 0;
        }
        if (active_ui_sound >= 0) {
            const auto& ui_samples = s_ui_clips[active_ui_sound].samples;
            const std::size_t ui_frames = ui_samples.size() / 2;
            for (std::size_t i = 0; i < kOutSamples && ui_frame < ui_frames; ++i, ++ui_frame) {
                for (int ch = 0; ch < 2; ++ch) {
                    const int mixed = static_cast<int>(out[i * 2 + ch]) +
                                      static_cast<int>(ui_samples[ui_frame * 2 + ch]);
                    out[i * 2 + ch] = static_cast<s16>(std::clamp(mixed, -32768, 32767));
                }
            }
            if (ui_frame >= ui_frames) {
                active_ui_sound = -1;
                ui_frame = 0;
            }
        }

        released->data_size = kOutBytes;
        audoutAppendAudioOutBuffer(released);
    }
}

void PlayLibnxUiSound(LibnxUiSound sound) {
    s_pending_ui_sound.store(static_cast<int>(sound), std::memory_order_release);
}

std::vector<std::string> ListLibnxSinkDevices() {
    return {"Default"};
}

} // namespace AudioCore

#endif // __SWITCH__
