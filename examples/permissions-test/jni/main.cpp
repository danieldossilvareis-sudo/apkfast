// Teste real da API de permissões em runtime.
#include <nativeapp/nativeapp.hpp>

#include <android/log.h>

#define LOG_TAG "perms-test"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

static const char* state_name(PermissionState state) {
    switch (state) {
        case PermissionState::Granted: return "GRANTED";
        case PermissionState::Denied: return "DENIED";
        case PermissionState::PermanentlyDenied: return "PERMANENTLY_DENIED";
        default: return "UNKNOWN";
    }
}

class PermsApp : public App {
public:
    explicit PermsApp(android_app* app) : App(app) {}

private:
    void on_ready() override {
        auto& perms = Permissions::instance(activity());

        LOGI("runtime_permissions_available=%d", perms.runtime_permissions_available());
        LOGI("package=%s", perms.package_name().c_str());

        for (const char* permission : permissions_) {
            LOGI("antes: %s = %s", permission, state_name(perms.state_of(permission)));
        }

        // Pedimos 3 de uma vez e recebemos o resultado real via callback.
        perms.request_async(
            {permissions_[0], permissions_[1], permissions_[2]},
            [](const PermissionResult& result) {
                LOGI("=== CALLBACK ===");
                LOGI("all_granted=%d rationale=%d permanent=%d",
                     result.all_granted, result.should_show_rationale,
                     result.permanently_denied);
                for (const auto& p : result.granted) LOGI("  GRANTED:   %s", p.c_str());
                for (const auto& p : result.denied) LOGI("  DENIED:    %s", p.c_str());
            });
    }

    void on_surface_created() override {}
    void on_resume() override { LOGI("on_resume (notify_activity_result)"); }
    void on_draw() override {}

    static constexpr const char* permissions_[] = {
        "android.permission.CAMERA",
        "android.permission.RECORD_AUDIO",
        "android.permission.ACCESS_FINE_LOCATION",
    };
};

APP_MAIN(PermsApp)
