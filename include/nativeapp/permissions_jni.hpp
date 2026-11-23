// nativeapp/permissions_jni.hpp — Permissões com JNI puro (SDL2/SDL3/raylib).
//
// A classe Permissions (permissions.hpp) usa ANativeActivity*. Este header
// oferece JniPermissions, que funciona com apenas JNIEnv* + jobject da
// Activity — exatamente o que SDL2/SDL3 expõem:
//
//   SDL2: SDL_AndroidGetActivity()  -> jobject
//   SDL3: SDL_GetAndroidActivity()  -> jobject
//   raylib: GetAndroidApp()->activity -> ANativeActivity* (use Permissions)
//
// Uso com SDL3:
//   #include <nativeapp/permissions_jni.hpp>
//   auto& p = JniPermissions::instance(env, SDL_GetAndroidActivity());
//   p.request_async({"android.permission.CAMERA"}, callback);
//   p.update();  // a cada frame

#pragma once

#include <jni.h>

#include "permission_types.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nativeapp {

/// Permissões via JNI puro — compatível com SDL2/SDL3 e qualquer código que
/// tenha acesso ao JNIEnv e ao jobject da Activity.
class JniPermissions {
public:
    using Callback = std::function<void(const PermissionResult&)>;
    static constexpr int kRequestCode = 0x4e415050;

    static JniPermissions& instance(JNIEnv* env, jobject activity) {
        static JniPermissions* singleton = nullptr;
        if (!singleton) singleton = new JniPermissions(env, activity);
        if (activity && singleton->activity_ != activity) {
            singleton->reattach(env, activity);
        } else if (env) {
            // JNIEnv é por thread: apenas atualiza, sem descartar um pedido
            // assíncrono em andamento.
            singleton->env_ = env;
        }
        return *singleton;
    }

    void reattach(JNIEnv* env, jobject activity) {
        if (!activity) return;
        JNIEnv* previous = env ? env : env_;
        if (previous && activity_class_) previous->DeleteGlobalRef(activity_class_);
        activity_class_ = nullptr;
        env_ = env ? env : env_;
        activity_ = activity;
        if (env_ && activity_) {
            jclass local = env_->GetObjectClass(activity_);
            if (!local) { clear_exception(env_); return; }
            activity_class_ = static_cast<jclass>(env_->NewGlobalRef(local));
            env_->DeleteLocalRef(local);
        }
        pending_.clear();
        callback_ = nullptr;
        snapshot_.clear();
        awaiting_response_ = false;
        response_received_ = false;
    }

    bool runtime_permissions_available() const {
        if (!env_) return false;
        jclass build = env_->FindClass("android/os/Build$VERSION");
        if (!build) { clear_exception(env_); return false; }
        jfieldID field = env_->GetStaticFieldID(build, "SDK_INT", "I");
        const int sdk = field ? env_->GetStaticIntField(build, field) : 0;
        if (env_->ExceptionCheck()) {
            clear_exception(env_);
            env_->DeleteLocalRef(build);
            return false;
        }
        env_->DeleteLocalRef(build);
        return sdk >= 23;
    }

    PermissionState state_of(std::string_view permission) const {
        if (!env_ || !activity_ || !activity_class_) return PermissionState::Unknown;
        if (!runtime_permissions_available()) return PermissionState::Granted;

        jmethodID check_method = method("checkSelfPermission", "(Ljava/lang/String;)I");
        if (!check_method) { clear_exception(env_); return PermissionState::Denied; }
        const std::string perm_str(permission);
        jstring jperm = env_->NewStringUTF(perm_str.c_str());
        if (!jperm) { clear_exception(env_); return PermissionState::Denied; }
        const jint check = env_->CallIntMethod(activity_, check_method, jperm);
        const bool failed = env_->ExceptionCheck();
        env_->DeleteLocalRef(jperm);
        if (failed) { clear_exception(env_); return PermissionState::Denied; }
        if (check == 0) return PermissionState::Granted;

        if (!was_requested(permission)) return PermissionState::Denied;

        jmethodID rationale_method = method(
            "shouldShowRequestPermissionRationale", "(Ljava/lang/String;)Z");
        if (!rationale_method) { clear_exception(env_); return PermissionState::Denied; }
        const std::string rationale_str(permission);
        jstring jrationale = env_->NewStringUTF(rationale_str.c_str());
        if (!jrationale) { clear_exception(env_); return PermissionState::Denied; }
        const jboolean rationale = env_->CallBooleanMethod(
            activity_, rationale_method, jrationale);
        const bool rationale_failed = env_->ExceptionCheck();
        env_->DeleteLocalRef(jrationale);
        if (rationale_failed) { clear_exception(env_); return PermissionState::Denied; }
        if (rationale) {
            denied_before_.insert(std::string(permission));
            return PermissionState::Denied;
        }
        return denied_before_.count(std::string(permission))
                   ? PermissionState::PermanentlyDenied
                   : PermissionState::Denied;
    }

    bool is_granted(std::string_view permission) const {
        return state_of(permission) == PermissionState::Granted;
    }

    PermissionResult request(const std::vector<std::string>& permissions) {
        PermissionResult result;
        if (permissions.empty()) { result.all_granted = true; return result; }
        if (!env_ || !activity_ || !activity_class_) {
            result.denied = permissions;
            return result;
        }
        if (!runtime_permissions_available()) {
            result.all_granted = true;
            result.granted = permissions;
            return result;
        }

        std::vector<std::string> pending;
        for (const auto& p : permissions) {
            if (!is_granted(p)) pending.push_back(p);
        }
        if (pending.empty()) {
            result.all_granted = true;
            result.granted = permissions;
            return result;
        }

        jclass string_class = env_->FindClass("java/lang/String");
        if (!string_class) { clear_exception(env_); result.denied = pending; return result; }
        jobjectArray array = env_->NewObjectArray(
            static_cast<jsize>(pending.size()), string_class, nullptr);
        env_->DeleteLocalRef(string_class);
        if (!array) { clear_exception(env_); result.denied = pending; return result; }
        for (size_t i = 0; i < pending.size(); ++i) {
            jstring item = env_->NewStringUTF(pending[i].c_str());
            if (!item) {
                clear_exception(env_);
                env_->DeleteLocalRef(array);
                result.denied = pending;
                return result;
            }
            env_->SetObjectArrayElement(array, static_cast<jsize>(i), item);
            env_->DeleteLocalRef(item);
        }

        jmethodID request_method = method("requestPermissions", "([Ljava/lang/String;I)V");
        bool sent = false;
        if (request_method) {
            env_->CallVoidMethod(activity_, request_method, array, kRequestCode);
            if (env_->ExceptionCheck()) clear_exception(env_);
            else sent = true;
        } else {
            clear_exception(env_);
        }
        env_->DeleteLocalRef(array);

        if (sent) {
            for (const auto& p : pending) requested_.insert(p);
        }
        snapshot_.clear();
        for (const auto& p : pending) snapshot_[p] = state_of(p);
        result.denied = pending;
        return result;
    }

    void request_async(const std::vector<std::string>& permissions, Callback callback) {
        auto initial = request(permissions);
        if (initial.all_granted) {
            if (callback) callback(initial);
            return;
        }
        pending_ = permissions;
        callback_ = std::move(callback);
        awaiting_response_ = true;
        response_received_ = false;
    }

    void notify_activity_result() { response_received_ = true; }

    void update() {
        if (!callback_ || pending_.empty() || !env_ || !activity_ || !activity_class_) return;
        bool changed = response_received_ && awaiting_response_;
        PermissionResult result;

        for (const auto& permission : pending_) {
            const PermissionState state = state_of(permission);
            auto previous = snapshot_.find(permission);
            if (previous != snapshot_.end() && previous->second != state) changed = true;
            snapshot_[permission] = state;

            if (state == PermissionState::Granted) {
                result.granted.push_back(permission);
            } else {
                result.denied.push_back(permission);
                if (state == PermissionState::Denied) result.should_show_rationale = true;
                else if (state == PermissionState::PermanentlyDenied) {
                    result.permanently_denied = true;
                }
            }
        }
        if (!changed) return;

        result.all_granted = result.denied.empty();
        response_received_ = false;
        awaiting_response_ = false;
        Callback callback = std::move(callback_);
        callback_ = nullptr;
        pending_.clear();
        snapshot_.clear();
        if (callback) callback(result);
    }

    bool has_pending() const { return !pending_.empty() && bool(callback_); }

    void openAppSettings() const {
        if (!env_ || !activity_ || !activity_class_) return;
        jclass intent_class = env_->FindClass("android/content/Intent");
        if (!intent_class) { clear_exception(env_); return; }
        jclass uri_class = env_->FindClass("android/net/Uri");
        if (!uri_class) {
            clear_exception(env_);
            env_->DeleteLocalRef(intent_class);
            return;
        }

        jmethodID intent_ctor = env_->GetMethodID(intent_class, "<init>", "(Ljava/lang/String;)V");
        jmethodID set_data = env_->GetMethodID(
            intent_class, "setData", "(Landroid/net/Uri;)Landroid/content/Intent;");
        jmethodID parse = env_->GetStaticMethodID(
            uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
        jmethodID start = method("startActivity", "(Landroid/content/Intent;)V");
        if (!intent_ctor || !set_data || !parse || !start) {
            clear_exception(env_);
            env_->DeleteLocalRef(uri_class);
            env_->DeleteLocalRef(intent_class);
            return;
        }

        jstring action = env_->NewStringUTF("android.settings.APPLICATION_DETAILS_SETTINGS");
        if (!action) {
            clear_exception(env_);
            env_->DeleteLocalRef(uri_class);
            env_->DeleteLocalRef(intent_class);
            return;
        }
        jobject intent = env_->NewObject(intent_class, intent_ctor, action);
        env_->DeleteLocalRef(action);

        const std::string uri = "package:" + package_name();
        jstring juri = env_->NewStringUTF(uri.c_str());
        jobject parsed = nullptr;
        if (intent && juri) parsed = env_->CallStaticObjectMethod(uri_class, parse, juri);
        if (juri) env_->DeleteLocalRef(juri);
        if (intent && parsed) {
            jobject result = env_->CallObjectMethod(intent, set_data, parsed);
            if (result) env_->DeleteLocalRef(result);  // setData devolve o Intent
            env_->CallVoidMethod(activity_, start, intent);
        }
        if (env_->ExceptionCheck()) clear_exception(env_);

        if (parsed) env_->DeleteLocalRef(parsed);
        if (intent) env_->DeleteLocalRef(intent);
        env_->DeleteLocalRef(uri_class);
        env_->DeleteLocalRef(intent_class);
    }

    std::string package_name() const {
        if (!env_ || !activity_ || !activity_class_) return {};
        jmethodID get_name = method("getPackageName", "()Ljava/lang/String;");
        if (!get_name) { clear_exception(env_); return {}; }
        jstring name = static_cast<jstring>(env_->CallObjectMethod(activity_, get_name));
        if (!name) { clear_exception(env_); return {}; }
        const char* chars = env_->GetStringUTFChars(name, nullptr);
        std::string result = chars ? chars : "";
        if (chars) env_->ReleaseStringUTFChars(name, chars);
        env_->DeleteLocalRef(name);
        if (env_->ExceptionCheck()) clear_exception(env_);
        return result;
    }

    void set_env(JNIEnv* env) { env_ = env; }
    JNIEnv* env() const { return env_; }
    jobject activity() const { return activity_; }

    /// Libera refs globais. Chamado pelo App::on_destroy().
    void shutdown() {
        // Não chamar UnregisterNatives(): esta classe nunca registra natives e
        // isso removeria os métodos registrados pelo motor (SDL, por exemplo).
        if (env_ && activity_class_) {
            env_->DeleteGlobalRef(activity_class_);
            activity_class_ = nullptr;
        }
        pending_.clear();
        callback_ = nullptr;
        activity_ = nullptr;
        response_received_ = false;
        awaiting_response_ = false;
        snapshot_.clear();
    }

private:
    JniPermissions(JNIEnv* env, jobject activity) : env_(env), activity_(activity) {
        if (env_ && activity_) {
            jclass local = env_->GetObjectClass(activity_);
            if (!local) { clear_exception(env_); return; }
            activity_class_ = static_cast<jclass>(env_->NewGlobalRef(local));
            env_->DeleteLocalRef(local);
        }
    }

    jmethodID method(const char* name, const char* signature) const {
        return (env_ && activity_class_)
            ? env_->GetMethodID(activity_class_, name, signature) : nullptr;
    }

    bool was_requested(std::string_view permission) const {
        return requested_.find(std::string(permission)) != requested_.end();
    }

    static void clear_exception(JNIEnv* env) {
        if (env && env->ExceptionCheck()) env->ExceptionClear();
    }

    JNIEnv* env_ = nullptr;
    jobject activity_ = nullptr;
    jclass activity_class_ = nullptr;
    std::vector<std::string> pending_;
    Callback callback_;
    bool response_received_ = false;
    bool awaiting_response_ = false;
    std::unordered_map<std::string, PermissionState> snapshot_;
    std::unordered_set<std::string> requested_;
    mutable std::unordered_set<std::string> denied_before_;
};

}  // namespace nativeapp
