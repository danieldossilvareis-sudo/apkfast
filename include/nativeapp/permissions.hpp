// nativeapp/permissions.hpp — Permissões em runtime para NativeActivity.
//
// Uso:
//   auto& p = Permissions::instance(app->activity);
//   auto r = p.request({"android.permission.CAMERA"});
//   if (r.all_granted) ...
//   else if (r.should_show_rationale) p.openAppSettings();
//
// Requer: -landroid (já incluído pelo apkfast).

#pragma once

#include <android/native_activity.h>
#include <jni.h>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nativeapp {

struct PermissionResult {
    bool all_granted = false;
    std::vector<std::string> denied;
    bool should_show_rationale = false;
};

class Permissions {
public:
    static constexpr int kRequestCode = 0x4e415050;  // 'NAPP'

    static Permissions& instance(ANativeActivity* activity) {
        static Permissions* singleton = nullptr;
        if (!singleton) singleton = new Permissions(activity);
        return *singleton;
    }

    // Em API < 23 permissões são concedidas no install; retorna true.
    bool runtime_permissions_available() const {
        JNIEnv* jenv = env();
        if (!jenv) return false;
        jclass build = jenv->FindClass("android/os/Build$VERSION");
        if (!build) return false;
        jfieldID field = jenv->GetStaticFieldID(build, "SDK_INT", "I");
        const int sdk = field ? jenv->GetStaticIntField(build, field) : 0;
        jenv->DeleteLocalRef(build);
        return sdk >= 23;
    }

    bool is_granted(std::string_view permission) const {
        JNIEnv* jenv = env();
        if (!jenv) return false;
        if (!runtime_permissions_available()) return true;
        jstring jperm = jenv->NewStringUTF(permission.data());
        jint result = jenv->CallIntMethod(
            activity_->clazz,
            jenv->GetMethodID(activity_class(), "checkSelfPermission", "(Ljava/lang/String;)I"),
            jperm);
        jenv->DeleteLocalRef(jperm);
        return result == 0;  // PackageManager.PERMISSION_GRANTED
    }

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
            return result;
        }

        std::vector<std::string> pending;
        for (const auto& p : permissions) {
            if (!is_granted(p)) pending.push_back(p);
        }
        if (pending.empty()) {
            result.all_granted = true;
            return result;
        }

        jclass string_class = jenv->FindClass("java/lang/String");
        jobjectArray array = jenv->NewObjectArray(
            static_cast<jsize>(pending.size()), string_class, nullptr);
        for (size_t i = 0; i < pending.size(); ++i) {
            jstring s = jenv->NewStringUTF(pending[i].c_str());
            jenv->SetObjectArrayElement(array, static_cast<jsize>(i), s);
            jenv->DeleteLocalRef(s);
        }
        jenv->DeleteLocalRef(string_class);

        jenv->CallVoidMethod(
            activity_->clazz,
            jenv->GetMethodID(activity_class(), "requestPermissions", "([Ljava/lang/String;I)V"),
            array, kRequestCode);
        jenv->DeleteLocalRef(array);

        // Resultado chega em onRequestPermissionsResult; use is_granted()
        // depois do callback. Devolvemos os pendentes para quem chama.
        result.denied = pending;
        jmethodID rationale = jenv->GetMethodID(
            activity_class(), "shouldShowRequestPermissionRationale", "(Ljava/lang/String;)Z");
        for (const auto& p : pending) {
            jstring jp = jenv->NewStringUTF(p.c_str());
            if (jenv->CallBooleanMethod(activity_->clazz, rationale, jp)) {
                result.should_show_rationale = true;
            }
            jenv->DeleteLocalRef(jp);
        }
        return result;
    }

    void openAppSettings() {
        JNIEnv* jenv = env();
        if (!jenv) return;

        jclass intent_class = jenv->FindClass("android/content/Intent");
        jstring action = jenv->NewStringUTF("android.settings.APPLICATION_DETAILS_SETTINGS");
        jobject intent = jenv->NewObject(
            intent_class,
            jenv->GetMethodID(intent_class, "<init>", "(Ljava/lang/String;)V"),
            action);
        jenv->DeleteLocalRef(action);

        jclass uri_class = jenv->FindClass("android/net/Uri");
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
            jenv->GetMethodID(activity_class(), "startActivity", "(Landroid/content/Intent;)V"),
            intent);

        jenv->DeleteLocalRef(parsed);
        jenv->DeleteLocalRef(intent);
        jenv->DeleteLocalRef(uri_class);
        jenv->DeleteLocalRef(intent_class);
    }

    // ----- API assíncrona sem Java ----------------------------------------
    // Registra um callback que será chamado assim que todas as permissões
    // pedidas forem concedidas (ou quando o usuário negar). Como NativeActivity
    // não tem Java para sobrescrever onRequestPermissionsResult, fazemos
    // polling de is_granted() a cada chamada de update() no loop de render.
    using Callback = std::function<void(const PermissionResult&)>;

    void request_async(const std::vector<std::string>& permissions, Callback callback) {
        auto result = request(permissions);
        pending_ = permissions;
        callback_ = std::move(callback);
        if (result.all_granted && callback_) {
            Callback cb = callback_;
            callback_ = nullptr;
            pending_.clear();
            cb(result);
        }
    }

    // Chame uma vez por frame (ou no on_resume). Dispara o callback quando o
    // estado das permissões mudar.
    // Chamado pelo App::on_resume(): a Activity voltou do diálogo de permissão.
    void notify_activity_result() { response_received_ = true; }

    void update() {
        if (!callback_ || pending_.empty()) return;
        if (!env() || !response_received_) return;

        PermissionResult result;
        result.all_granted = true;
        for (const auto& permission : pending_) {
            if (is_granted(permission)) continue;
            result.all_granted = false;
            result.denied.push_back(permission);
            jmethodID rationale = env()->GetMethodID(
                activity_class(), "shouldShowRequestPermissionRationale", "(Ljava/lang/String;)Z");
            jstring jp = env()->NewStringUTF(permission.c_str());
            if (env()->CallBooleanMethod(activity_->clazz, rationale, jp)) {
                result.should_show_rationale = true;
            }
            env()->DeleteLocalRef(jp);
        }
        response_received_ = false;
        Callback cb = callback_;
        callback_ = nullptr;
        pending_.clear();
        cb(result);
    }

    bool has_pending() const { return !pending_.empty(); }

    std::string package_name() const {
        JNIEnv* jenv = env();
        if (!jenv) return {};
        jstring name = static_cast<jstring>(jenv->CallObjectMethod(
            activity_->clazz,
            jenv->GetMethodID(activity_class(), "getPackageName", "()Ljava/lang/String;")));
        const char* chars = jenv->GetStringUTFChars(name, nullptr);
        std::string result(chars);
        jenv->ReleaseStringUTFChars(name, chars);
        jenv->DeleteLocalRef(name);
        return result;
    }

private:
    explicit Permissions(ANativeActivity* activity) : activity_(activity) {}

    // android_main() roda numa thread criada pelo NativeActivity, diferente
    // da thread da UI. activity_->env só é válido na thread da UI, então
    // anexamos a thread atual ao JavaVM antes de qualquer chamada JNI.
    JNIEnv* env() const {
        if (!activity_ || !activity_->vm) return nullptr;
        JNIEnv* jenv = nullptr;
        const jint status = activity_->vm->GetEnv(
            reinterpret_cast<void**>(&jenv), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            if (activity_->vm->AttachCurrentThread(&jenv, nullptr) != JNI_OK) {
                return nullptr;
            }
        } else if (status != JNI_OK) {
            return nullptr;
        }
        return jenv;
    }

    jclass activity_class() const {
        JNIEnv* jenv = env();
        return jenv && activity_->clazz ? jenv->GetObjectClass(activity_->clazz) : nullptr;
    }

    ANativeActivity* activity_;
    std::vector<std::string> pending_;
    Callback callback_;
    bool response_received_ = false;
};

}  // namespace nativeapp
