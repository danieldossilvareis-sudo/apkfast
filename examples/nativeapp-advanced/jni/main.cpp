// Demonstra a API gráfica avançada do nativeapp:
// MeshData, Material, matrizes, on_update(dt), input helpers e Scene gerenciada.
#include <nativeapp/nativeapp.hpp>

#include <android/log.h>

#define LOG_TAG "advanced"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

class AdvancedApp : public App {
public:
    explicit AdvancedApp(android_app* app) : App(app) {}

private:
    void on_ready() override {
        LOGI("on_ready - GLES%d", scene_.egl().version());
        set_scene(&scene_);  // App chama begin_frame()/present() automaticamente
        scene_.set_clear_color(0.05f, 0.05f, 0.1f, 1.0f);
    }

    void on_surface_created() override {
        LOGI("on_surface_created %dx%d", width(), height());
        scene_.init(window());
    }

    void on_surface_destroyed() override { scene_.destroy(); }

    void on_surface_changed(int w, int h) override {
        LOGI("on_surface_changed %dx%d", w, h);
        scene_.resize(w, h);
    }

    void on_update(float dt) override {
        time_ += dt;
        angle_ += dt * 1.5f;

        // Movimento circular do quadrado dentro da area util da tela.
        const float amplitude_x = width() * 0.25f;
        const float amplitude_y = height() * 0.25f;
        const Mat4 translation = mat4_translate(
            width() / 2.0f + std::sin(time_) * amplitude_x,
            height() / 2.0f + std::cos(time_) * amplitude_y,
            0.0f);
        const Mat4 rotation = mat4_rotate_z(angle_);
        const Mat4 scale = mat4_scale(80.0f, 80.0f, 1.0f);
        model_ = mat4_multiply(translation, mat4_multiply(rotation, scale));
    }

    void on_draw() override {
        Material material;
        material.color[0] = 1.0f;
        material.color[1] = 1.0f;
        material.color[2] = 1.0f;
        material.color[3] = 1.0f;

        // Triangulo fixo no canto superior esquerdo (coordenadas de tela).
        const Mat4 tri_model = mat4_translate(150.0f, 150.0f, 0.0f);
        Mat4 tri_scale = mat4_scale(150.0f, 150.0f, 1.0f);
        scene_.draw(triangle_, material, mat4_multiply(tri_model, tri_scale).data());

        // Quadrado rotacionado que se move pela tela.
        scene_.draw(quad_, material, model_.data());
    }

    void on_key(int32_t code, bool down) override {
        if (down && code == AKEYCODE_BACK) request_exit();
    }

    GLES2Scene scene_;
    MeshData quad_ = MeshData::quad();
    MeshData triangle_ = MeshData::triangle();
    Mat4 model_ = mat4_identity();
    float time_ = 0.0f;
    float angle_ = 0.0f;
};

APP_MAIN(AdvancedApp)
