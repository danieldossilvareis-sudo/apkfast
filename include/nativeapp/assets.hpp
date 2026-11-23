// nativeapp/assets.hpp — Leitura de arquivos em assets/ via AAssetManager.

#pragma once

#include <android/asset_manager.h>
#include <android/native_activity.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace nativeapp {

class Assets {
public:
    static Assets& instance(ANativeActivity* activity) {
        // Inicialização local static é thread-safe em C++11+ e o objeto é
        // destruído no shutdown (o ponteiro new vazava para sempre).
        // Como antes, o activity só é usado na primeira chamada.
        static Assets singleton(activity);
        return singleton;
    }

    void set_activity(ANativeActivity* activity) {
        if (activity) manager_ = activity->assetManager;
    }

    bool exists(const std::string& path) const {
        if (!manager_) return false;
        AAsset* asset = AAssetManager_open(manager_, path.c_str(), AASSET_MODE_BUFFER);
        if (!asset) return false;
        AAsset_close(asset);
        return true;
    }

    std::vector<uint8_t> read(const std::string& path) const {
        std::vector<uint8_t> result;
        if (!manager_) return result;
        AAsset* asset = AAssetManager_open(manager_, path.c_str(), AASSET_MODE_BUFFER);
        if (!asset) return result;
        const int64_t length = AAsset_getLength64(asset);
        if (length < 0 ||
            static_cast<uint64_t>(length) >
                static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            AAsset_close(asset);
            return result;
        }
        const size_t size = static_cast<size_t>(length);
        if (size == 0) {
            AAsset_close(asset);
            return result;
        }
        result.resize(size);
        // AAsset_read pode devolver menos bytes que o pedido; completa o
        // buffer em vez de devolver um arquivo truncado. Qualquer falha
        // ainda resulta em vetor vazio (comportamento histórico).
        size_t total = 0;
        while (total < size) {
            const int bytes = AAsset_read(asset, result.data() + total,
                                          size - total);
            if (bytes <= 0) {
                result.clear();
                break;
            }
            total += static_cast<size_t>(bytes);
        }
        AAsset_close(asset);
        return result;
    }

    std::string read_text(const std::string& path) const {
        const auto data = read(path);
        if (data.empty()) return {};
        return std::string(reinterpret_cast<const char*>(data.data()), data.size());
    }

    bool write(const std::string&, const std::vector<uint8_t>&) const {
        return false;  // assets são somente leitura
    }

    AAssetManager* manager() const { return manager_; }

private:
    explicit Assets(ANativeActivity* activity)
        : manager_(activity ? activity->assetManager : nullptr) {}

    AAssetManager* manager_ = nullptr;
};

}  // namespace nativeapp
