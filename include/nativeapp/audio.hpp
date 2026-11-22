// nativeapp/audio.hpp — Áudio: AAudio (API 26+) com fallback OpenSLES.
//
// Toca WAV PCM de assets/ com controle de volume/loop. Para música longa use
// OGG via stb_vorbis (não incluído); WAV é suficiente para efeitos e protótipos.

#pragma once

#include <android/log.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <aaudio/AAudio.h>

#include "assets.hpp"

#include <algorithm>
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
            sample_rate == 0 || num_channels < 1 || num_channels > 2) {
            return false;
        }
        out.sample_rate = static_cast<int32_t>(sample_rate);
        out.channels = num_channels;
        out.samples.resize(pcm.size() / 2);
        std::memcpy(out.samples.data(), pcm.data(), out.samples.size() * 2);
        return true;
    }

    /// Toca um buffer uma vez.
    void play(const AudioBuffer& buffer, float volume = 1.0f) {
        if (buffer.empty()) return;
        if (aaudio_stream_) {
            play_aaudio(buffer, volume);
        } else if (sles_player_) {
            play_opensles(buffer, volume);
        }
    }

    /// Define volume 0..1 (apenas OpenSLES; AAudio usa Ganho no buffer).
    void set_volume(float volume) { volume_ = volume < 0 ? 0 : volume > 1 ? 1 : volume; }
    float volume() const { return volume_; }

    bool using_aaudio() const { return aaudio_stream_ != nullptr; }

    void destroy() {
        if (aaudio_stream_) {
            AAudioStream_requestStop(aaudio_stream_);
            AAudioStream_close(aaudio_stream_);
            aaudio_stream_ = nullptr;
        }
        if (sles_player_) { (*sles_player_)->Destroy(sles_player_); sles_player_ = nullptr; }
        if (sles_output_mix_) { (*sles_output_mix_)->Destroy(sles_output_mix_); sles_output_mix_ = nullptr; }
        if (sles_engine_) { (*sles_engine_)->Destroy(sles_engine_); sles_engine_ = nullptr; }
        initialized_ = false;
    }

private:
    Audio() = default;

    static uint16_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
    static uint32_t le32(const uint8_t* p) {
        return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
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
        const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &aaudio_stream_);
        AAudioStreamBuilder_delete(builder);
        return result == AAUDIO_OK && aaudio_stream_ != nullptr;
    }

    void play_aaudio(const AudioBuffer& buffer, float volume) {
        const int frames = static_cast<int>(buffer.samples.size() / buffer.channels);
        // Converte mono -> estéreo se necessário (enviamos 2 canais no stream).
        std::vector<int16_t> output;
        if (buffer.channels == 2) {
            output = buffer.samples;
            if (volume < 1.0f) {
                for (auto& sample : output) {
                    sample = static_cast<int16_t>(sample * volume);
                }
            }
        } else {
            output.resize(frames * 2);
            for (int i = 0; i < frames; ++i) {
                const int16_t s = static_cast<int16_t>(buffer.samples[i] * volume);
                output[i * 2] = s;
                output[i * 2 + 1] = s;
            }
        }
        AAudioStream_requestStart(aaudio_stream_);
        int16_t* frame = output.data();
        int remaining = static_cast<int>(output.size() / 2);
        while (remaining > 0) {
            const aaudio_result_t written = AAudioStream_write(
                aaudio_stream_, frame, remaining, 1000000000LL);
            if (written <= 0) break;
            frame += written * 2;
            remaining -= static_cast<int>(written);
        }
    }
#endif

    bool init_opensles() {
        if (SL_RESULT_SUCCESS != slCreateEngine(&sles_engine_, 0, nullptr, 0, nullptr, nullptr)) return false;
        (*sles_engine_)->Realize(sles_engine_, SL_BOOLEAN_FALSE);
        (*sles_engine_)->GetInterface(sles_engine_, SL_IID_ENGINE, &sles_engine_interface_);

        const SLInterfaceID ids[] = {SL_IID_PLAY};
        const SLboolean req[] = {SL_BOOLEAN_TRUE};
        (*sles_engine_interface_)->CreateOutputMix(
            sles_engine_interface_, &sles_output_mix_, 1, ids, req);
        (*sles_output_mix_)->Realize(sles_output_mix_, SL_BOOLEAN_FALSE);
        return true;
    }

    void play_opensles(const AudioBuffer& buffer, float volume) {
        (void)volume;
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
                sles_engine_interface_, &sles_player_, &source, &sink, 3, ids, req) != SL_RESULT_SUCCESS) {
            return;
        }
        (*sles_player_)->Realize(sles_player_, SL_BOOLEAN_FALSE);
        SLPlayItf play = nullptr;
        SLAndroidSimpleBufferQueueItf queue = nullptr;
        (*sles_player_)->GetInterface(sles_player_, SL_IID_PLAY, &play);
        (*sles_player_)->GetInterface(sles_player_, SL_IID_BUFFERQUEUE, &queue);
        (*queue)->Enqueue(queue, buffer.samples.data(),
                          buffer.samples.size() * sizeof(int16_t));
        (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING);
    }

    bool initialized_ = false;
    float volume_ = 1.0f;
    AAudioStream* aaudio_stream_ = nullptr;
    SLObjectItf sles_engine_ = nullptr;
    SLEngineItf sles_engine_interface_ = nullptr;
    SLObjectItf sles_output_mix_ = nullptr;
    SLObjectItf sles_player_ = nullptr;
};

}  // namespace nativeapp
