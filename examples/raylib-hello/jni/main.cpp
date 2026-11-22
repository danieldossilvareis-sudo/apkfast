// raylib em NativeActivity — sem Java, sem Gradle.
// O raylib fornece android_main() e chama main() internamente.
#include <raylib.h>

#include <android/log.h>

#define LOG_TAG "raylib-hello"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

int main(void) {
    LOGI("main iniciado");

    InitWindow(0, 0, "raylib no Android");
    SetTargetFPS(60);

    int frames = 0;
    int x = 0;
    int dir = 4;

    while (!WindowShouldClose()) {
        BeginDrawing();
            ClearBackground((Color){20, 20, 31, 255});

            DrawText("raylib + apkfast!", 40, 120, 48, (Color){245, 245, 245, 255});

            x += dir;
            if (x > GetScreenWidth() - 120 || x < 0) dir = -dir;
            DrawCircle(x + 60, GetScreenHeight() / 2, 50, (Color){94, 202, 112, 255});

            DrawText(TextFormat("frame %d", frames++), 40, 200, 28,
                     (Color){180, 180, 180, 255});
        EndDrawing();
    }

    CloseWindow();
    LOGI("main encerrado");
    return 0;
}
