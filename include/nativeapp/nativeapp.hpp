// nativeapp/nativeapp.hpp — API de alto nível para NativeActivity.
//
// Inclui: ciclo de vida, tipos gráficos, matemática, OpenGL ES, Vulkan,
// áudio/codecs, dispositivo/sensores, teclado virtual e permissões.
//
// Exemplo completo (GLES2 com Scene gerenciada pelo App):
//   #include <nativeapp/nativeapp.hpp>
//
//   class MeuApp : public nativeapp::App {
//   public:
//     explicit MeuApp(android_app* app) : App(app) {}
//
//   private:
//     nativeapp::GLES2Scene scene_;
//
//     void on_ready() override {
//       set_scene(&scene_);  // App chama begin_frame()/present() por nós
//       scene_.set_clear_color(0.05f, 0.05f, 0.1f);
//       nativeapp::Permissions::instance(activity())
//         .request({"android.permission.VIBRATE"});
//     }
//     // Recursos ligados à janela pertencem à surface, não ao on_ready():
//     void on_surface_created() override { scene_.init(window()); }
//     void on_surface_destroyed() override { scene_.destroy(); }
//     void on_surface_changed(int w, int h) override { scene_.resize(w, h); }
//     void on_draw() override {
//       scene_.draw(nativeapp::MeshData::triangle(), nativeapp::Material{});
//     }
//   };
//   APP_MAIN(MeuApp)

#pragma once
#include "version.hpp"

#include "app.hpp"
#include "animated_image.hpp"
#include "animated_svg.hpp"

// Tipos base primeiro (app.hpp e os backends também os incluem, mas a
// umbrella deve ser autossuficiente mesmo se a ordem interna mudar).
#include "assets.hpp"
#include "graphics_types.hpp"
#include "math.hpp"
#include "permission_types.hpp"

#include "audio.hpp"
#include "codecs.hpp"
#include "device.hpp"
#include "font.hpp"
#include "image_codecs.hpp"
#include "input_helpers.hpp"
#include "jni_bitmap.hpp"
#include "music.hpp"
#include "permissions.hpp"
#include "permissions_jni.hpp"
#include "sensor.hpp"
#include "text_input.hpp"

#include "gl.hpp"
#include "vk.hpp"
