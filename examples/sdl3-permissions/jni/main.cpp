// Teste de permissões em runtime com SDL3 + JniPermissions.
#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_main.h>
#include <android_native_app_glue.h>
#include <android/log.h>
#include <nativeapp/permissions_jni.hpp>

#define LOG_TAG "sdl3-perms"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

extern "C" int SDL_main(int argc, char* argv[]);

extern "C" void android_main(android_app* app) {
    (void)app;
    SDL_main(0, nullptr);
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_Window* window = SDL_CreateWindow("SDL3 perms", 0, 0, SDL_WINDOW_FULLSCREEN);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);

    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    LOGI("env=%p activity=%p", env, activity);

    auto& perms = JniPermissions::instance(env, activity);
    LOGI("package=%s", perms.package_name().c_str());

    perms.request_async(
        {"android.permission.CAMERA", "android.permission.RECORD_AUDIO"},
        [](const PermissionResult& result) {
            LOGI("=== CALLBACK ===");
            LOGI("all_granted=%d permanent=%d", result.all_granted,
                 result.permanently_denied);
            for (const auto& p : result.granted) LOGI("  GRANTED: %s", p.c_str());
            for (const auto& p : result.denied) LOGI("  DENIED:  %s", p.c_str());
        });

    bool running = true;
    SDL_Event event;
    while (running) {
        perms.update();  // processa o callback quando a Activity volta
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
        }
        SDL_SetRenderDrawColor(renderer, 20, 20, 31, 255);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
