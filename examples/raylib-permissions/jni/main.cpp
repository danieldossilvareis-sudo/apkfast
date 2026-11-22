// Permissões em runtime com raylib + API nativeapp.
// raylib expõe GetAndroidApp() (android_app*) internamente.
#include <raylib.h>
#include <android_native_app_glue.h>
#include <android/log.h>
#include <nativeapp/permissions.hpp>

#define LOG_TAG "raylib-perms"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// Declarado em rcore_android.c da raylib, mas não exposto no header público.
extern "C" android_app* GetAndroidApp(void);

int main(void) {
    android_app* app = GetAndroidApp();
    LOGI("android_app=%p activity=%p", app, app ? app->activity : nullptr);

    auto& perms = nativeapp::Permissions::instance(app->activity);
    LOGI("package=%s", perms.package_name().c_str());

    perms.request_async(
        {"android.permission.CAMERA", "android.permission.RECORD_AUDIO"},
        [](const nativeapp::PermissionResult& result) {
            LOGI("=== CALLBACK ===");
            LOGI("all_granted=%d permanent=%d", result.all_granted,
                 result.permanently_denied);
            for (const auto& p : result.granted) LOGI("  GRANTED: %s", p.c_str());
            for (const auto& p : result.denied) LOGI("  DENIED:  %s", p.c_str());
        });

    InitWindow(0, 0, "raylib perms");
    SetTargetFPS(60);
    while (!WindowShouldClose()) {
        perms.update();  // processa o callback
        BeginDrawing();
          ClearBackground(DARKBLUE);
          DrawText("raylib + permissions", 40, 80, 32, WHITE);
        EndDrawing();
    }
    CloseWindow();
    return 0;
}
