// nativeapp/permissions.hpp — Permissões em runtime para NativeActivity.
//
// Uso síncrono:
//   auto& p = Permissions::instance(app->activity);
//   auto r = p.request({"android.permission.CAMERA"});
//   if (r.all_granted) ...
//
// Uso assíncrono (recomendado):
//   p.request_async({"android.permission.CAMERA"},
//       [](const PermissionResult& r) {
//           if (r.all_granted) ... else if (r.permanently_denied) ...
//       });
//
// O callback é disparado automaticamente pelo App::on_tick() quando o estado
// muda (concedido / negado), sem precisar de Java nem de onActivityResult.

#pragma once

#include <android/native_activity.h>
#include <jni.h>

#include "permission_types.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nativeapp {

class Permissions {
public:
    using Callback = std::function<void(const PermissionResult&)>;

    static Permissions& instance(ANativeActivity* activity) {
        static Permissions* singleton = nullptr;
        if (!singleton) singleton = new Permissions(activity);
        if (activity && singleton->activity_ != activity) singleton->reattach(activity);
        return *singleton;
    }

    /// Atualiza a referência de Activity após recriação (rotação, etc.).
    void reattach(ANativeActivity* activity) {
        if (!activity) return;
        // A Activity antiga pode já estar morta: tenta o VM nova antes de
        // liberar a global ref.
        JNIEnv* jenv = env();
        if (!jenv) jenv = env_for(activity);
        if (jenv) release_global_refs(jenv);
        activity_ = activity;
        jenv = env();
        if (jenv) cache_global_refs(jenv);
        pending_.clear();
        callback_ = nullptr;
        response_received_ = false;
        awaiting_response_ = false;
        snapshot_.clear();
    }

    /// API < 23: permissões concedidas no install.
    bool runtime_permissions_available() const {
        JNIEnv* jenv = env();
        if (!jenv) return false;
        jclass build = jenv->FindClass("android/os/Build$VERSION");
        if (!build) { clear_exception(jenv); return false; }
        jfieldID field = jenv->GetStaticFieldID(build, "SDK_INT", "I");
        const int sdk = field ? jenv->GetStaticIntField(build, field) : 0;
        if (jenv->ExceptionCheck()) {
            clear_exception(jenv);
            jenv->DeleteLocalRef(build);
            return false;
        }
        jenv->DeleteLocalRef(build);
        return sdk >= 23;
    }

    /// Retorna o estado detalhado de uma permissão.
    PermissionState state_of(std::string_view permission) const {
        JNIEnv* jenv = env();
        if (!jenv || !activity_ || !activity_->clazz || !activity_class_) {
            return PermissionState::Unknown;
        }
        if (!runtime_permissions_available()) return PermissionState::Granted;

        jmethodID check_method = method(jenv, "checkSelfPermission", "(Ljava/lang/String;)I");
        if (!check_method) { clear_exception(jenv); return PermissionState::Denied; }
        const std::string perm_str(permission);
        jstring jperm = jenv->NewStringUTF(perm_str.c_str());
        if (!jperm) { clear_exception(jenv); return PermissionState::Denied; }
        const jint check = jenv->CallIntMethod(activity_->clazz, check_method, jperm);
        const bool failed = jenv->ExceptionCheck();
        jenv->DeleteLocalRef(jperm);
        if (failed) { clear_exception(jenv); return PermissionState::Denied; }
        if (check == 0) return PermissionState::Granted;

        // Depois da primeira negação, rationale=true significa que ainda pode
        // perguntar; rationale=false + já negada = "don't ask again".
        // "Nunca pedida" não é permanente: rationale=false antes da primeira
        // solicitação é o comportamento normal do Android.
        if (!was_requested(permission)) return PermissionState::Denied;

        jmethodID rationale_method = method(
            jenv, "shouldShowRequestPermissionRationale", "(Ljava/lang/String;)Z");
        if (!rationale_method) { clear_exception(jenv); return PermissionState::Denied; }
        const std::string rationale_str(permission);
        jstring jrationale = jenv->NewStringUTF(rationale_str.c_str());
        if (!jrationale) { clear_exception(jenv); return PermissionState::Denied; }
        const jboolean rationale = jenv->CallBooleanMethod(
            activity_->clazz, rationale_method, jrationale);
        const bool rationale_failed = jenv->ExceptionCheck();
        jenv->DeleteLocalRef(jrationale);
        if (rationale_failed) { clear_exception(jenv); return PermissionState::Denied; }

        // rationale=true -> pode perguntar de novo.
        // rationale=false SÓ é permanente se já houve uma negação registrada
        // nesta sessão (caso contrário é a primeira negação do sistema).
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

    /// Solicita permissões. O resultado real é assíncrono; use update() ou
    /// request_async() para receber a resposta.
    PermissionResult request(const std::vector<std::string>& permissions) {
        PermissionResult result;
        if (permissions.empty()) {
            result.all_granted = true;
            return result;
        }

        JNIEnv* jenv = env();
        if (!jenv || !activity_ || !activity_->clazz || !activity_class_) {
            result.denied = permissions;
            return result;
        }
        if (!runtime_permissions_available()) {
            result.all_granted = true;
            result.granted = permissions;
            return result;
        }

        std::vector<std::string> pending;
        for (const auto& permission : permissions) {
            if (!is_granted(permission)) pending.push_back(permission);
        }
        if (pending.empty()) {
            result.all_granted = true;
            result.granted = permissions;
            return result;
        }

        jclass string_class = find_class(jenv, "java/lang/String");
        if (!string_class) { clear_exception(jenv); result.denied = pending; return result; }
        jobjectArray array = jenv->NewObjectArray(
            static_cast<jsize>(pending.size()), string_class, nullptr);
        jenv->DeleteLocalRef(string_class);
        if (!array) { clear_exception(jenv); result.denied = pending; return result; }

        for (size_t i = 0; i < pending.size(); ++i) {
            jstring item = jenv->NewStringUTF(pending[i].c_str());
            if (!item) {
                clear_exception(jenv);
                jenv->DeleteLocalRef(array);
                result.denied = pending;
                return result;
            }
            jenv->SetObjectArrayElement(array, static_cast<jsize>(i), item);
            jenv->DeleteLocalRef(item);
        }

        jmethodID request_method = method(jenv, "requestPermissions", "([Ljava/lang/String;I)V");
        bool sent = false;
        if (request_method) {
            jenv->CallVoidMethod(activity_->clazz, request_method, array, kRequestCode);
            if (jenv->ExceptionCheck()) clear_exception(jenv);
            else sent = true;
        } else {
            clear_exception(jenv);
        }
        jenv->DeleteLocalRef(array);

        // Marca como pedidas ANTES de consultar o rationale.
        if (sent) {
            for (const auto& permission : pending) requested_.insert(permission);
        }

        // Guarda o estado atual para detectar mudanças no update().
        snapshot_.clear();
        for (const auto& permission : pending) {
            snapshot_[permission] = state_of(permission);
        }
        result.denied = pending;
        return result;
    }

    /// Registra callback e dispara quando o estado das permissões mudar.
    void request_async(const std::vector<std::string>& permissions, Callback callback) {
        auto initial = request(permissions);
        if (initial.all_granted) {
            if (callback) callback(initial);
            return;
        }
        pending_ = permissions;
        callback_ = std::move(callback);
        awaiting_response_ = true;
        // Descarta o RESUME inicial (que ocorreu antes de pedir); só o RESUME
        // posterior ao diálogo deve disparar o callback.
        response_received_ = false;
    }

    /// Chamado automaticamente pelo App a cada frame.
    void update() {
        if (!callback_ || pending_.empty()) return;
        JNIEnv* jenv = env();
        if (!jenv) return;

        // O diálogo de permissão pausa a Activity; ao voltar, o App chama
        // notify_activity_result(). Isso é suficiente para disparar o callback
        // mesmo que o estado final seja igual ao inicial (usuário negou).
        bool changed = response_received_ && awaiting_response_;
        PermissionResult result;

        for (const auto& permission : pending_) {
            PermissionState state = state_of(permission);
            auto previous = snapshot_.find(permission);

            if (previous != snapshot_.end() && previous->second != state) {
                changed = true;
            }
            snapshot_[permission] = state;

            if (state == PermissionState::Granted) {
                result.granted.push_back(permission);
            } else {
                result.denied.push_back(permission);
                if (state == PermissionState::Denied) {
                    result.should_show_rationale = true;
                } else if (state == PermissionState::PermanentlyDenied) {
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

    /// Sinaliza que a Activity voltou do diálogo de permissão.
    void notify_activity_result() { response_received_ = true; }

    bool has_pending() const { return !pending_.empty() && bool(callback_); }

    void open_app_settings() const { openAppSettings(); }

    void openAppSettings() const {
        JNIEnv* jenv = env();
        if (!jenv || !activity_ || !activity_->clazz) return;

        jclass intent_class = find_class(jenv, "android/content/Intent");
        if (!intent_class) { clear_exception(jenv); return; }
        jclass uri_class = find_class(jenv, "android/net/Uri");
        if (!uri_class) {
            clear_exception(jenv);
            jenv->DeleteLocalRef(intent_class);
            return;
        }

        jmethodID intent_ctor = jenv->GetMethodID(intent_class, "<init>", "(Ljava/lang/String;)V");
        jmethodID set_data = jenv->GetMethodID(
            intent_class, "setData", "(Landroid/net/Uri;)Landroid/content/Intent;");
        jmethodID parse = jenv->GetStaticMethodID(
            uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
        jmethodID start = method(jenv, "startActivity", "(Landroid/content/Intent;)V");
        if (!intent_ctor || !set_data || !parse || !start) {
            clear_exception(jenv);
            jenv->DeleteLocalRef(uri_class);
            jenv->DeleteLocalRef(intent_class);
            return;
        }

        jstring action = jenv->NewStringUTF("android.settings.APPLICATION_DETAILS_SETTINGS");
        if (!action) {
            clear_exception(jenv);
            jenv->DeleteLocalRef(uri_class);
            jenv->DeleteLocalRef(intent_class);
            return;
        }
        jobject intent = jenv->NewObject(intent_class, intent_ctor, action);
        jenv->DeleteLocalRef(action);

        const std::string uri = "package:" + package_name();
        jstring juri = jenv->NewStringUTF(uri.c_str());
        jobject parsed = nullptr;
        if (intent && juri) {
            parsed = jenv->CallStaticObjectMethod(uri_class, parse, juri);
        }
        if (juri) jenv->DeleteLocalRef(juri);
        if (intent && parsed) {
            jobject result = jenv->CallObjectMethod(intent, set_data, parsed);
            if (result) jenv->DeleteLocalRef(result);  // setData devolve o Intent
            jenv->CallVoidMethod(activity_->clazz, start, intent);
        }
        if (jenv->ExceptionCheck()) clear_exception(jenv);

        if (parsed) jenv->DeleteLocalRef(parsed);
        if (intent) jenv->DeleteLocalRef(intent);
        jenv->DeleteLocalRef(uri_class);
        jenv->DeleteLocalRef(intent_class);
    }

    std::string package_name() const {
        JNIEnv* jenv = env();
        if (!jenv || !activity_ || !activity_->clazz || !activity_class_) return {};
        jmethodID get_name = method(jenv, "getPackageName", "()Ljava/lang/String;");
        if (!get_name) { clear_exception(jenv); return {}; }
        jstring name = static_cast<jstring>(
            jenv->CallObjectMethod(activity_->clazz, get_name));
        if (!name) { clear_exception(jenv); return {}; }
        const char* chars = jenv->GetStringUTFChars(name, nullptr);
        std::string result = chars ? chars : "";
        if (chars) jenv->ReleaseStringUTFChars(name, chars);
        jenv->DeleteLocalRef(name);
        if (jenv->ExceptionCheck()) clear_exception(jenv);
        return result;
    }

    static constexpr int kRequestCode = 0x4e415050;  // 'NAPP'

private:
    explicit Permissions(ANativeActivity* activity) : activity_(activity) {
        JNIEnv* jenv = env();
        if (jenv) cache_global_refs(jenv);
    }

    static JNIEnv* env_for(ANativeActivity* activity) {
        if (!activity || !activity->vm) return nullptr;
        JNIEnv* jenv = nullptr;
        const jint status = activity->vm->GetEnv(
            reinterpret_cast<void**>(&jenv), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            if (activity->vm->AttachCurrentThread(&jenv, nullptr) != JNI_OK) return nullptr;
        } else if (status != JNI_OK) {
            return nullptr;
        }
        return jenv;
    }

    JNIEnv* env() const { return env_for(activity_); }

    jclass activity_class() const { return activity_class_; }

    bool was_requested(std::string_view permission) const {
        return requested_.find(std::string(permission)) != requested_.end();
    }

    jmethodID method(JNIEnv* jenv, const char* name, const char* signature) const {
        if (!jenv || !activity_class_) return nullptr;
        return jenv->GetMethodID(activity_class_, name, signature);
    }

    static jclass find_class(JNIEnv* jenv, const char* name) {
        return jenv->FindClass(name);  // chamador deve DeleteLocalRef
    }

    static void clear_exception(JNIEnv* jenv) {
        if (jenv && jenv->ExceptionCheck()) jenv->ExceptionClear();
    }

    void cache_global_refs(JNIEnv* jenv) {
        if (!activity_ || !activity_->clazz || activity_class_) return;
        jclass local = jenv->GetObjectClass(activity_->clazz);
        if (!local) { clear_exception(jenv); return; }
        activity_class_ = static_cast<jclass>(jenv->NewGlobalRef(local));
        jenv->DeleteLocalRef(local);
    }

public:
    /// Libera refs globais. Chamado pelo App::on_destroy().
    void shutdown() {
        JNIEnv* jenv = env();
        if (jenv) release_global_refs(jenv);
        pending_.clear();
        callback_ = nullptr;
        activity_class_ = nullptr;
        activity_ = nullptr;
        response_received_ = false;
        awaiting_response_ = false;
        snapshot_.clear();
    }

private:
    void release_global_refs(JNIEnv* jenv) {
        if (activity_class_) {
            jenv->DeleteGlobalRef(activity_class_);
            activity_class_ = nullptr;
        }
    }

    ANativeActivity* activity_ = nullptr;
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
