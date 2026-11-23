// nativeapp/codecs.hpp — Decodificação de OGG/MP3/WAV para AudioBuffer.
//
// As implementações (stb_vorbis, dr_mp3) vivem em src/nativeapp_impl.cpp,
// injetado automaticamente pelo builder. Aqui só declaramos/usamos as APIs.

#pragma once

#include "assets.hpp"
#include "audio.hpp"

#include <nativeapp/third_party/dr_mp3.h>
#include <nativeapp/third_party/stb_vorbis.h>

#include <cstdlib>
#include <vector>

namespace nativeapp {

class Codecs {
public:
    /// Decodifica um arquivo de assets/ automaticamente pela extensão.
    /// Suporta .wav (via Audio::load_wav), .ogg e .mp3.
    static bool decode(const std::string& path, AudioBuffer& out) {
        auto& assets = Assets::instance(nullptr);
        if (!assets.manager()) return false;
        const auto data = assets.read(path);
        if (data.empty()) return false;

        const std::string lower = lowercase(path);
        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".wav") == 0) {
            return Audio::instance().load_wav(path, out);
        }
        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".ogg") == 0) {
            return decode_ogg(data, out);
        }
        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".mp3") == 0) {
            return decode_mp3(data, out);
        }
        return false;
    }

    static bool decode_ogg(const std::vector<uint8_t>& data, AudioBuffer& out) {
        int channels = 0, sample_rate = 0;
        short* pcm = nullptr;
        const int frames = stb_vorbis_decode_memory(
            data.data(), static_cast<int>(data.size()),
            &channels, &sample_rate, &pcm);
        if (frames <= 0 || !pcm) return false;

        out.channels = channels;
        out.sample_rate = sample_rate;
        out.samples.assign(pcm, pcm + static_cast<size_t>(frames) * channels);
        std::free(pcm);  // veio de malloc interno do stb_vorbis
        return true;
    }

    static bool decode_mp3(const std::vector<uint8_t>& data, AudioBuffer& out) {
        drmp3_config config{};
        drmp3_uint64 frames = 0;
        drmp3_int16* pcm = drmp3_open_memory_and_read_pcm_frames_s16(
            data.data(), data.size(), &config, &frames, nullptr);
        if (!pcm || frames == 0) {
            if (pcm) drmp3_free(pcm, nullptr);
            return false;
        }
        out.channels = static_cast<int>(config.channels);
        out.sample_rate = static_cast<int>(config.sampleRate);
        out.samples.assign(pcm, pcm + static_cast<size_t>(frames) * config.channels);
        drmp3_free(pcm, nullptr);
        return true;
    }

private:
    static std::string lowercase(std::string value) {
        for (char& c : value) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return value;
    }
};

/// Implementação de Audio::load — definida aqui porque depende de Codecs.
inline bool Audio::load(const std::string& path, AudioBuffer& out) const {
    return Codecs::decode(path, out);
}

}  // namespace nativeapp
