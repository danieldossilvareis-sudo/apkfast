// v2.0.0 media demo: fonte TTF, textura PNG, WAV, OGG, MP3 e música em stream.
#include <nativeapp/nativeapp.hpp>
#include <android/log.h>

#define LOG_TAG "media-demo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

class MediaApp : public App {
public:
    explicit MediaApp(android_app* app) : App(app) {}

private:
    void on_ready() override {
        set_scene(&scene_);
        scene_.set_clear_color(0.04f, 0.05f, 0.09f);
        LOGI("ready");
    }

    void on_surface_created() override {
        scene_.init(window());
        scene_.load_font("font.ttf", 28);
        sprite_.load("sprite.png");
        LOGI("sprite=%dx%d font=%d", sprite_.width(), sprite_.height(), scene_.has_font());

        // Efeitos curtos: decode completo.
        Audio::instance().init();
        if (Audio::instance().load("beep.wav", beep_)) {
            LOGI("wav: %dch %dHz %zu samples", beep_.channels, beep_.sample_rate,
                 beep_.samples.size());
        }
        if (Audio::instance().load("tone.ogg", ogg_)) {
            LOGI("ogg: %dch %dHz %zu samples", ogg_.channels, ogg_.sample_rate,
                 ogg_.samples.size());
        }
        if (Audio::instance().load("tone.mp3", mp3_)) {
            LOGI("mp3: %dch %dHz %zu samples", mp3_.channels, mp3_.sample_rate,
                 mp3_.samples.size());
        }

        // Música em streaming (AAudio callback, sem PCM inteiro na memória).
        if (music_.open("tone.mp3")) {
            LOGI("music open: %dch %dHz", music_.channels(), music_.sample_rate());
            music_.set_loop(true);
            music_.set_volume(0.35f);
            music_.play(true);
        } else {
            LOGI("music indisponível nesta API (requer AAudio/API26+)");
        }
    }

    void on_surface_destroyed() override {
        music_.stop();
        scene_.destroy();
    }

    void on_surface_changed(int w, int h) override { scene_.resize(w, h); }

    void on_draw() override {
        Material mat;
        mat.texture = "sprite.png";
        Mat4 quad = mat4_translate(width() * 0.5f, height() * 0.30f, 0);
        quad = mat4_multiply(quad, mat4_scale(220, 220, 1));
        scene_.draw(sprite_mesh_, mat, quad.data());

        scene_.draw_text("apkfast v2.0.0", 40, 120, 1.6f, 0.37f, 0.79f, 0.44f);
        scene_.draw_text("font TTF + PNG + WAV + OGG + MP3", 40, 200, 1.0f);
        scene_.draw_text(music_.is_playing() ? "musica: PLAYING (loop)"
                                             : "musica: parada",
                         40, 260, 1.0f, 0.95f, 0.85f, 0.3f);
        scene_.draw_text("toque: beep | BACK: sair", 40, height() - 120, 0.9f,
                         0.6f, 0.6f, 0.6f);
        (void)sprite_;
    }

    void on_touch(TouchAction action, const std::vector<TouchPoint>&) override {
        if (action == TouchAction::Down) {
            Audio::instance().play(beep_, 0.8f);
        }
    }

    void on_key(int32_t code, bool down) override {
        if (!down) return;
        if (code == 4) request_exit();
        if (code == 66 /* ENTER */) Audio::instance().play(ogg_);
        if (code == 62 /* SPACE */) Audio::instance().play(mp3_);
    }

    GLES2Scene scene_;
    MeshData sprite_mesh_ = MeshData::quad();
    Texture sprite_;
    AudioBuffer beep_, ogg_, mp3_;
    Music music_;
};

APP_MAIN(MediaApp)
