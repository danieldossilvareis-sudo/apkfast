// nativeapp/app.hpp — Ciclo de vida e eventos para NativeActivity.
//
// Máquina de estados:
//   Created -> Ready <-> Paused -> Destroyed
//   A surface (janela nativa) pode nascer/morrer várias vezes dentro de Ready
//   (rotação, background, diálogo de permissão). Cada mudança dispara
//   on_surface_created() / on_surface_destroyed().
//
// Uso mínimo:
//   class MeuApp : public nativeapp::App {
//     void on_ready() override { /* init uma vez */ }
//     void on_surface_created() override { scene_.init(window()); }
//     void on_surface_destroyed() override { scene_.invalidate(); }
//     void on_draw() override { scene_.render(); }
//   };
//   APP_MAIN(MeuApp);

#pragma once

#include <android_native_app_glue.h>

#include "assets.hpp"
#include "graphics_types.hpp"
#include "input_helpers.hpp"
#include "permissions.hpp"
#include "permissions_jni.hpp"
#include "text_input.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nativeapp {

enum class TouchAction { Down, Up, Move, Cancel };

struct TouchPoint {
    int32_t id;
    float x;
    float y;
};

/// Estados do ciclo de vida da aplicação.
enum class Lifecycle {
    Created,    ///< android_main iniciou, sem janela ainda
    Ready,      ///< janela ativa, renderizando
    Paused,     ///< em background (janela pode ter sido destruída)
    Destroyed,  ///< APP_CMD_DESTROY recebido
};

class App {
public:
    explicit App(android_app* app) : app_(app) {}
    virtual ~App() {
        // Evita que um evento tardio do glue chegue a uma instância já morta.
        if (app_ && app_->userData == this) {
            app_->userData = nullptr;
            app_->onAppCmd = nullptr;
            app_->onInputEvent = nullptr;
        }
    }

    int run() {
        if (!app_) return 0;
        app_->userData = this;
        app_->onAppCmd = &App::on_app_command_thunk;
        app_->onInputEvent = &App::on_input_thunk;
        state_ = Lifecycle::Created;
        finished_ = false;
        last_frame_ = std::chrono::steady_clock::now();
        reset_input();

        while (state_ != Lifecycle::Destroyed) {
            int events = 0;
            android_poll_source* source = nullptr;
            // timeout=0 sempre. Processa TODOS os eventos pendentes (sem
            // limite artificial) — um limite de 8 events deixava INIT_WINDOW
            // pendente em devices com fila cheia no startup, causando tela
            // preta. Com timeout=0 o poll retorna -1 quando a fila esvazia.
            while (ALooper_pollOnce(0, nullptr, &events,
                                    reinterpret_cast<void**>(&source)) >= 0) {
                if (source) source->process(app_, source);
                if (app_->destroyRequested || state_ == Lifecycle::Destroyed) {
                    finish();
                    return 0;
                }
                if (state_ != Lifecycle::Ready && !native_window_) break;
            }
            if (state_ == Lifecycle::Ready && native_window_ && !paused_) {
                const auto now = std::chrono::steady_clock::now();
                float dt = std::chrono::duration<float>(now - last_frame_).count();
                last_frame_ = now;
                // Clamp superior após background; NaN/negativo (relógio
                // ajustado/resume fora de ordem) vira zero para não
                // contaminar a lógica do jogo nem fps().
                if (!std::isfinite(dt) || dt < 0.0f) dt = 0.0f;
                delta_time_ = dt > 0.25f ? 0.25f : dt;

                on_tick();
                on_update(delta_time_);
                if (scene_) scene_->begin_frame();
                on_draw();
                if (scene_) scene_->present();
                frame_count_++;
            }
        }
        finish();
        return 0;
    }

    void finish() {
        // run() pode chamar finish() de dois caminhos diferentes (e o usuário
        // pode chamar manualmente): shutdown/on_destroy devem rodar uma vez.
        if (finished_) return;
        finished_ = true;
        // Cleanup de refs JNI globais antes de notificar a subclasse.
        Permissions::instance(nullptr).shutdown();
        JniPermissions::instance(nullptr, nullptr).shutdown();
        on_destroy();
    }

    // ----- Consulta de estado ----------------------------------------------
    Lifecycle lifecycle() const { return state_; }
    bool is_ready() const { return state_ == Lifecycle::Ready && !paused_; }
    ANativeActivity* activity() const { return app_ ? app_->activity : nullptr; }
    android_app* native_app() const { return app_; }
    ANativeWindow* window() const { return native_window_; }
    int width() const { return width_; }
    int height() const { return height_; }
    uint64_t frames() const { return frame_count_; }
    float delta_time() const { return delta_time_; }
    float fps() const { return delta_time_ > 0.0f ? 1.0f / delta_time_ : 0.0f; }
    bool paused() const { return paused_; }
    bool focused() const { return focused_; }

    /// Define uma cena para o App gerenciar present() automaticamente.
    void set_scene(Scene* scene) { scene_ = scene; }
    Scene* scene() const { return scene_; }

    // ----- Input helpers ----------------------------------------------------
    bool is_key_down(int32_t keycode) const {
        return keys_down_.count(keycode) > 0;
    }
    bool is_touch_down(int32_t pointer_id = 0) const {
        return touches_down_.count(pointer_id) > 0;
    }
    TouchPoint last_touch(int32_t pointer_id = 0) const {
        auto it = last_touches_.find(pointer_id);
        return it != last_touches_.end() ? it->second : TouchPoint{pointer_id, 0, 0};
    }

    void request_exit() {
        if (app_ && app_->activity) ANativeActivity_finish(app_->activity);
    }

    // ----- Eventos de ciclo de vida ----------------------------------------
    virtual void on_ready() {}          ///< uma vez, após primeira janela
    virtual void on_destroy() {}        ///< uma vez, antes de sair

    virtual void on_surface_created() {}  ///< janela criada (init GL/Vulkan aqui)
    virtual void on_surface_destroyed() {}///< janela destruída (invalidar swapchain)
    virtual void on_surface_changed(int, int) {}  ///< resize/rotação

    virtual void on_pause() {}          ///< app foi para background
    virtual void on_resume() {}         ///< app voltou para foreground

    virtual void on_draw() {}
    /// Chamado a cada frame com o delta time em segundos.
    virtual void on_update(float) {}
    virtual void on_touch(TouchAction, const std::vector<TouchPoint>&) {}
    virtual void on_key(int32_t, bool) {}

    /// Chamado a cada frame antes de on_draw().
    virtual void on_tick() {
        if (app_) Permissions::instance(app_->activity).update();
    }

protected:
    // Compatibilidade com código antigo. O estado agora é gerenciado pelo
    // ciclo de vida; chamar isto não é necessário mas é inofensivo.
    void set_ready() {}

private:
    static void on_app_command_thunk(android_app* app, int32_t cmd) {
        if (!app || !app->userData) return;
        static_cast<App*>(app->userData)->on_app_command(cmd);
    }

    static int32_t on_input_thunk(android_app* app, AInputEvent* event) {
        if (!app || !app->userData) return 0;
        return static_cast<App*>(app->userData)->on_input(event);
    }

    /// Zera o estado de polling (não há UP depois de pause/destroy).
    void reset_input() {
        keys_down_.clear();
        touches_down_.clear();
        last_touches_.clear();
    }

    bool has_valid_window() const {
        if (!app_ || !app_->window) return false;
        return ANativeWindow_getWidth(app_->window) > 0 &&
               ANativeWindow_getHeight(app_->window) > 0;
    }

    /// Cria (ou recria) a surface e notifica a subclasse.
    void activate_surface() {
        if (!has_valid_window()) return;

        const bool is_new = (native_window_ != app_->window) || !surface_active_;
        // Uma surface recriada sem passar pelo RESUME não deve herdar o dt da
        // surface anterior (janela destruída em background/rotação).
        if (is_new) last_frame_ = std::chrono::steady_clock::now();
        native_window_ = app_->window;
        width_ = ANativeWindow_getWidth(native_window_);
        height_ = ANativeWindow_getHeight(native_window_);

        if (!initialized_) {
            initialized_ = true;
            // Injeta o AAssetManager antes de qualquer cena carregar assets.
            Assets::instance(app_->activity).set_activity(app_->activity);
            // Inicializa o TextInput com o JNIEnv/Activity da UI thread.
            JNIEnv* ui_env = nullptr;
            if (app_->activity->vm) {
                app_->activity->vm->GetEnv(reinterpret_cast<void**>(&ui_env),
                                           JNI_VERSION_1_6);
            }
            if (ui_env) {
                TextInput::instance(ui_env, app_->activity->clazz);
            }
            on_ready();
            state_ = Lifecycle::Ready;
        }

        if (is_new) {
            on_surface_created();
            surface_active_ = true;
            // Notifica dimensões também na criação: muitas cenas precisam de
            // resize() para configurar projection/viewport. Sem isso, a
            // geometria em coordenadas de tela fica fora do NDC (tela preta).
            on_surface_changed(width_, height_);
        } else if (width_ != last_notified_width_ || height_ != last_notified_height_) {
            on_surface_changed(width_, height_);
        }
        last_notified_width_ = width_;
        last_notified_height_ = height_;
    }

    /// Libera a surface atual e notifica a subclasse.
    void deactivate_surface() {
        // Limpa o ponteiro ANTES do callback: a janela está sendo destruída
        // e window() não deve devolver um ponteiro inválido.
        native_window_ = nullptr;
        if (surface_active_) {
            on_surface_destroyed();
            surface_active_ = false;
        }
    }

    void on_app_command(int32_t cmd) {
        if (!app_) return;
        switch (cmd) {
            case APP_CMD_INIT_WINDOW:
                activate_surface();
                break;

            case APP_CMD_TERM_WINDOW:
                deactivate_surface();
                reset_input();
                if (state_ == Lifecycle::Ready) state_ = Lifecycle::Paused;
                break;

            case APP_CMD_WINDOW_RESIZED:
            case APP_CMD_WINDOW_REDRAW_NEEDED:
                if (has_valid_window() && surface_active_) {
                    native_window_ = app_->window;
                    const int w = ANativeWindow_getWidth(native_window_);
                    const int h = ANativeWindow_getHeight(native_window_);
                    if (w > 0 && h > 0 && (w != width_ || h != height_)) {
                        width_ = w;
                        height_ = h;
                        // Mantém o cache coerente: sem isso um activate_surface()
                        // seguinte notificaria o mesmo tamanho duas vezes.
                        last_notified_width_ = w;
                        last_notified_height_ = h;
                        on_surface_changed(w, h);
                    }
                }
                break;

            case APP_CMD_PAUSE:
                paused_ = true;
                reset_input();
                on_pause();
                break;

            case APP_CMD_RESUME:
                paused_ = false;
                last_frame_ = std::chrono::steady_clock::now();
                Permissions::instance(app_->activity).notify_activity_result();
                on_resume();
                if (state_ == Lifecycle::Paused) state_ = Lifecycle::Ready;
                // Ao voltar de background a janela pode ser nova.
                activate_surface();
                break;

            case APP_CMD_GAINED_FOCUS:
                focused_ = true;
                if (has_valid_window()) activate_surface();
                break;

            case APP_CMD_LOST_FOCUS:
                focused_ = false;
                break;

            case APP_CMD_DESTROY:
                deactivate_surface();
                reset_input();
                state_ = Lifecycle::Destroyed;
                break;

            default:
                break;
        }
    }

    int32_t on_input(AInputEvent* event) {
        if (!event) return 0;
        InputHelpers::instance().on_event(event);
        const int32_t type = AInputEvent_getType(event);
        if (type == AINPUT_EVENT_TYPE_KEY) {
            const int32_t action = AKeyEvent_getAction(event);
            const int32_t code = AKeyEvent_getKeyCode(event);
            // AKEY_EVENT_FLAG_CANCELED (ex.: troca de IME) não é um UP, mas
            // também não pode deixar a tecla "presa" no polling.
            const bool canceled =
                (AKeyEvent_getFlags(event) & AKEY_EVENT_FLAG_CANCELED) != 0;
            const bool down = !canceled &&
                              (action == AKEY_EVENT_ACTION_DOWN ||
                               action == AKEY_EVENT_ACTION_MULTIPLE);
            if (down) keys_down_.insert(code);
            else keys_down_.erase(code);

            // Repassa ao teclado virtual (se estiver aberto). O unicode vem
            // do IME e inclui acentos/símbolos de teclados internacionais.
            auto& text_input = TextInput::instance(nullptr, nullptr);
            if (text_input.is_shown()) {
                // Unicode real do IME (API 21+ via fallback JNI), com
                // mapeamento ASCII como último recurso.
                const int32_t unicode = text_input.unicode_from_event(event);
                if (text_input.on_key(code, down, unicode)) {
                    return 1;
                }
            }
            on_key(code, down);
            // Não consome teclas do sistema (volume, HOME, multimídia).
            return 0;
        }
        if (type != AINPUT_EVENT_TYPE_MOTION) return 0;

        const int32_t action_raw = AMotionEvent_getAction(event);
        const int32_t action = action_raw & AMOTION_EVENT_ACTION_MASK;
        TouchAction ta;
        switch (action) {
            case AMOTION_EVENT_ACTION_DOWN:
            case AMOTION_EVENT_ACTION_POINTER_DOWN: ta = TouchAction::Down; break;
            case AMOTION_EVENT_ACTION_UP:
            case AMOTION_EVENT_ACTION_POINTER_UP: ta = TouchAction::Up; break;
            case AMOTION_EVENT_ACTION_MOVE: ta = TouchAction::Move; break;
            case AMOTION_EVENT_ACTION_CANCEL: ta = TouchAction::Cancel; break;
            default: return 0;  // hover/out-of-frame: ignora
        }

        std::vector<TouchPoint> points;
        const size_t count = AMotionEvent_getPointerCount(event);
        points.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            points.push_back({
                AMotionEvent_getPointerId(event, i),
                AMotionEvent_getX(event, i),
                AMotionEvent_getY(event, i),
            });
        }
        // POINTER_DOWN/POINTER_UP alteram só o dedo cujo índice vem na ação.
        // ACTION_UP usa o índice 0 por contrato; tratá-lo como "todos os
        // ponteiros" apagaria dedos que ainda estão no ar em alguns devices.
        const bool single_pointer_change =
            action == AMOTION_EVENT_ACTION_POINTER_DOWN ||
            action == AMOTION_EVENT_ACTION_POINTER_UP ||
            action == AMOTION_EVENT_ACTION_UP;
        int32_t changed_id = -1;
        if (single_pointer_change) {
            int32_t index = 0;
            if (action != AMOTION_EVENT_ACTION_UP) {
                index = (action_raw >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT) &
                        AMOTION_EVENT_ACTION_POINTER_INDEX_MASK;
            }
            if (index >= 0 && static_cast<size_t>(index) < points.size()) {
                changed_id = points[static_cast<size_t>(index)].id;
            } else if (!points.empty()) {
                // Evento com índice fora da faixa: não deixa o dedo "preso".
                changed_id = points.front().id;
            }
        }

        for (const auto& point : points) {
            const bool changed = !single_pointer_change || point.id == changed_id;
            if (ta == TouchAction::Down || ta == TouchAction::Move) {
                touches_down_.insert(point.id);
            } else if (changed) {
                touches_down_.erase(point.id);
            }
            // last_touch() continua devolvendo a última posição conhecida
            // mesmo depois do dedo sair (comportamento histórico preservado).
            last_touches_[point.id] = point;
        }
        if (ta == TouchAction::Cancel) {
            touches_down_.clear();
        }
        on_touch(ta, points);
        return 1;
    }

    android_app* app_;
    Lifecycle state_ = Lifecycle::Created;
    ANativeWindow* native_window_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int last_notified_width_ = -1;
    int last_notified_height_ = -1;
    bool initialized_ = false;  // on_ready já foi chamado
    bool finished_ = false;     // finish() já rodou (shutdown é idempotente)
    bool surface_active_ = false;
    bool paused_ = false;
    bool focused_ = false;
    uint64_t frame_count_ = 0;
    float delta_time_ = 0.0f;
    std::chrono::steady_clock::time_point last_frame_{};
    Scene* scene_ = nullptr;
    std::unordered_set<int32_t> keys_down_;
    std::unordered_set<int32_t> touches_down_;
    std::unordered_map<int32_t, TouchPoint> last_touches_;
};

// Cria a instância e chama run(). Use no lugar de android_main().
#define APP_MAIN(AppClass)                                              \
    void android_main(struct android_app* app) {                         \
        AppClass instance(app);                                          \
        instance.run();                                                  \
    }

}  // namespace nativeapp
