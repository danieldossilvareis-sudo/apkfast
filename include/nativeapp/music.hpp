// nativeapp/music.hpp — Música em streaming (OGG/MP3) sem carregar PCM na memória.
//
// Usa um stream AAudio separado com data callback. O decoder (stb_vorbis ou
// dr_mp3) roda na thread de áudio, preenchendo o buffer de saída em chunks.
// Requer API 26+ (AAudio). Em API 21-25 use Audio::load + play (decode completo).

#pragma once

#include "assets.hpp"
#include "codecs.hpp"

#include <aaudio/AAudio.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
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
        auto& assets = Assets::instance(nullptr);
        if (!assets.manager()) return false;
        data_ = assets.read(path);
        if (data_.empty()) return false;

        const std::string lower = lowercase(path);
        const bool is_ogg = lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".ogg") == 0;
        const bool is_mp3 = lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".mp3") == 0;
        if (!is_ogg && !is_mp3) return false;

        if (is_ogg) {
            vorbis_ = stb_vorbis_open_memory(
                data_.data(), static_cast<int>(data_.size()), &vorbis_error_, nullptr);
            if (!vorbis_) return false;
            const stb_vorbis_info info = stb_vorbis_get_info(vorbis_);
            channels_ = info.channels;
            sample_rate_ = static_cast<int>(info.sample_rate);
        } else {
            if (!drmp3_init_memory(&mp3_, data_.data(), data_.size(), nullptr)) return false;
            channels_ = static_cast<int>(mp3_.channels);
            sample_rate_ = static_cast<int>(mp3_.sampleRate);
            mp3_ready_ = true;
        }
        source_channels_ = channels_;
        loop_ = false;
        playing_ = false;
        return true;
    }

    /// Inicia a reprodução. Chama init() internamente se necessário.
    bool play(bool loop = false) {
        if (data_.empty()) return false;
        if (playing_) return true;
        loop_ = loop;

        if (!create_stream()) return false;
        playing_ = true;
        AAudioStream_requestStart(stream_);
        return true;
    }

    void pause() {
        if (stream_ && playing_) AAudioStream_requestPause(stream_);
        playing_ = false;
    }

    void stop() {
        playing_ = false;
        if (stream_) {
            AAudioStream_requestStop(stream_);
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
        if (vorbis_) { stb_vorbis_close(vorbis_); vorbis_ = nullptr; }
        if (mp3_ready_) { drmp3_uninit(&mp3_); mp3_ready_ = false; }
        data_.clear();
    }

    void set_volume(float value) {
        volume_ = value < 0 ? 0 : value > 1 ? 1 : value;
    }
    float volume() const { return volume_; }
    bool is_playing() const { return playing_; }
    bool is_looping() const { return loop_; }
    void set_loop(bool value) { loop_ = value; }

    int sample_rate() const { return sample_rate_; }
    int channels() const { return source_channels_; }

private:
    static std::string lowercase(std::string value) {
        for (char& c : value) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return value;
    }

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
        const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream_);
        AAudioStreamBuilder_delete(builder);
        return result == AAUDIO_OK && stream_ != nullptr;
    }

    /// Subsample/oversample linear para converter sample_rate -> 44100.
    int read_frames(short* output, int frames) {
        // Busca no sample rate original ( aprox. )
        const int source_frames = static_cast<int>(
            static_cast<double>(frames) * sample_rate_ / 44100.0);
        if (source_frames <= 0) return 0;

        std::vector<short> raw(static_cast<size_t>(source_frames) * source_channels_);
        int got = 0;
        if (vorbis_) {
            got = stb_vorbis_get_samples_short_interleaved(
                vorbis_, source_channels_, raw.data(), source_frames);
        } else if (mp3_ready_) {
            got = static_cast<int>(drmp3_read_pcm_frames_s16(
                &mp3_, source_frames, raw.data()));
        }
        if (got <= 0) {
            if (!loop_) return 0;
            rewind();
            return read_frames(output, frames);
        }

        // Converte para 2 canais e resample linear simples.
        for (int i = 0; i < frames; ++i) {
            const double pos = static_cast<double>(i) * got / frames;
            const int idx = static_cast<int>(pos);
            const int next = idx + 1 < got ? idx + 1 : idx;
            const float t = static_cast<float>(pos - idx);
            for (int ch = 0; ch < 2; ++ch) {
                const int src_ch = source_channels_ == 1 ? 0 : ch;
                const float a = static_cast<float>(raw[idx * source_channels_ + src_ch]);
                const float b = static_cast<float>(raw[next * source_channels_ + src_ch]);
                output[i * 2 + ch] = static_cast<short>((a * (1 - t) + b * t) * volume_);
            }
        }
        return frames;
    }

    void rewind() {
        if (vorbis_) stb_vorbis_seek_start(vorbis_);
        else if (mp3_ready_) drmp3_seek_to_pcm_frame(&mp3_, 0);
    }

    static aaudio_data_callback_result_t JNICALL data_callback(
        AAudioStream*, void* user_data, void* audio_data, int32_t num_frames) {
        Music* self = static_cast<Music*>(user_data);
        if (!self || !self->playing_) return AAUDIO_CALLBACK_RESULT_STOP;
        auto* out = static_cast<short*>(audio_data);
        const int written = self->read_frames(out, num_frames);
        if (written <= 0) return AAUDIO_CALLBACK_RESULT_STOP;
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    AAudioStream* stream_ = nullptr;
    std::vector<uint8_t> data_;
    stb_vorbis* vorbis_ = nullptr;
    int vorbis_error_ = 0;
    drmp3 mp3_{};
    bool mp3_ready_ = false;
    int source_channels_ = 1;
    int channels_ = 1;
    int sample_rate_ = 44100;
    float volume_ = 1.0f;
    bool loop_ = false;
    std::atomic<bool> playing_{false};
};

}  // namespace nativeapp
