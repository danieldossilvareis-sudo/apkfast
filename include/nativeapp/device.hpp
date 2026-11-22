// nativeapp/device.hpp — Utilitários do dispositivo (vibração, bateria, etc).

#pragma once

#include <android/log.h>
#include <jni.h>

#include <string>

namespace nativeapp {

class Device {
public:
    static Device& instance(JNIEnv* env, jobject activity) {
        static Device* singleton = nullptr;
        if (!singleton) singleton = new Device(env, activity);
        if (env && activity) { singleton->env_ = env; singleton->activity_ = activity; }
        return *singleton;
    }

    /// Vibra por `millis` (requer android.permission.VIBRATE).
    void vibrate(int millis) {
        if (!env_ || !activity_) return;
        jobject service = system_service("vibrator");
        if (!service) return;
        jclass clazz = env_->FindClass("android/os/Vibrator");
        if (clazz) {
            jmethodID vibrate = env_->GetMethodID(clazz, "vibrate", "(J)V");
            if (vibrate) {
                env_->CallVoidMethod(service, vibrate, static_cast<jlong>(millis));
            }
            env_->DeleteLocalRef(clazz);
        }
        env_->DeleteLocalRef(service);
    }

    /// Nível de bateria 0..100, ou -1 se indisponível.
    int battery_level() const {
        if (!env_ || !activity_) return -1;
        jobject intent = register_receiver("android.intent.action.BATTERY_CHANGED");
        if (!intent) return -1;
        jclass clazz = env_->GetObjectClass(intent);
        jmethodID get_int = env_->GetMethodID(clazz, "getIntExtra", "(Ljava/lang/String;I)I");
        jstring key = env_->NewStringUTF("level");
        jstring max_key = env_->NewStringUTF("scale");
        const int level = env_->CallIntMethod(intent, get_int, key, 0);
        const int scale = env_->CallIntMethod(intent, get_int, max_key, 100);
        env_->DeleteLocalRef(key);
        env_->DeleteLocalRef(max_key);
        env_->DeleteLocalRef(clazz);
        env_->DeleteLocalRef(intent);
        return scale > 0 ? (level * 100) / scale : -1;
    }

    /// true se carregando.
    bool is_charging() const {
        if (!env_ || !activity_) return false;
        jobject intent = register_receiver("android.intent.action.BATTERY_CHANGED");
        if (!intent) return false;
        jclass clazz = env_->GetObjectClass(intent);
        jmethodID get_int = env_->GetMethodID(clazz, "getIntExtra", "(Ljava/lang/String;I)I");
        jstring key = env_->NewStringUTF("plugged");
        const int plugged = env_->CallIntMethod(intent, get_int, key, 0);
        env_->DeleteLocalRef(key);
        env_->DeleteLocalRef(clazz);
        env_->DeleteLocalRef(intent);
        return plugged != 0;
    }

    enum class Orientation { Portrait, Landscape, Unknown };

    Orientation orientation() const {
        if (!env_ || !activity_) return Orientation::Unknown;
        jclass clazz = env_->FindClass("android/content/res/Resources");
        if (!clazz) return Orientation::Unknown;
        jmethodID get_config = env_->GetMethodID(
            find_class("android/content/Context"), "getResources",
            "()Landroid/content/res/Resources;");
        jobject res = env_->CallObjectMethod(activity_, get_config);
        if (!res) { env_->DeleteLocalRef(clazz); return Orientation::Unknown; }
        jmethodID get_conf = env_->GetMethodID(
            clazz, "getConfiguration", "()Landroid/content/res/Configuration;");
        jobject conf = env_->CallObjectMethod(res, get_conf);
        Orientation result = Orientation::Unknown;
        if (conf) {
            jclass conf_class = env_->GetObjectClass(conf);
            jfieldID field = env_->GetFieldID(conf_class, "orientation", "I");
            const int value = env_->GetIntField(conf, field);
            if (value == 1) result = Orientation::Portrait;
            else if (value == 2) result = Orientation::Landscape;
            env_->DeleteLocalRef(conf_class);
            env_->DeleteLocalRef(conf);
        }
        env_->DeleteLocalRef(res);
        env_->DeleteLocalRef(clazz);
        return result;
    }

    std::string device_model() const {
        return build_string_field("android/os/Build", "MODEL");
    }

    std::string android_version() const {
        return build_string_field("android/os/Build$VERSION", "RELEASE");
    }

    int api_level() const {
        if (!env_) return 0;
        jclass build = env_->FindClass("android/os/Build$VERSION");
        if (!build) return 0;
        jfieldID field = env_->GetStaticFieldID(build, "SDK_INT", "I");
        const int sdk = field ? env_->GetStaticIntField(build, field) : 0;
        env_->DeleteLocalRef(build);
        return sdk;
    }

    /// Ajusta brilho 0..1 (-1 = automático).
    void set_brightness(float value) {
        if (!env_ || !activity_) return;
        jclass win_clazz = find_class("android/view/Window");
        jmethodID get_window = env_->GetMethodID(
            find_class("android/app/Activity"), "getWindow", "()Landroid/view/Window;");
        jobject window = env_->CallObjectMethod(activity_, get_window);
        if (!window) return;
        jclass params_clazz = find_class("android/view/WindowManager$LayoutParams");
        jmethodID get_attrs = env_->GetMethodID(
            win_clazz, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;");
        jobject attrs = env_->CallObjectMethod(window, get_attrs);
        if (attrs && params_clazz) {
            jfieldID screen = env_->GetFieldID(params_clazz, "screenBrightness", "F");
            if (screen) env_->SetFloatField(attrs, screen, value < 0 ? -1.0f : value);
            jmethodID set_attrs = env_->GetMethodID(
                win_clazz, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V");
            if (set_attrs) env_->CallVoidMethod(window, set_attrs, attrs);
            env_->DeleteLocalRef(attrs);
        }
        env_->DeleteLocalRef(params_clazz);
        env_->DeleteLocalRef(window);
        env_->DeleteLocalRef(win_clazz);
    }

private:
    explicit Device(JNIEnv* env, jobject activity) : env_(env), activity_(activity) {}

    jclass find_class(const char* name) const {
        return env_ ? env_->FindClass(name) : nullptr;
    }

    jobject system_service(const char* name) const {
        if (!env_ || !activity_) return nullptr;
        jclass context_class = find_class("android/content/Context");
        if (!context_class) return nullptr;
        jmethodID get_service = env_->GetMethodID(
            context_class, "getSystemService",
            "(Ljava/lang/String;)Ljava/lang/Object;");
        env_->DeleteLocalRef(context_class);
        if (!get_service) return nullptr;
        jstring key = env_->NewStringUTF(name);
        jobject service = env_->CallObjectMethod(activity_, get_service, key);
        env_->DeleteLocalRef(key);
        return service;
    }

    jobject register_receiver(const char* action) const {
        if (!env_ || !activity_) return nullptr;
        jclass intent_filter = find_class("android/content/IntentFilter");
        if (!intent_filter) return nullptr;
        jmethodID ctor = env_->GetMethodID(intent_filter, "<init>", "(Ljava/lang/String;)V");
        if (!ctor) { env_->DeleteLocalRef(intent_filter); return nullptr; }
        jstring action_str = env_->NewStringUTF(action);
        jobject filter = env_->NewObject(intent_filter, ctor, action_str);
        env_->DeleteLocalRef(action_str);

        jclass ctx = find_class("android/content/Context");
        if (!ctx) { env_->DeleteLocalRef(intent_filter); return nullptr; }
        jmethodID register_method = env_->GetMethodID(
            ctx, "registerReceiver",
            "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)"
            "Landroid/content/Intent;");
        env_->DeleteLocalRef(ctx);
        if (!register_method) { env_->DeleteLocalRef(intent_filter); return nullptr; }
        jobject intent = env_->CallObjectMethod(activity_, register_method, nullptr, filter);
        env_->DeleteLocalRef(filter);
        env_->DeleteLocalRef(intent_filter);
        return intent;
    }

    std::string build_string_field(const char* class_name, const char* key) const {
        if (!env_) return {};
        jclass build = find_class(class_name);
        if (!build) return {};
        jfieldID field = env_->GetStaticFieldID(build, key, "Ljava/lang/String;");
        if (!field) { env_->DeleteLocalRef(build); return {}; }
        jstring value = static_cast<jstring>(env_->GetStaticObjectField(build, field));
        std::string result;
        if (value) {
            const char* chars = env_->GetStringUTFChars(value, nullptr);
            result = chars ? chars : "";
            if (chars) env_->ReleaseStringUTFChars(value, chars);
            env_->DeleteLocalRef(value);
        }
        env_->DeleteLocalRef(build);
        return result;
    }

    JNIEnv* env_ = nullptr;
    jobject activity_ = nullptr;
};

}  // namespace nativeapp
