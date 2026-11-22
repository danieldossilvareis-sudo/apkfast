// SDL2 em NativeActivity — sem Java, sem Gradle.
#include <SDL.h>
#include <SDL_main.h>

#include <android/log.h>
#include <android_native_app_glue.h>

#define LOG_TAG "sdl2-hello"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// Entry point chamado pelo glue do NDK. O SDL renomeia main() -> SDL_main().
extern "C" void android_main(struct android_app* app) {
    (void)app;
    SDL_main(0, nullptr);
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    LOGI("SDL_main iniciado");

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        LOGI("SDL_Init falhou: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "SDL2 no Android", 0, 0, 0, 0, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    bool running = true;
    SDL_Event event;
    int frames = 0;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_AC_BACK) {
                running = false;
            }
        }

        SDL_SetRenderDrawColor(renderer, 20, 20, 31, 255);
        SDL_RenderClear(renderer);

        SDL_Rect rect{100 + (frames % 200), 300, 120, 120};
        SDL_SetRenderDrawColor(renderer, 94, 202, 112, 255);
        SDL_RenderFillRect(renderer, &rect);

        SDL_RenderPresent(renderer);
        frames++;
        SDL_Delay(16);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    LOGI("SDL_main encerrado");
    return 0;
}
