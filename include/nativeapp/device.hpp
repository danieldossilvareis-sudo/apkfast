// nativeapp/device.hpp — Utilitários do dispositivo (vibração, bateria, etc).

#pragma once

#include <android/log.h>
#include <jni.h>

#include <algorithm>
#include <cmath>
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
        if (!env_ || !activity_ || millis <= 0) return;
        jobject service = system_service("vibrator");
        if (!service) return;
        jclass clazz = env_->FindClass("android/os/Vibrator");
        if (clazz) {
            jmethodID vibrate = env_->GetMethodID(clazz, "vibrate", "(J)V");
            if (vibrate) {
                env_->CallVoidMethod(service, vibrate, static_cast<jlong>(millis));
            }
            clear_exception();
            env_->DeleteLocalRef(clazz);
        } else {
            clear_exception();
        }
        env_->DeleteLocalRef(service);
    }

    /// Nível de bateria 0..100, ou -1 se indisponível.
    int battery_level() const {
        if (!env_ || !activity_) return -1;
        jobject intent = register_receiver("android.intent.action.BATTERY_CHANGED");
        if (!intent) return -1;
        jclass clazz = env_->GetObjectClass(intent);
        jmethodID get_int = clazz
            ? env_->GetMethodID(clazz, "getIntExtra", "(Ljava/lang/String;I)I") : nullptr;
        if (!get_int) {
            clear_exception();
            if (clazz) env_->DeleteLocalRef(clazz);
            env_->DeleteLocalRef(intent);
            return -1;
        }
        jstring key = new_string("level");
        jstring max_key = new_string("scale");
        int level = 0, scale = 0;
        if (key && max_key) {
            level = env_->CallIntMethod(intent, get_int, key, 0);
            scale = env_->CallIntMethod(intent, get_int, max_key, 100);
        }
        clear_exception();
        if (key) env_->DeleteLocalRef(key);
        if (max_key) env_->DeleteLocalRef(max_key);
        env_->DeleteLocalRef(clazz);
        env_->DeleteLocalRef(intent);
        if (scale <= 0) return -1;
        return std::max(0, std::min(100, (level * 100) / scale));
    }

    /// true se carregando.
    bool is_charging() const {
        if (!env_ || !activity_) return false;
        jobject intent = register_receiver("android.intent.action.BATTERY_CHANGED");
        if (!intent) return false;
        jclass clazz = env_->GetObjectClass(intent);
        jmethodID get_int = clazz
            ? env_->GetMethodID(clazz, "getIntExtra", "(Ljava/lang/String;I)I") : nullptr;
        if (!get_int) {
            clear_exception();
            if (clazz) env_->DeleteLocalRef(clazz);
            env_->DeleteLocalRef(intent);
            return false;
        }
        jstring key = new_string("plugged");
        int plugged = 0;
        if (key) plugged = env_->CallIntMethod(intent, get_int, key, 0);
        clear_exception();
        if (key) env_->DeleteLocalRef(key);
        env_->DeleteLocalRef(clazz);
        env_->DeleteLocalRef(intent);
        return plugged != 0;
    }

    enum class Orientation { Portrait, Landscape, Unknown };

    Orientation orientation() const {
        if (!env_ || !activity_) return Orientation::Unknown;
        // getRotation() reflete a rotação real da janela mesmo em multi-window.
        jclass context_class = find_class("android/content/Context");
        if (!context_class) { clear_exception(); return Orientation::Unknown; }
        jmethodID get_wm = env_->GetMethodID(
            context_class, "getWindowManager", "()Landroid/view/WindowManager;");
        env_->DeleteLocalRef(context_class);
        if (!get_wm) { clear_exception(); return Orientation::Unknown; }

        jobject wm = env_->CallObjectMethod(activity_, get_wm);
        if (!wm) { clear_exception(); return Orientation::Unknown; }
        jclass wm_class = env_->GetObjectClass(wm);
        jmethodID get_display = wm_class
            ? env_->GetMethodID(wm_class, "getDefaultDisplay", "()Landroid/view/Display;")
            : nullptr;
        jobject display = get_display ? env_->CallObjectMethod(wm, get_display) : nullptr;
        Orientation result = Orientation::Unknown;
        if (display) {
            jclass display_class = env_->GetObjectClass(display);
            jmethodID get_rotation = display_class
                ? env_->GetMethodID(display_class, "getRotation", "()I") : nullptr;
            if (get_rotation) {
                const int rotation = env_->CallIntMethod(display, get_rotation);
                // ROTATION_0=0, 90=1, 180=2, 270=3
                result = (rotation == 1 || rotation == 3)
                    ? Orientation::Landscape : Orientation::Portrait;
            }
            clear_exception();
            if (display_class) env_->DeleteLocalRef(display_class);
            env_->DeleteLocalRef(display);
        }
        clear_exception();
        if (wm_class) env_->DeleteLocalRef(wm_class);
        env_->DeleteLocalRef(wm);
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
        if (!build) { clear_exception(); return 0; }
        jfieldID field = env_->GetStaticFieldID(build, "SDK_INT", "I");
        const int sdk = field ? env_->GetStaticIntField(build, field) : 0;
        clear_exception();
        env_->DeleteLocalRef(build);
        return sdk;
    }

    /// Ajusta brilho 0..1 (-1 = automático).
    void set_brightness(float value) {
        if (!env_ || !activity_) return;

        jclass activity_clazz = find_class("android/app/Activity");
        jclass win_clazz = find_class("android/view/Window");
        jclass params_clazz = find_class("android/view/WindowManager$LayoutParams");
        if (!activity_clazz || !win_clazz || !params_clazz) {
            clear_exception();
            if (activity_clazz) env_->DeleteLocalRef(activity_clazz);
            if (win_clazz) env_->DeleteLocalRef(win_clazz);
            if (params_clazz) env_->DeleteLocalRef(params_clazz);
            return;
        }

        jmethodID get_window = env_->GetMethodID(
            activity_clazz, "getWindow", "()Landroid/view/Window;");
        jmethodID get_attrs = env_->GetMethodID(
            win_clazz, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;");
        jmethodID set_attrs = env_->GetMethodID(
            win_clazz, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V");
        jfieldID screen = env_->GetFieldID(params_clazz, "screenBrightness", "F");
        env_->DeleteLocalRef(activity_clazz);

        if (!get_window || !get_attrs || !set_attrs || !screen) {
            clear_exception();
            env_->DeleteLocalRef(win_clazz);
            env_->DeleteLocalRef(params_clazz);
            return;
        }

        jobject window = env_->CallObjectMethod(activity_, get_window);
        if (!window) {
            clear_exception();
            env_->DeleteLocalRef(win_clazz);
            env_->DeleteLocalRef(params_clazz);
            return;
        }
        jobject attrs = env_->CallObjectMethod(window, get_attrs);
        if (attrs) {
            const float clamped = !std::isfinite(value) ? -1.0f : std::max(-1.0f, std::min(1.0f, value));
            env_->SetFloatField(attrs, screen, clamped);
            env_->CallVoidMethod(window, set_attrs, attrs);
            clear_exception();
            env_->DeleteLocalRef(attrs);
        }
        env_->DeleteLocalRef(window);
        env_->DeleteLocalRef(win_clazz);
        env_->DeleteLocalRef(params_clazz);
    }

private:
    explicit Device(JNIEnv* env, jobject activity) : env_(env), activity_(activity) {}

    void clear_exception() const {
        if (env_ && env_->ExceptionCheck()) env_->ExceptionClear();
    }

    jstring new_string(const char* text) const {
        if (!env_) return nullptr;
        jstring value = env_->NewStringUTF(text);
        if (!value) clear_exception();
        return value;
    }

    jclass find_class(const char* name) const {
        return env_ ? env_->FindClass(name) : nullptr;
    }

    jobject system_service(const char* name) const {
        if (!env_ || !activity_) return nullptr;
        jclass context_class = find_class("android/content/Context");
        if (!context_class) { clear_exception(); return nullptr; }
        jmethodID get_service = env_->GetMethodID(
            context_class, "getSystemService",
            "(Ljava/lang/String;)Ljava/lang/Object;");
        env_->DeleteLocalRef(context_class);
        if (!get_service) { clear_exception(); return nullptr; }
        jstring key = new_string(name);
        if (!key) return nullptr;
        jobject service = env_->CallObjectMethod(activity_, get_service, key);
        clear_exception();
        env_->DeleteLocalRef(key);
        return service;
    }

    jobject register_receiver(const char* action) const {
        if (!env_ || !activity_) return nullptr;
        jclass intent_filter = find_class("android/content/IntentFilter");
        if (!intent_filter) { clear_exception(); return nullptr; }
        jmethodID ctor = env_->GetMethodID(intent_filter, "<init>", "(Ljava/lang/String;)V");
        if (!ctor) {
            clear_exception();
            env_->DeleteLocalRef(intent_filter);
            return nullptr;
        }
        jstring action_str = new_string(action);
        if (!action_str) {
            env_->DeleteLocalRef(intent_filter);
            return nullptr;
        }
        jobject filter = env_->NewObject(intent_filter, ctor, action_str);
        env_->DeleteLocalRef(action_str);
        env_->DeleteLocalRef(intent_filter);
        if (!filter) { clear_exception(); return nullptr; }

        jclass ctx = find_class("android/content/Context");
        if (!ctx) {
            clear_exception();
            env_->DeleteLocalRef(filter);
            return nullptr;
        }
        jmethodID register_method = env_->GetMethodID(
            ctx, "registerReceiver",
            "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)"
            "Landroid/content/Intent;");
        env_->DeleteLocalRef(ctx);
        if (!register_method) {
            clear_exception();
            env_->DeleteLocalRef(filter);
            return nullptr;
        }
        jobject intent = env_->CallObjectMethod(activity_, register_method, nullptr, filter);
        clear_exception();
        env_->DeleteLocalRef(filter);
        return intent;
    }

    std::string build_string_field(const char* class_name, const char* key) const {
        if (!env_) return {};
        jclass build = find_class(class_name);
        if (!build) { clear_exception(); return {}; }
        jfieldID field = env_->GetStaticFieldID(build, key, "Ljava/lang/String;");
        if (!field) {
            clear_exception();
            env_->DeleteLocalRef(build);
            return {};
        }
        jstring value = static_cast<jstring>(env_->GetStaticObjectField(build, field));
        std::string result;
        if (value) {
            const char* chars = env_->GetStringUTFChars(value, nullptr);
            result = chars ? chars : "";
            if (chars) env_->ReleaseStringUTFChars(value, chars);
            env_->DeleteLocalRef(value);
        }
        clear_exception();
        env_->DeleteLocalRef(build);
        return result;
    }

    JNIEnv* env_ = nullptr;
    jobject activity_ = nullptr;
};

}  // namespace nativeapp
