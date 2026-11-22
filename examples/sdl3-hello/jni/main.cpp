// SDL3 com SDLActivity (classe Java oficial do SDL incluída em java/).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <android/log.h>
#include <android_native_app_glue.h>

#define LOG_TAG "sdl3-hello"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern "C" int SDL_main(int argc, char* argv[]);

// SDLActivity chama android_main(); SDL renomeia main() para SDL_main().
extern "C" void android_main(struct android_app* app) {
    (void)app;
    SDL_main(0, nullptr);
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    LOGI("SDL3_main iniciado");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOGI("SDL_Init falhou: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("SDL3 no Android", 0, 0,
                                          SDL_WINDOW_FULLSCREEN);
    if (!window) { LOGI("CreateWindow falhou: %s", SDL_GetError()); return 1; }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) { LOGI("CreateRenderer falhou: %s", SDL_GetError()); return 1; }
    LOGI("window e renderer OK");

    bool running = true;
    SDL_Event event;
    int frames = 0;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
        }

        SDL_SetRenderDrawColor(renderer, 20, 20, 31, 255);
        SDL_RenderClear(renderer);

        SDL_FRect rect{100.0f + static_cast<float>(frames % 200), 300.0f,
                       120.0f, 120.0f};
        SDL_SetRenderDrawColor(renderer, 94, 202, 112, 255);
        SDL_RenderFillRect(renderer, &rect);

        SDL_RenderPresent(renderer);
        frames++;
        SDL_Delay(16);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    LOGI("SDL3_main encerrado");
    return 0;
}
