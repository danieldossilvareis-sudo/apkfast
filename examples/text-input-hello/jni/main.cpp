// Teclado virtual sem EditText — NativeActivity + nativeapp::TextInput.
// O texto digitado aparece no logcat (tag "text-input") e muda a cor da cena.
#include <nativeapp/nativeapp.hpp>
#include <android/log.h>

#define LOG_TAG "text-input"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

class TextApp : public App {
public:
    explicit TextApp(android_app* app) : App(app) {}

private:
    void on_ready() override {
        set_scene(&scene_);
        scene_.set_clear_color(0.05f, 0.05f, 0.10f);

        auto& input = TextInput::instance(nullptr, nullptr);
        input.set_callback([this](const std::string& text, bool done) {
            typed_ = text;
            LOGI("texto='%s' done=%d", text.c_str(), done);
            // Muda a cor conforme o tamanho do texto (feedback visual).
            const float t = static_cast<float>(text.size()) / 20.0f;
            scene_.set_clear_color(0.05f + t * 0.5f, 0.05f, 0.10f + t * 0.3f);
        });
        LOGI("ready");
    }

    void on_surface_created() override { scene_.init(window()); }
    void on_surface_destroyed() override { scene_.destroy(); }
    void on_surface_changed(int w, int h) override { scene_.resize(w, h); }

    void on_draw() override {
        // Cena gerencia begin_frame/present; apenas desenhamos um quadrado.
        Material material;
        Mat4 model = mat4_translate(width() / 2.0f, height() / 2.0f, 0.0f);
        model = mat4_multiply(model, mat4_scale(200.0f, 200.0f, 1.0f));
        scene_.draw(tile_, material, model.data());
    }

    void on_touch(TouchAction action, const std::vector<TouchPoint>&) override {
        if (action != TouchAction::Down) return;
        auto& input = TextInput::instance(nullptr, nullptr);
        if (!input.is_shown()) {
            input.show();
            LOGI("show() -> is_shown=%d", input.is_shown());
        } else {
            input.hide();
        }
    }

    void on_key(int32_t code, bool down) override {
        if (down && code == 4 /* BACK */) {
            auto& input = TextInput::instance(nullptr, nullptr);
            if (input.is_shown()) input.hide();
            else request_exit();
        }
    }

    GLES2Scene scene_;
    MeshData tile_ = MeshData::quad();
    std::string typed_;
};

APP_MAIN(TextApp)
