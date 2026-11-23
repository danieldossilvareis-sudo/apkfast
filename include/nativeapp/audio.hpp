// nativeapp/audio.hpp — Áudio: AAudio (API 26+) com fallback OpenSLES.
//
// Toca WAV PCM de assets/ com controle de volume/loop. Para música longa use
// OGG via stb_vorbis (implementado em codecs.hpp/music.hpp).

#pragma once

#include <android/log.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <aaudio/AAudio.h>

#include "assets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nativeapp {

struct AudioBuffer {
    std::vector<int16_t> samples;
    int32_t sample_rate = 44100;
    int channels = 1;

    bool empty() const { return samples.empty(); }
};

class Audio {
public:
    ~Audio() { destroy(); }

    static Audio& instance() {
        static Audio singleton;
        return singleton;
    }

    /// Inicializa o backend. AAudio se API>=26 e disponível, senão OpenSLES.
    bool init() {
        if (initialized_) return true;
#if __ANDROID_API__ >= 26
        if (init_aaudio()) { initialized_ = true; return true; }
        __android_log_print(ANDROID_LOG_INFO, "nativeapp-audio",
                            "AAudio indisponivel, usando OpenSLES");
#endif
        if (init_opensles()) { initialized_ = true; return true; }
        return false;
    }

    /// Carrega WAV PCM de assets/.
    bool load_wav(const std::string& path, AudioBuffer& out) const {
        auto& assets = Assets::instance(nullptr);
        if (!assets.manager()) return false;
        const auto data = assets.read(path);
        if (data.size() < 44) return false;

        // Cabeçalho RIFF/WAVE
        if (std::memcmp(data.data(), "RIFF", 4) != 0 ||
            std::memcmp(data.data() + 8, "WAVE", 4) != 0) return false;

        size_t offset = 12;
        uint16_t audio_format = 0, num_channels = 0, bits_per_sample = 0;
        uint32_t sample_rate = 0;
        std::vector<uint8_t> pcm;

        while (offset + 8 <= data.size()) {
            const char* chunk_id = reinterpret_cast<const char*>(data.data() + offset);
            const uint32_t chunk_size = le32(data.data() + offset + 4);
            // Valida o tamanho do chunk antes de qualquer leitura (evita OOB
            // e wrap com valores corrompidos próximos de UINT32_MAX).
            if (static_cast<size_t>(chunk_size) > data.size() - offset - 8) {
                return false;
            }
            const uint8_t* payload = data.data() + offset + 8;
            if (std::memcmp(chunk_id, "fmt ", 4) == 0 && chunk_size >= 16) {
                audio_format = le16(payload);
                num_channels = le16(payload + 2);
                sample_rate = le32(payload + 4);
                bits_per_sample = le16(payload + 14);
            } else if (std::memcmp(chunk_id, "data", 4) == 0) {
                pcm.assign(payload, payload + chunk_size);
            }
            const size_t advance = 8 + static_cast<size_t>(chunk_size) + (chunk_size & 1);
            if (advance > data.size() - offset) break;
            offset += advance;
        }

        if (audio_format != 1 || bits_per_sample != 16 || pcm.empty() ||
            sample_rate == 0 || sample_rate > 0x7fffffffu ||
            num_channels < 1 || num_channels > 2) {
            return false;
        }
        out.sample_rate = static_cast<int32_t>(sample_rate);
        out.channels = num_channels;
        out.samples.resize(pcm.size() / 2);
        std::memcpy(out.samples.data(), pcm.data(), out.samples.size() * 2);
        return true;
    }

    /// Carrega qualquer formato suportado de assets/ (.wav/.ogg/.mp3).
    /// Implementado em codecs.hpp (incluído abaixo para evitar dependência
    /// circular — AudioBuffer já está definido aqui).
    bool load(const std::string& path, AudioBuffer& out) const;

    /// Toca um buffer uma vez. O ganho final é `volume` × set_volume().
    void play(const AudioBuffer& buffer, float volume = 1.0f) {
        if (buffer.empty() || buffer.channels < 1 || buffer.channels > 2 ||
            buffer.sample_rate <= 0 ||
            buffer.samples.size() % static_cast<size_t>(buffer.channels) != 0) {
            return;
        }
#if __ANDROID_API__ >= 26
        if (aaudio_stream_) {
            play_aaudio(buffer, volume);
            return;
        }
#endif
        if (sles_player_) play_opensles(buffer, volume);
    }

    /// Define volume global 0..1; multiplica o volume por chamada de play().
    void set_volume(float volume) {
        volume_ = std::isfinite(volume) ? std::max(0.0f, std::min(1.0f, volume)) : 1.0f;
    }
    float volume() const { return volume_; }

    bool using_aaudio() const { return aaudio_stream_ != nullptr; }

    void destroy() {
#if __ANDROID_API__ >= 26
        if (aaudio_stream_) {
            AAudioStream_requestStop(aaudio_stream_);
            AAudioStream_close(aaudio_stream_);
            aaudio_stream_ = nullptr;
        }
#endif
        destroy_opensles();
        sles_samples_.clear();
        initialized_ = false;
    }

private:
    Audio() = default;

    static uint16_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
    static uint32_t le32(const uint8_t* p) {
        return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    }

    static float clamp01(float v) {
        return std::isfinite(v) ? std::max(0.0f, std::min(1.0f, v)) : 1.0f;
    }

    float gain_for(float requested) const {
        return clamp01(requested) * clamp01(volume_);
    }

    static int16_t scale_sample(int32_t sample, float gain) {
        const float value = static_cast<float>(sample) * gain;
        if (value >= 32767.0f) return 32767;
        if (value <= -32768.0f) return -32768;
        return static_cast<int16_t>(value);
    }

#if __ANDROID_API__ >= 26
    bool init_aaudio() {
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) return false;
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setSampleRate(builder, 44100);
        AAudioStream* stream = nullptr;
        const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK || !stream) {
            if (stream) AAudioStream_close(stream);
            aaudio_stream_ = nullptr;
            return false;
        }
        aaudio_stream_ = stream;
        return true;
    }

    void play_aaudio(const AudioBuffer& buffer, float volume) {
        const size_t channels = static_cast<size_t>(buffer.channels);
        const size_t frames = buffer.samples.size() / channels;
        if (frames == 0) return;

        const float gain = gain_for(volume);
        // O stream é fixo em 44100 Hz / 2 canais: interpola e converte
        // mono -> estéreo (antes o WAV tocava desafinado se não fosse 44.1k).
        const double step = static_cast<double>(buffer.sample_rate) / 44100.0;
        const double out_frames_d = std::ceil(static_cast<double>(frames) / step);
        if (out_frames_d <= 0.0 || out_frames_d > static_cast<double>(SIZE_MAX / 2)) return;
        const size_t out_frames = static_cast<size_t>(out_frames_d);

        std::vector<int16_t> output(out_frames * 2);
        for (size_t i = 0; i < out_frames; ++i) {
            const double position = static_cast<double>(i) * step;
            size_t idx = static_cast<size_t>(position);
            if (idx >= frames) idx = frames - 1;
            const size_t next = idx + 1 < frames ? idx + 1 : idx;
            const float t = static_cast<float>(position - static_cast<double>(idx));
            for (size_t ch = 0; ch < 2; ++ch) {
                const size_t src_ch = channels == 1 ? 0 : ch;
                const int32_t a = buffer.samples[idx * channels + src_ch];
                const int32_t b = buffer.samples[next * channels + src_ch];
                const int32_t value = static_cast<int32_t>(a + (b - a) * t);
                output[i * 2 + ch] = scale_sample(value, gain);
            }
        }

        const aaudio_result_t started = AAudioStream_requestStart(aaudio_stream_);
        if (started != AAUDIO_OK && started != AAUDIO_ERROR_INVALID_STATE) return;
        const int16_t* frame = output.data();
        size_t remaining = output.size() / 2;
        while (remaining > 0) {
            const aaudio_result_t written = AAudioStream_write(
                aaudio_stream_, frame, static_cast<int32_t>(remaining), 1000000000LL);
            if (written <= 0) break;
            frame += static_cast<size_t>(written) * 2;
            remaining -= static_cast<size_t>(written);
        }
    }
#endif

    bool init_opensles() {
        if (slCreateEngine(&sles_engine_, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS ||
            !sles_engine_) {
            destroy_opensles();
            return false;
        }
        if ((*sles_engine_)->Realize(sles_engine_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS ||
            (*sles_engine_)->GetInterface(sles_engine_, SL_IID_ENGINE, &sles_engine_interface_)
                != SL_RESULT_SUCCESS || !sles_engine_interface_) {
            destroy_opensles();
            return false;
        }

        const SLInterfaceID ids[] = {SL_IID_PLAY};
        const SLboolean req[] = {SL_BOOLEAN_TRUE};
        if ((*sles_engine_interface_)->CreateOutputMix(
                sles_engine_interface_, &sles_output_mix_, 1, ids, req)
                != SL_RESULT_SUCCESS || !sles_output_mix_) {
            destroy_opensles();
            return false;
        }
        if ((*sles_output_mix_)->Realize(sles_output_mix_, SL_BOOLEAN_FALSE)
                != SL_RESULT_SUCCESS) {
            destroy_opensles();
            return false;
        }
        return true;
    }

    void destroy_opensles() {
        if (sles_player_) {
            (*sles_player_)->Destroy(sles_player_);
            sles_player_ = nullptr;
        }
        if (sles_output_mix_) {
            (*sles_output_mix_)->Destroy(sles_output_mix_);
            sles_output_mix_ = nullptr;
        }
        if (sles_engine_) {
            (*sles_engine_)->Destroy(sles_engine_);
            sles_engine_ = nullptr;
        }
        sles_engine_interface_ = nullptr;
    }

    void play_opensles(const AudioBuffer& buffer, float volume) {
        if (!sles_engine_interface_ || buffer.samples.empty()) return;
        // Aplica ganho em uma cópia: funciona independentemente de suporte a
        // SLVolumeItf no device e mantém o buffer original intacto.
        sles_samples_ = buffer.samples;  // mantém vivo até o player terminar
        const float gain = gain_for(volume);
        if (gain < 1.0f) {
            for (auto& sample : sles_samples_) {
                sample = scale_sample(sample, gain);
            }
        }
        SLDataLocator_AndroidSimpleBufferQueue locator{
            SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1};
        SLDataFormat_PCM format{
            SL_DATAFORMAT_PCM, static_cast<SLuint32>(buffer.channels),
            static_cast<SLuint32>(buffer.sample_rate * 1000), SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_PCMSAMPLEFORMAT_FIXED_16,
            buffer.channels == 2 ? (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT)
                                 : SL_SPEAKER_FRONT_CENTER,
            SL_BYTEORDER_LITTLEENDIAN};

        SLDataSource source{&locator, &format};
        // O locator de saída é uma struct, não o ponteiro do objeto.
        SLDataLocator_OutputMix output_locator{
            SL_DATALOCATOR_OUTPUTMIX, sles_output_mix_};
        SLDataSink sink{&output_locator, nullptr};

        const SLInterfaceID ids[] = {SL_IID_PLAY, SL_IID_BUFFERQUEUE, SL_IID_VOLUME};
        const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
        if (sles_player_) { (*sles_player_)->Destroy(sles_player_); sles_player_ = nullptr; }
        if ((*sles_engine_interface_)->CreateAudioPlayer(
                sles_engine_interface_, &sles_player_, &source, &sink, 3, ids, req)
                != SL_RESULT_SUCCESS || !sles_player_) {
            sles_player_ = nullptr;
            return;
        }
        if ((*sles_player_)->Realize(sles_player_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS)
            return;
        SLPlayItf play = nullptr;
        SLAndroidSimpleBufferQueueItf queue = nullptr;
        if ((*sles_player_)->GetInterface(sles_player_, SL_IID_PLAY, &play) != SL_RESULT_SUCCESS ||
            (*sles_player_)->GetInterface(sles_player_, SL_IID_BUFFERQUEUE, &queue)
                != SL_RESULT_SUCCESS || !play || !queue) {
            return;
        }
        if ((*queue)->Enqueue(queue, sles_samples_.data(),
                              sles_samples_.size() * sizeof(int16_t)) == SL_RESULT_SUCCESS) {
            (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING);
        }
    }

    bool initialized_ = false;
    float volume_ = 1.0f;
    std::vector<int16_t> sles_samples_;
    AAudioStream* aaudio_stream_ = nullptr;
    SLObjectItf sles_engine_ = nullptr;
    SLEngineItf sles_engine_interface_ = nullptr;
    SLObjectItf sles_output_mix_ = nullptr;
    SLObjectItf sles_player_ = nullptr;
};

// A implementação de Audio::load está em codecs.hpp (evita ciclo de include).

}  // namespace nativeapp
