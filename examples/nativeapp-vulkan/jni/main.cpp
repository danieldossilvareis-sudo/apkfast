#include <nativeapp/nativeapp.hpp>

#include <android/log.h>

#include <algorithm>

#define LOG_TAG "nativeapp-vk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

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
        scene_.set_clear_color(0.10f, 0.20f, 0.35f, 1.0f);

        // Assets são por APK: coloque sprite.png e font.ttf em
        // examples/nativeapp-vulkan/assets/ antes de buildar este exemplo.
        has_sprite_ = scene_.load_texture("sprite.png");
        if (!has_sprite_) {
            LOGW("sprite.png ausente: desenhando apenas a malha colorida");
        }
        has_text_ = scene_.load_font("font.ttf", 28);
        if (!has_text_) {
            LOGW("font.ttf ausente: draw_text() será ignorado");
        }
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
        // Define a projeção ortho em pixels (mesma convenção do GLES2Scene).
        scene_.resize(w, h);
    }

    void on_pause() override { LOGI("on_pause"); }
    void on_resume() override { LOGI("on_resume"); }

    void on_draw() override {
        const float w = static_cast<float>(width() > 0 ? width() : 1);
        const float h = static_cast<float>(height() > 0 ? height() : 1);

        // 1) Triângulo com cor por vértice (Material branco, sem textura).
        const MeshData triangle = MeshData::triangle();
        Material colored;
        colored.color[0] = 1.0f;
        colored.color[1] = 1.0f;
        colored.color[2] = 1.0f;
        colored.color[3] = 1.0f;
        colored.texture.clear();
        const Mat4 triangle_model = mat4_multiply(
            mat4_translate(w * 0.5f, h * 0.35f, 0.0f),
            mat4_scale(std::min(w, h) * 0.45f, std::min(w, h) * 0.45f, 1.0f));
        scene_.draw(triangle, colored, triangle_model.data());

        // 2) Quad com textura vinda de assets/ (decode compartilhado com GL).
        if (has_sprite_) {
            const MeshData quad = MeshData::quad(180.0f, 180.0f);
            Material textured;
            textured.texture = "sprite.png";
            textured.blend = true;
            textured.depth_test = true;
            const Mat4 quad_model = mat4_translate(w * 0.72f, h * 0.55f, 0.0f);
            scene_.draw(quad, textured, quad_model.data());
        }

        // 3) Texto com FontAtlas, desenhado por cima (sem depth).
        if (has_text_ && scene_.has_font()) {
            scene_.draw_text("VULKAN OK", 24.0f, 48.0f, 1.4f,
                             1.0f, 0.95f, 0.65f, 1.0f);
        }

        // Grava, submete e apresenta os itens enfileirados acima.
        scene_.render();
    }

    void on_key(int32_t code, bool down) override {
        if (down && code == AKEYCODE_BACK) request_exit();
    }

    // Demonstração das APIs P2 (compilada, mas não chamada: a cena acima
    // permanece pixel-a-pixel idêntica à referência GLES2).
    void demo_p2_api() {
        scene_.set_validation(true);  // antes de init(); degrada se não houver layer
        scene_.load_texture("sprite.png");
        scene_.invalidate_texture("sprite.png");
        scene_.reload_texture("sprite.png");

        std::vector<Mat4> models;
        for (int i = 0; i < 3; ++i) {
            models.push_back(mat4_translate(80.0f * i, 0.0f, 0.0f));
        }
        // Um draw real com binding de instância (inputRate=INSTANCE).
        scene_.draw_instanced(MeshData::quad(40.0f, 40.0f), Material{}, models);

        CustomUniformBinding binding;
        binding.binding = 0;
        binding.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binding.size = 16;
        const CustomUniformHandle handle = scene_.register_custom_uniforms({binding});
        const float color[4] = {1.0f, 0.5f, 0.25f, 1.0f};
        scene_.write_uniform(handle, 0, color, sizeof(color));
        scene_.set_pipeline(PipelineConfig{}, handle);  // configure SPV antes de usar
        scene_.destroy_custom_uniforms(handle);
    }

    bool has_sprite_ = false;
    bool has_text_ = false;
    VulkanScene scene_;
};

APP_MAIN(MeuApp)
