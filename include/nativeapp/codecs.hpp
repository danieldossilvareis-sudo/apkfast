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
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace nativeapp {

class Codecs {
public:
    /// Decodifica um arquivo de assets/ automaticamente pela extensão.
    /// Suporta .wav (via Audio::load_wav), .ogg e .mp3.
    static bool decode(const std::string& path, AudioBuffer& out) {
        const std::string lower = lowercase(path);
        const bool is_wav = has_extension(lower, ".wav");
        const bool is_ogg = has_extension(lower, ".ogg");
        const bool is_mp3 = has_extension(lower, ".mp3");
        if (!is_wav && !is_ogg && !is_mp3) return false;
        if (is_wav) return Audio::instance().load_wav(path, out);

        auto& assets = Assets::instance(nullptr);
        if (!assets.manager()) return false;
        const auto data = assets.read(path);
        if (data.empty()) return false;

        return is_ogg ? decode_ogg(data, out) : decode_mp3(data, out);
    }

    static bool decode_ogg(const std::vector<uint8_t>& data, AudioBuffer& out) {
        if (data.empty()) return false;
        int channels = 0, sample_rate = 0;
        short* pcm = nullptr;
        const int frames = stb_vorbis_decode_memory(
            data.data(), static_cast<int>(data.size()),
            &channels, &sample_rate, &pcm);
        // Liberado em todos os caminhos (inclusive se assign() lançar).
        std::unique_ptr<short, void (*)(void*)> guard(pcm, &std::free);
        if (frames <= 0 || !pcm || channels < 1 || sample_rate <= 0) return false;
        if (static_cast<size_t>(channels) > SIZE_MAX / static_cast<size_t>(frames)) return false;

        const size_t count = static_cast<size_t>(frames) * static_cast<size_t>(channels);
        out.channels = channels;
        out.sample_rate = sample_rate;
        out.samples.assign(guard.get(), guard.get() + count);
        return true;
    }

    static bool decode_mp3(const std::vector<uint8_t>& data, AudioBuffer& out) {
        if (data.empty()) return false;
        drmp3_config config{};
        drmp3_uint64 frames = 0;
        drmp3_int16* pcm = drmp3_open_memory_and_read_pcm_frames_s16(
            data.data(), data.size(), &config, &frames, nullptr);
        const auto deleter = [](drmp3_int16* p) { drmp3_free(p, nullptr); };
        std::unique_ptr<drmp3_int16, decltype(deleter)> guard(pcm, deleter);
        if (!pcm || frames == 0 || config.channels == 0 || config.sampleRate == 0) return false;
        if (static_cast<drmp3_uint64>(config.channels) > SIZE_MAX / frames) return false;

        const size_t count = static_cast<size_t>(frames) * config.channels;
        out.channels = static_cast<int>(config.channels);
        out.sample_rate = static_cast<int>(config.sampleRate);
        out.samples.assign(guard.get(), guard.get() + count);
        return true;
    }

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
};

/// Implementação de Audio::load — definida aqui porque depende de Codecs.
inline bool Audio::load(const std::string& path, AudioBuffer& out) const {
    return Codecs::decode(path, out);
}

}  // namespace nativeapp
