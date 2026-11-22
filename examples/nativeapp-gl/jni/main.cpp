#include <nativeapp/nativeapp.hpp>

#include <android/log.h>

#define LOG_TAG "nativeapp-gl"
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
    }

    void on_surface_created() override {
        LOGI("on_surface_created: %dx%d", width(), height());
        scene_.init(window());
        scene_.resize(width(), height());
    }

    void on_surface_destroyed() override {
        LOGI("on_surface_destroyed");
        scene_.destroy();
    }

    void on_surface_changed(int w, int h) override {
        LOGI("on_surface_changed: %dx%d", w, h);
        scene_.resize(w, h);
    }

    void on_draw() override {
        // Triângulo segue o dedo / clique.
        const float ox = (touch_x_ / static_cast<float>(width())) * 2.0f - 1.0f;
        const float oy = 1.0f - (touch_y_ / static_cast<float>(height())) * 2.0f;
        scene_.set_offset(ox * 0.3f, oy * 0.3f);
        scene_.render();
    }

    void on_touch(TouchAction action, const std::vector<TouchPoint>& points) override {
        if (!points.empty()) {
            touch_x_ = points[0].x;
            touch_y_ = points[0].y;
        }
    }

    void on_key(int32_t code, bool down) override {
        if (down && code == AKEYCODE_BACK) request_exit();
    }

    GLES2Scene scene_;
    float touch_x_ = 0.0f;
    float touch_y_ = 0.0f;
};

APP_MAIN(MeuApp)
