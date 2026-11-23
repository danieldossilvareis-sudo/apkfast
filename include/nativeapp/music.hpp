// nativeapp/music.hpp — Música em streaming (OGG/MP3) sem carregar PCM na memória.
//
// Usa um stream AAudio separado com data callback. O decoder (stb_vorbis ou
// dr_mp3) roda na thread de áudio, preenchendo o buffer de saída em chunks.
// Requer API 26+ (AAudio). Em API 21-25 use Audio::load + play (decode completo).

#pragma once

#include "assets.hpp"
#include "codecs.hpp"

#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nativeapp {

class Music {
public:
    Music() = default;
    ~Music() { stop(); }

    Music(const Music&) = delete;
    Music& operator=(const Music&) = delete;

    /// Abre um arquivo de assets/ (.ogg ou .mp3). Não inicia a reprodução.
    bool open(const std::string& path) {
        stop();
        loop_.store(false);
        playing_.store(false);
        auto& assets = Assets::instance(nullptr);
        if (!assets.manager()) return false;
        data_ = assets.read(path);
        if (data_.empty()) return false;

        const std::string lower = lowercase(path);
        const bool is_ogg = has_extension(lower, ".ogg");
        const bool is_mp3 = has_extension(lower, ".mp3");
        if (!is_ogg && !is_mp3) {
            data_.clear();
            return false;
        }

        int channels = 0;
        int rate = 0;
        if (is_ogg) {
            vorbis_ = stb_vorbis_open_memory(
                data_.data(), static_cast<int>(data_.size()), &vorbis_error_, nullptr);
            if (!vorbis_) { data_.clear(); return false; }
            const stb_vorbis_info info = stb_vorbis_get_info(vorbis_);
            channels = info.channels;
            rate = static_cast<int>(info.sample_rate);
        } else {
            if (!drmp3_init_memory(&mp3_, data_.data(), data_.size(), nullptr)) {
                data_.clear();
                return false;
            }
            mp3_ready_ = true;
            channels = static_cast<int>(mp3_.channels);
            rate = static_cast<int>(mp3_.sampleRate);
        }
        if (channels < 1 || rate <= 0 || rate > 0x7fffffff) {
            close_decoders();
            data_.clear();
            return false;
        }

        source_channels_.store(channels);
        channels_.store(channels);
        sample_rate_.store(rate);
        loop_.store(false);
        playing_.store(false);
        return true;
    }

    /// Inicia (ou retoma, após pause()) a reprodução.
    bool play(bool loop = false) {
#if __ANDROID_API__ >= 26
        if (data_.empty() || (!vorbis_ && !mp3_ready_)) return false;
        if (playing_.load()) return true;
        loop_.store(loop);
        if (!stream_ && !create_stream()) return false;
        playing_.store(true);
        const aaudio_result_t result = AAudioStream_requestStart(stream_);
        // INVALID_STATE = já iniciado; não é erro para nós.
        if (static_cast<int32_t>(result) < 0 &&
            result != AAUDIO_ERROR_INVALID_STATE) {
            playing_.store(false);
            return false;
        }
        return true;
#else
        (void)loop;
        return false;  // AAudio não existe no runtime do NDK < 26
#endif
    }

    void pause() {
#if __ANDROID_API__ >= 26
        if (stream_ && playing_.load()) AAudioStream_requestPause(stream_);
#endif
        playing_.store(false);
    }

    void stop() {
        playing_.store(false);
#if __ANDROID_API__ >= 26
        if (stream_) {
            AAudioStream_requestStop(stream_);
            AAudioStream_close(stream_);  // não retorna enquanto o callback roda
        }
#endif
        stream_ = nullptr;
        close_decoders();
        data_.clear();
    }

    void set_volume(float value) {
        volume_.store(std::isfinite(value) ? std::max(0.0f, std::min(1.0f, value)) : 1.0f);
    }
    float volume() const { return volume_.load(); }
    bool is_playing() const { return playing_.load(); }
    bool is_looping() const { return loop_.load(); }
    void set_loop(bool value) { loop_.store(value); }

    int sample_rate() const { return sample_rate_.load(); }
    int channels() const { return source_channels_.load(); }

private:
    static std::string lowercase(std::string value) {
        for (char& c : value) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return value;
    }

    static bool has_extension(const std::string& path, const char* extension) {
        const size_t size = std::strlen(extension);
        return path.size() >= size && path.compare(path.size() - size, size, extension) == 0;
    }

    void close_decoders() {
        if (vorbis_) { stb_vorbis_close(vorbis_); vorbis_ = nullptr; }
        if (mp3_ready_) { drmp3_uninit(&mp3_); mp3_ready_ = false; }
    }

#if __ANDROID_API__ >= 26
    bool create_stream() {
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) return false;
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setSampleRate(builder, 44100);
        AAudioStreamBuilder_setDataCallback(builder, &Music::data_callback, this);
        AAudioStream* stream = nullptr;
        const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK || !stream) {
            if (stream) AAudioStream_close(stream);
            stream_ = nullptr;
            return false;
        }
        stream_ = stream;
        return true;
    }
#endif

    /// Subsample/oversample linear para converter sample_rate -> 44100.
    /// Chamado somente pela data callback (thread de áudio).
    int read_frames(short* output, int frames) {
        if (!output || frames <= 0) return 0;
        const int rate = sample_rate_.load();
        const int src_ch = source_channels_.load();
        if (rate <= 0 || src_ch < 1) return 0;

        const double step = static_cast<double>(rate) / 44100.0;
        // Ceil garante uma amostra extra para a interpolação do último frame.
        const double needed = std::ceil(static_cast<double>(frames) * step);
        if (needed <= 0.0 || needed > static_cast<double>(INT32_MAX)) return 0;
        const int source_frames = std::max(1, static_cast<int>(needed));
        if (static_cast<size_t>(src_ch) > SIZE_MAX / static_cast<size_t>(source_frames)) return 0;
        try {
            raw_.resize(static_cast<size_t>(source_frames) * static_cast<size_t>(src_ch));
        } catch (...) {
            return 0;  // nunca deixar exceção escapar para a thread RT
        }

        int got = 0;
        for (int attempt = 0; attempt < 2 && got <= 0; ++attempt) {
            if (attempt > 0) {
                if (!loop_.load()) break;
                rewind();
            }
            if (vorbis_) {
                got = stb_vorbis_get_samples_short_interleaved(
                    vorbis_, src_ch, raw_.data(), source_frames);
            } else if (mp3_ready_) {
                const drmp3_uint64 count = drmp3_read_pcm_frames_s16(
                    &mp3_, static_cast<drmp3_uint64>(source_frames), raw_.data());
                got = static_cast<int>(std::min<drmp3_uint64>(
                    count, static_cast<drmp3_uint64>(source_frames)));
            }
        }
        if (got <= 0) return 0;

        const float gain = volume_.load();
        for (int i = 0; i < frames; ++i) {
            const double position = static_cast<double>(i) * got / frames;
            size_t idx = static_cast<size_t>(position);
            if (idx >= static_cast<size_t>(got)) idx = static_cast<size_t>(got) - 1;
            const size_t next = idx + 1 < static_cast<size_t>(got) ? idx + 1 : idx;
            const float t = static_cast<float>(position - static_cast<double>(idx));
            for (int ch = 0; ch < 2; ++ch) {
                const size_t src = src_ch == 1 ? 0 : static_cast<size_t>(ch);
                const float a = static_cast<float>(raw_[idx * static_cast<size_t>(src_ch) + src]);
                const float b = static_cast<float>(raw_[next * static_cast<size_t>(src_ch) + src]);
                float value = (a * (1.0f - t) + b * t) * gain;
                if (value >= 32767.0f) value = 32767.0f;
                if (value <= -32768.0f) value = -32768.0f;
                output[static_cast<size_t>(i) * 2 + static_cast<size_t>(ch)] =
                    static_cast<short>(value);
            }
        }
        return frames;
    }

    void rewind() {
        if (vorbis_) stb_vorbis_seek_start(vorbis_);
        else if (mp3_ready_) drmp3_seek_to_pcm_frame(&mp3_, 0);
    }

#if __ANDROID_API__ >= 26
    static aaudio_data_callback_result_t JNICALL data_callback(
        AAudioStream*, void* user_data, void* audio_data, int32_t num_frames) {
        Music* self = static_cast<Music*>(user_data);
        if (!self || !self->playing_.load() || !audio_data || num_frames <= 0) {
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
        auto* out = static_cast<short*>(audio_data);
        // Silêncio no resto do buffer se o decode terminar antes do callback.
        std::memset(out, 0, static_cast<size_t>(num_frames) * 2 * sizeof(short));
        const int written = self->read_frames(out, num_frames);
        return written > 0 ? AAUDIO_CALLBACK_RESULT_CONTINUE
                           : AAUDIO_CALLBACK_RESULT_STOP;
    }
#endif

    AAudioStream* stream_ = nullptr;
    std::vector<uint8_t> data_;
    std::vector<short> raw_;  // reaproveitado pela data callback
    stb_vorbis* vorbis_ = nullptr;
    int vorbis_error_ = 0;
    drmp3 mp3_{};
    bool mp3_ready_ = false;
    std::atomic<int> source_channels_{1};
    std::atomic<int> channels_{1};
    std::atomic<int> sample_rate_{44100};
    std::atomic<float> volume_{1.0f};
    std::atomic<bool> loop_{false};
    std::atomic<bool> playing_{false};
};

}  // namespace nativeapp
