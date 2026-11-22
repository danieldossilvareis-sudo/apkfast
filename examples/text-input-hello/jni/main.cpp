// Teclado virtual com raylib + nativeapp::TextInput (100% JNI, sem Java extra).
#include <raylib.h>
#include <android_native_app_glue.h>
#include <nativeapp/text_input.hpp>

#include <string>

extern "C" android_app* GetAndroidApp(void);

int main(void) {
    android_app* app = GetAndroidApp();
    ANativeActivity* activity = app->activity;

    JNIEnv* env = nullptr;
    activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (!env) activity->vm->AttachCurrentThread(&env, nullptr);

    auto& input = nativeapp::TextInput::instance(env, activity->clazz);

    std::string text;
    input.set_callback([&](const std::string& value, bool) { text = value; });

    InitWindow(0, 0, "text input");
    SetTargetFPS(60);

    bool focused = false;
    while (!WindowShouldClose()) {
        input.update();

        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) ||
            (GetTouchPointCount() > 0 && GetTouchPosition(0).y > GetScreenHeight() * 0.55f)) {
            if (!focused) {
                input.show("Digite algo:");
                focused = true;
            } else {
                input.hide();
                focused = false;
            }
        }

        BeginDrawing();
          ClearBackground((Color){20, 20, 31, 255});
          DrawText("apkfast - teclado virtual", 40, 100, 40, (Color){245, 245, 245, 255});
          DrawText(focused ? "Toque para FECHAR o teclado" : "Toque para ABRIR o teclado",
                   40, 180, 28, (Color){180, 180, 180, 255});
          DrawRectangleLines(40, 260, GetScreenWidth() - 80, 120,
                             (Color){94, 202, 112, 255});
          DrawText(text.empty() ? "(vazio)" : text.c_str(), 60, 300, 36,
                   (Color){94, 202, 112, 255});
          DrawText("ENTER finaliza - suporte a acentos/emoji via IME",
                   40, 430, 24, (Color){140, 140, 140, 255});
        EndDrawing();
    }

    input.destroy();
    CloseWindow();
    return 0;
}
