// v2.2.0 image demo: WebP via BitmapFactory e SVG via NanoSVG.
#include <nativeapp/nativeapp.hpp>
#include <android/log.h>

#define LOG_TAG "image-demo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

class ImageApp : public App {
public:
    explicit ImageApp(android_app* app) : App(app) {}

private:
    void on_ready() override {
        set_scene(&scene_);
        scene_.set_clear_color(0.06f, 0.07f, 0.11f);
    }

    void on_surface_created() override {
        scene_.init(window());

        //Magic bytes decidem o decoder; load_webp_asset/load_svg_asset são aliases.
        webp_ok_ = webp_.load_webp_asset("sprite.webp");
        svg_ok_ = svg_.load_svg_asset("logo.svg", 256, 256);
        LOGI("webp=%s %dx%d svg=%s %dx%d",
             webp_ok_ ? "OK" : "FALHOU", webp_.width(), webp_.height(),
             svg_ok_ ? "OK" : "FALHOU", svg_.width(), svg_.height());
    }

    void on_surface_destroyed() override { scene_.destroy(); }
    void on_surface_changed(int w, int h) override { scene_.resize(w, h); }

    void on_draw() override {
        Material mat;
        mat.texture = "sprite.webp";
        Mat4 left = mat4_translate(width() * 0.30f, height() * 0.5f, 0);
        left = mat4_multiply(left, mat4_scale(260, 260, 1));
        scene_.draw(quad_, mat, left.data());

        mat.texture = "logo.svg";
        Mat4 right = mat4_translate(width() * 0.70f, height() * 0.5f, 0);
        right = mat4_multiply(right, mat4_scale(260, 260, 1));
        scene_.draw(quad_, mat, right.data());
    }

    void on_key(int32_t code, bool down) override {
        if (down && code == 4) request_exit();  // BACK
    }

    GLES2Scene scene_;
    MeshData quad_ = MeshData::quad();
    Texture webp_;
    Texture svg_;
    bool webp_ok_ = false;
    bool svg_ok_ = false;
};

APP_MAIN(ImageApp)
