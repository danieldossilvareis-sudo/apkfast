// nativeapp/assets.hpp — Leitura de arquivos em assets/ via AAssetManager.

#pragma once

#include <android/asset_manager.h>
#include <android/native_activity.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nativeapp {

class Assets {
public:
    static Assets& instance(ANativeActivity* activity) {
        static Assets* singleton = nullptr;
        if (!singleton) singleton = new Assets(activity);
        return *singleton;
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
        const size_t size = static_cast<size_t>(AAsset_getLength64(asset));
        result.resize(size);
        if (size > 0) {
            AAsset_read(asset, result.data(), size);
        }
        AAsset_close(asset);
        return result;
    }

    std::string read_text(const std::string& path) const {
        const auto data = read(path);
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
