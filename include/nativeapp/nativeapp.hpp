// nativeapp/nativeapp.hpp — API de alto nível para NativeActivity.
//
// Inclui: ciclo de vida, OpenGL ES, Vulkan e permissões em runtime.
//
// Exemplo completo:
//   #include <nativeapp/nativeapp.hpp>
//
//   class MeuApp : public nativeapp::App {
//     nativeapp::GLES2Scene scene_;
//     void on_ready() override {
//       scene_.init(window());
//       nativeapp::Permissions::instance(activity())
//         .request({"android.permission.VIBRATE"});
//     }
//     void on_resize(int w, int h) override { scene_.resize(w, h); }
//     void on_draw() override { scene_.render(); }
//   };
//   APP_MAIN(MeuApp);

#pragma once

#include "app.hpp"
#include "gl.hpp"
#include "permissions.hpp"
#include "permissions_jni.hpp"
#include "text_input.hpp"
#include "vk.hpp"
