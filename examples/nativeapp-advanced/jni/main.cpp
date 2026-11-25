// Demonstra a API gráfica avançada do nativeapp:
// MeshData, Material, matrizes, instancing real GLES3, on_update(dt),
// input helpers e Scene gerenciada.
#include <nativeapp/nativeapp.hpp>

#include <android/log.h>

#include <type_traits>

#define LOG_TAG "advanced"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

class AdvancedApp : public App {
public:
    explicit AdvancedApp(android_app* app) : App(app) {}

private:
    void on_ready() override {
        set_scene(&scene_);  // App chama begin_frame()/present() automaticamente
        scene_.set_clear_color(0.05f, 0.05f, 0.1f, 1.0f);
    }

    void on_surface_created() override {
        LOGI("on_surface_created %dx%d GLES%d", width(), height(),
             scene_.egl().version());
        scene_.init(window());  // pede GLES3; há fallback para GLES2 se preciso
        LOGI("contexto ativo: GLES%d", scene_.egl().version());
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

        // 12 instancias reais: 4 colunas x 3 linhas, cada uma com matriz e cor
        // proprias. Nenhuma matriz e uniform de u_model: vao + divisor 1.
        instances_.clear();
        instances_.reserve(12);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 4; ++col) {
                const float x = width() * (0.18f + 0.21f * col);
                const float y = height() * (0.60f + 0.085f * row);
                const Mat4 place = mat4_translate(x, y, 0.0f);
                const Mat4 spin = mat4_rotate_z(angle_ * 0.5f + row + col);
                const Mat4 size = mat4_scale(58.0f, 58.0f, 1.0f);
                GLES3Scene::Instance instance;
                instance.model = mat4_multiply(place, mat4_multiply(spin, size));
                instance.color = {{0.25f + 0.18f * col,
                                   0.25f + 0.22f * row,
                                   0.85f - 0.12f * col,
                                   1.0f}};
                instances_.push_back(instance);
            }
        }
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

        // UM draw call para 12 copias com matriz/cor próprias. O bool informa
        // se o caminho GLES3 (glDrawElementsInstanced) foi realmente usado.
        const bool instanced = scene_.draw_instanced(quad_, material, instances_);
        if (!logged_instancing_) {
            LOGI("draw_instanced: %s (GLES%d, %zu instancias)",
                 instanced ? "real" : "fallback", scene_.egl().version(),
                 instances_.size());
            logged_instancing_ = true;
        }
    }

    void on_key(int32_t code, bool down) override {
        if (down && code == AKEYCODE_BACK) request_exit();
    }

    // GLES3Scene deriva de GLES2Scene. O static_assert mantém o token no código
    // (sem comentários), o que faz o builder linkar -lEGL/-lGLESv2.
    static_assert(std::is_base_of<GLES2Scene, GLES3Scene>::value,
                  "GLES3Scene deve derivar de GLES2Scene");
    GLES3Scene scene_;
    MeshData quad_ = MeshData::quad();
    MeshData triangle_ = MeshData::triangle();
    std::vector<GLES3Scene::Instance> instances_;
    Mat4 model_ = mat4_identity();
    float time_ = 0.0f;
    float angle_ = 0.0f;
    bool logged_instancing_ = false;
};

APP_MAIN(AdvancedApp)
