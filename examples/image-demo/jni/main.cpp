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
        font_ok_ = svg_register_font_asset("DejaVu Sans", "font.ttf");
        svg_register_font_asset("sans-serif", "font.ttf");
        svg_register_font_asset("Arial", "font.ttf");
        svg_ok_ = svg_.load_svg_asset("logo.svg", 256, 256);
        anim_ok_ = anim_.open_asset("animated.webp");
        if (anim_ok_) {
            const auto& first = anim_.current_frame();
            anim_tex_.load_rgba(first.rgba.data(), first.width, first.height);
        }
        LOGI("webp=%s %dx%d svg=%s %dx%d font=%s anim=%s frames=%zu loop=%d",
             webp_ok_ ? "OK" : "FALHOU", webp_.width(), webp_.height(),
             svg_ok_ ? "OK" : "FALHOU", svg_.width(), svg_.height(),
             font_ok_ ? "OK" : "FALHOU",
             anim_ok_ ? "OK" : "FALHOU", anim_.frame_count(), anim_.loop_count());
    }

    void on_surface_destroyed() override { scene_.destroy(); }
    void on_surface_changed(int w, int h) override { scene_.resize(w, h); }

    void on_update(float dt) override {
        if (!anim_ok_) return;
        if (anim_.update(dt * 1000.0f)) {
            const auto& frame = anim_.current_frame();
            anim_tex_.load_rgba(frame.rgba.data(), frame.width, frame.height);
            LOGI("anim frame=%zu/%zu", anim_.current_index() + 1, anim_.frame_count());
        }
    }

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

        if (anim_ok_) {
            Material anim_mat;
            anim_mat.depth_test = false;
            anim_mat.color[3] = 1.0f;
            Mat4 center = mat4_translate(width() * 0.5f, height() * 0.16f, 0);
            center = mat4_multiply(center, mat4_scale(160, 160, 1));
            scene_.draw_with_texture(quad_, anim_mat, center.data(), &anim_tex_);
        }
    }

    void on_key(int32_t code, bool down) override {
        if (down && code == 4) request_exit();  // BACK
    }

    GLES2Scene scene_;
    MeshData quad_ = MeshData::quad();
    Texture webp_;
    Texture svg_;
    Texture anim_tex_;
    AnimatedImage anim_;
    bool webp_ok_ = false;
    bool svg_ok_ = false;
    bool anim_ok_ = false;
    bool font_ok_ = false;
};

APP_MAIN(ImageApp)
