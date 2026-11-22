// Demonstra Device, Sensors, InputHelpers e Audio.
#include <nativeapp/nativeapp.hpp>
#include <android/log.h>

#include <cmath>
#include <vector>

#define LOG_TAG "device-demo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

class DemoApp : public App {
public:
    explicit DemoApp(android_app* app) : App(app) {}

private:
    void on_ready() override {
        // Inicializa Device com o JNIEnv da thread nativa.
        JNIEnv* env = nullptr;
        activity()->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (!env) activity()->vm->AttachCurrentThread(&env, nullptr);
        Device::instance(env, activity()->clazz);

        LOGI("device=%s android=%s api=%d bateria=%d%%",
             Device::instance(nullptr, nullptr).device_model().c_str(),
             Device::instance(nullptr, nullptr).android_version().c_str(),
             Device::instance(nullptr, nullptr).api_level(),
             Device::instance(nullptr, nullptr).battery_level());

        LOGI("sensores: accel=%d gyro=%d init=%d",
             Sensors::instance().init(),
             Sensors::instance().has_gyroscope(),
             Sensors::instance().has_accelerometer());

        LOGI("audio: AAudio=%d init=%d",
             Audio::instance().init(), Audio::instance().using_aaudio());

        // Beep 440Hz gerado em memória (0.15s).
        beep_.sample_rate = 44100;
        beep_.channels = 1;
        const int frames = 44100 * 15 / 100;
        beep_.samples.resize(frames);
        for (int i = 0; i < frames; ++i) {
            const float envelope = 1.0f - static_cast<float>(i) / frames;
            beep_.samples[i] = static_cast<int16_t>(
                std::sin(2.0f * 3.14159265f * 440.0f * i / 44100.0f) *
                12000.0f * envelope);
        }
        Audio::instance().play(beep_);
        Device::instance(nullptr, nullptr).vibrate(80);

        set_scene(&scene_);
        scene_.set_clear_color(0.05f, 0.05f, 0.10f);
    }

    void on_surface_created() override { scene_.init(window()); }
    void on_surface_destroyed() override { scene_.destroy(); }
    void on_surface_changed(int w, int h) override { scene_.resize(w, h); }

    void on_tick() override {
        Sensors::instance().update();
        if (frames() % 120 == 0) {
            const auto accel = Sensors::instance().acceleration();
            LOGI("accel=%.2f %.2f %.2f mouse=%.0f,%.0f gamepad=%d",
                 accel.x, accel.y, accel.z,
                 InputHelpers::instance().mouse_x(),
                 InputHelpers::instance().mouse_y(),
                 InputHelpers::instance().gamepad_connected());
        }
    }

    void on_draw() override {
        const auto accel = Sensors::instance().acceleration();
        const float x = width() / 2.0f + accel.x * 200.0f;
        const float y = height() / 2.0f + accel.y * 200.0f;
        Mat4 model = mat4_translate(x, y, 0);
        model = mat4_multiply(model, mat4_scale(120, 120, 1));
        Material material;
        scene_.draw(tile_, material, model.data());
    }

    void on_touch(TouchAction action, const std::vector<TouchPoint>& pts) override {
        if (action == TouchAction::Down && !pts.empty()) {
            Device::instance(nullptr, nullptr).vibrate(40);
            Audio::instance().play(beep_);
        }
    }

    void on_key(int32_t code, bool down) override {
        if (down && code == 4) request_exit();
    }

    GLES2Scene scene_;
    MeshData tile_ = MeshData::quad();
    AudioBuffer beep_;
};

APP_MAIN(DemoApp)
