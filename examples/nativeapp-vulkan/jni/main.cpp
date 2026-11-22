#include <nativeapp/nativeapp.hpp>

#include <android/log.h>

#define LOG_TAG "nativeapp-vk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

class MeuApp : public App {
public:
    explicit MeuApp(android_app* app) : App(app), scene_() {}

private:
    void on_ready() override {
        LOGI("on_ready");

        auto& perms = Permissions::instance(activity());
        perms.request_async({"android.permission.VIBRATE"},
            [](const PermissionResult& result) {
                if (result.all_granted) {
                    LOGI("Permissão concedida!");
                } else {
                    LOGI("Permissão negada (%zu)", result.denied.size());
                }
            });

        // Device Vulkan é criado uma única vez.
        scene_.init(activity());
        scene_.set_clear_color(0.1f, 0.2f, 0.35f, 1.0f);
    }

    void on_surface_created() override {
        LOGI("on_surface_created: %dx%d", width(), height());
        // Swapchain é (re)criada sempre que a janela volta.
        scene_.attach(window());
    }

    void on_surface_destroyed() override {
        LOGI("on_surface_destroyed");
        scene_.invalidate();
    }

    void on_surface_changed(int w, int h) override {
        LOGI("on_surface_changed: %dx%d", w, h);
        scene_.resize(w, h);
    }

    void on_pause() override { LOGI("on_pause"); }
    void on_resume() override { LOGI("on_resume"); }

    void on_draw() override { scene_.render(); }

    void on_key(int32_t code, bool down) override {
        if (down && code == AKEYCODE_BACK) request_exit();
    }

    VulkanScene scene_;
};

APP_MAIN(MeuApp)
