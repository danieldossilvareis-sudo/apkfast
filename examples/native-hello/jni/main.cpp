#include <android/native_activity.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <cstring>

#define LOG_TAG "native-hello"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static void onAppCmd(struct android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            LOGI("APP_CMD_INIT_WINDOW");
            break;
        case APP_CMD_TERM_WINDOW:
            LOGI("APP_CMD_TERM_WINDOW");
            break;
        default:
            break;
    }
}

static int32_t onInputEvent(struct android_app*, AInputEvent*) {
    return 0;
}

void android_main(struct android_app* app) {
    app->onAppCmd = onAppCmd;
    app->onInputEvent = onInputEvent;

    LOGI("NativeActivity iniciada sem Java!");

    ANativeWindow* window = nullptr;
    while (true) {
        int events;
        struct android_poll_source* source;
        while (ALooper_pollOnce(-1, nullptr, &events, (void**)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) return;
        }
        if (app->window && !window) {
            window = app->window;
            LOGI("Janela nativa pronta: %dx%d",
                 ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));
        }
    }
}
