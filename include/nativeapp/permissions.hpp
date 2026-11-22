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

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nativeapp {

enum class PermissionState {
    Unknown,
    Granted,
    Denied,           ///< negado, mas com "rationale" (pode perguntar de novo)
    PermanentlyDenied ///< negado e "don't ask again" (só via configurações)
};

struct PermissionResult {
    bool all_granted = false;
    std::vector<std::string> granted;
    std::vector<std::string> denied;
    bool should_show_rationale = false;
    bool permanently_denied = false;

    bool empty() const { return granted.empty() && denied.empty(); }
};

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
        JNIEnv* jenv = env();
        if (jenv) {
            release_global_refs(jenv);
        }
        activity_ = activity;
        jenv = env();
        if (jenv) cache_global_refs(jenv);
        pending_.clear();
        callback_ = nullptr;
        response_received_ = false;
        snapshot_.clear();
    }

    /// API < 23: permissões concedidas no install.
    bool runtime_permissions_available() const {
        JNIEnv* jenv = env();
        if (!jenv) return false;
        jclass build = find_class(jenv, "android/os/Build$VERSION");
        if (!build) return false;
        jfieldID field = jenv->GetStaticFieldID(build, "SDK_INT", "I");
        const int sdk = field ? jenv->GetStaticIntField(build, field) : 0;
        jenv->DeleteLocalRef(build);
        return sdk >= 23;
    }

    /// Retorna o estado detalhado de uma permissão.
    PermissionState state_of(std::string_view permission) const {
        JNIEnv* jenv = env();
        if (!jenv) return PermissionState::Unknown;
        if (!runtime_permissions_available()) return PermissionState::Granted;

        jstring jperm = jenv->NewStringUTF(permission.data());
        const jint check = jenv->CallIntMethod(
            activity_->clazz,
            method(jenv, "checkSelfPermission", "(Ljava/lang/String;)I"),
            jperm);
        jenv->DeleteLocalRef(jperm);
        if (check == 0) return PermissionState::Granted;

        // Depois da primeira negação, rationale=true significa que ainda pode
        // perguntar; rationale=false + já negada = "don't ask again".
        // "Nunca pedida" não é permanente: rationale=false antes da primeira
        // solicitação é o comportamento normal do Android.
        if (!was_requested(permission)) return PermissionState::Denied;

        jstring jrationale = jenv->NewStringUTF(permission.data());
        const jboolean rationale = jenv->CallBooleanMethod(
            activity_->clazz,
            method(jenv, "shouldShowRequestPermissionRationale", "(Ljava/lang/String;)Z"),
            jrationale);
        jenv->DeleteLocalRef(jrationale);

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
        if (!jenv) {
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
        jobjectArray array = jenv->NewObjectArray(
            static_cast<jsize>(pending.size()), string_class, nullptr);
        jenv->DeleteLocalRef(string_class);

        for (size_t i = 0; i < pending.size(); ++i) {
            jstring item = jenv->NewStringUTF(pending[i].c_str());
            jenv->SetObjectArrayElement(array, static_cast<jsize>(i), item);
            jenv->DeleteLocalRef(item);
        }

        jenv->CallVoidMethod(
            activity_->clazz,
            method(jenv, "requestPermissions", "([Ljava/lang/String;I)V"),
            array, kRequestCode);
        jenv->DeleteLocalRef(array);

        // Marca como pedidas ANTES de consultar o rationale.
        for (const auto& permission : pending) requested_.insert(permission);

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
        if (!jenv) return;

        jclass intent_class = find_class(jenv, "android/content/Intent");
        jstring action = jenv->NewStringUTF("android.settings.APPLICATION_DETAILS_SETTINGS");
        jobject intent = jenv->NewObject(
            intent_class,
            jenv->GetMethodID(intent_class, "<init>", "(Ljava/lang/String;)V"),
            action);
        jenv->DeleteLocalRef(action);

        jclass uri_class = find_class(jenv, "android/net/Uri");
        std::string uri = "package:" + package_name();
        jstring juri = jenv->NewStringUTF(uri.c_str());
        jobject parsed = jenv->CallStaticObjectMethod(
            uri_class,
            jenv->GetStaticMethodID(uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"),
            juri);
        jenv->DeleteLocalRef(juri);

        jenv->CallObjectMethod(
            intent,
            jenv->GetMethodID(intent_class, "setData", "(Landroid/net/Uri;)Landroid/content/Intent;"),
            parsed);
        jenv->CallVoidMethod(
            activity_->clazz,
            method(jenv, "startActivity", "(Landroid/content/Intent;)V"),
            intent);

        jenv->DeleteLocalRef(parsed);
        jenv->DeleteLocalRef(intent);
        jenv->DeleteLocalRef(uri_class);
        jenv->DeleteLocalRef(intent_class);
    }

    std::string package_name() const {
        JNIEnv* jenv = env();
        if (!jenv) return {};
        jstring name = static_cast<jstring>(jenv->CallObjectMethod(
            activity_->clazz, method(jenv, "getPackageName", "()Ljava/lang/String;")));
        if (!name) return {};
        const char* chars = jenv->GetStringUTFChars(name, nullptr);
        std::string result = chars ? chars : "";
        if (chars) jenv->ReleaseStringUTFChars(name, chars);
        jenv->DeleteLocalRef(name);
        return result;
    }

    static constexpr int kRequestCode = 0x4e415050;  // 'NAPP'

private:
    explicit Permissions(ANativeActivity* activity) : activity_(activity) {
        JNIEnv* jenv = env();
        if (jenv) cache_global_refs(jenv);
    }

    JNIEnv* env() const {
        if (!activity_ || !activity_->vm) return nullptr;
        JNIEnv* jenv = nullptr;
        const jint status = activity_->vm->GetEnv(
            reinterpret_cast<void**>(&jenv), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            if (activity_->vm->AttachCurrentThread(&jenv, nullptr) != JNI_OK) return nullptr;
        } else if (status != JNI_OK) {
            return nullptr;
        }
        return jenv;
    }

    jclass activity_class() const { return activity_class_; }

    bool was_requested(std::string_view permission) const {
        return requested_.find(std::string(permission)) != requested_.end();
    }

    jmethodID method(JNIEnv* jenv, const char* name, const char* signature) const {
        if (!activity_class_) return nullptr;
        return jenv->GetMethodID(activity_class_, name, signature);
    }

    static jclass find_class(JNIEnv* jenv, const char* name) {
        jclass local = jenv->FindClass(name);
        return local;  // chamador deve DeleteLocalRef
    }

    void cache_global_refs(JNIEnv* jenv) {
        if (!activity_ || !activity_->clazz) return;
        jclass local = jenv->GetObjectClass(activity_->clazz);
        if (local) {
            activity_class_ = static_cast<jclass>(jenv->NewGlobalRef(local));
            jenv->DeleteLocalRef(local);
        }
    }

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
