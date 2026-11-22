// nativeapp/text_input.hpp — Teclado virtual (soft keyboard) para apps nativos.
//
// Implementação 100% JNI com classes do framework Android — NÃO exige classes
// Java customizadas, funciona com NativeActivity, raylib e SDL2/SDL3.
//
// O texto é obtido por polling (update()) — você decide quando ler.
//
// Uso:
//   auto& input = nativeapp::TextInput::instance(env, activity);
//   input.show("Digite seu nome:");
//   input.update();                       // a cada frame
//   if (input.is_done()) { auto text = input.text(); ... }

#pragma once

#include <android/log.h>
#include <jni.h>

#include <functional>
#include <string>

namespace nativeapp {

class TextInput {
public:
    static TextInput& instance(JNIEnv* env, jobject activity) {
        static TextInput* singleton = nullptr;
        if (!singleton) singleton = new TextInput(env, activity);
        if (activity && singleton->activity_ != activity) singleton->recreate(env, activity);
        return *singleton;
    }

    bool available() const { return edit_text_ != nullptr && env_ != nullptr; }

    /// Mostra o teclado virtual ancorado em um EditText invisível.
    void show(const std::string& hint = "") {
        if (!env_ || !activity_) return;
        if (!edit_text_) create_edit_text();
        if (!edit_text_) return;

        if (!hint.empty()) set_hint(hint);

        env_->CallBooleanMethod(edit_text_, m("requestFocus", "()Z"));
        jclass imm_class = env_->FindClass("android/view/inputmethod/InputMethodManager");
        if (!imm_class) return;
        jmethodID get_service = env_->GetMethodID(
            find_class("android/content/Context"), "getSystemService",
            "(Ljava/lang/String;)Ljava/lang/Object;");
        jstring service_name = env_->NewStringUTF("input_method");
        jobject service = env_->CallObjectMethod(activity_, get_service, service_name);
        env_->DeleteLocalRef(service_name);
        if (service) {
            jmethodID show_soft = env_->GetMethodID(
                imm_class, "showSoftInput", "(Landroid/view/View;I)Z");
            if (show_soft) {
                env_->CallBooleanMethod(service, show_soft, edit_text_, 0);
            }
            env_->DeleteLocalRef(service);
        }
        env_->DeleteLocalRef(imm_class);
        shown_ = true;
    }

    void hide() {
        if (!env_ || !edit_text_) return;
        jclass imm_class = env_->FindClass("android/view/inputmethod/InputMethodManager");
        if (!imm_class) return;
        jmethodID get_service = env_->GetMethodID(
            find_class("android/content/Context"), "getSystemService",
            "(Ljava/lang/String;)Ljava/lang/Object;");
        jstring name = env_->NewStringUTF("input_method");
        jobject service = env_->CallObjectMethod(activity_, get_service, name);
        env_->DeleteLocalRef(name);
        if (service) {
            jmethodID hide_soft = env_->GetMethodID(
                imm_class, "hideSoftInputFromWindow", "(Landroid/os/IBinder;I)Z");
            jobject token = env_->CallObjectMethod(edit_text_, m("getWindowToken", "()Landroid/os/IBinder;"));
            if (hide_soft && token) {
                env_->CallBooleanMethod(service, hide_soft, token, 0);
            }
            if (token) env_->DeleteLocalRef(token);
            env_->DeleteLocalRef(service);
        }
        env_->DeleteLocalRef(imm_class);
        shown_ = false;
    }

    void set_text(const std::string& text) {
        if (!env_ || !edit_text_) return;
        jstring value = env_->NewStringUTF(text.c_str());
        env_->CallVoidMethod(edit_text_, m("setText", "(Ljava/lang/CharSequence;)V"), value);
        env_->DeleteLocalRef(value);
    }

    void set_hint(const std::string& hint) {
        if (!env_ || !edit_text_) return;
        jstring value = env_->NewStringUTF(hint.c_str());
        env_->CallVoidMethod(edit_text_, m("setHint", "(Ljava/lang/CharSequence;)V"), value);
        env_->DeleteLocalRef(value);
    }

    void set_multiline(bool multiline) {
        if (!env_ || !edit_text_) return;
        env_->CallVoidMethod(edit_text_, m("setSingleLine", "(Z)V"), multiline ? JNI_FALSE : JNI_TRUE);
    }

    /// Chamado a cada frame. Detecta mudanças de texto e ENTER.
    void update() {
        if (!env_ || !edit_text_ || !shown_) return;
        const std::string current = text();
        if (current != last_text_) {
            last_text_ = current;
            if (callback_) callback_(current, false);
        }
    }

    /// Chame no seu on_key(); retorna true se o evento foi consumido.
    bool on_key(int32_t keycode, bool down) {
        if (!down || !shown_) return false;
        constexpr int32_t KEYCODE_ENTER = 66;
        constexpr int32_t KEYCODE_NUMPAD_ENTER = 160;
        if (keycode == KEYCODE_ENTER || keycode == KEYCODE_NUMPAD_ENTER) {
            done_ = true;
            hide();
            if (callback_) callback_(text(), true);
            return true;
        }
        return false;
    }

    std::string text() const {
        if (!env_ || !edit_text_) return {};
        jstring value = static_cast<jstring>(
            env_->CallObjectMethod(edit_text_, m("getText", "()Ljava/lang/CharSequence;")));
        if (!value) return {};
        const char* chars = env_->GetStringUTFChars(value, nullptr);
        std::string result = chars ? chars : "";
        if (chars) env_->ReleaseStringUTFChars(value, chars);
        env_->DeleteLocalRef(value);
        return result;
    }

    bool is_shown() const { return shown_; }
    bool is_done() const { return done_; }
    void clear_done() { done_ = false; }

    using Callback = std::function<void(const std::string&, bool /*done*/)>;
    void set_callback(Callback callback) { callback_ = std::move(callback); }

    void destroy() {
        if (!env_ || !edit_text_) return;
        hide();
        env_->DeleteGlobalRef(edit_text_);
        edit_text_ = nullptr;
    }

private:
    explicit TextInput(JNIEnv* env, jobject activity) : env_(env), activity_(activity) {
        create_edit_text();
    }

    void recreate(JNIEnv* env, jobject activity) {
        destroy();
        env_ = env;
        activity_ = activity;
        create_edit_text();
    }

    jclass find_class(const char* name) const {
        return env_ ? env_->FindClass(name) : nullptr;
    }

    jmethodID m(const char* name, const char* signature) const {
        if (!env_ || !edit_text_) return nullptr;
        return env_->GetMethodID(env_->GetObjectClass(edit_text_), name, signature);
    }

    void create_edit_text() {
        if (!env_ || !activity_) return;
        jclass edit_class = env_->FindClass("android/widget/EditText");
        if (!edit_class) {
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-text", "EditText nao encontrado");
            return;
        }
        jmethodID constructor = env_->GetMethodID(edit_class, "<init>", "(Landroid/content/Context;)V");
        jobject local = env_->NewObject(edit_class, constructor, activity_);
        env_->DeleteLocalRef(edit_class);
        if (!local) return;
        edit_text_ = static_cast<jobject>(env_->NewGlobalRef(local));
        env_->DeleteLocalRef(local);

        ConfigurarView();
    }

    void ConfigurarView() {
        // Background transparente, texto branco, single-line, alpha baixo.
        jclass color = env_->FindClass("android/graphics/Color");
        jmethodID transparent = env_->GetStaticMethodID(color, "parseColor", "(Ljava/lang/String;)I");
        jstring black = env_->NewStringUTF("#00000000");
        jint argb = env_->CallStaticIntMethod(color, transparent, black);
        env_->DeleteLocalRef(black);
        env_->DeleteLocalRef(color);
        env_->CallVoidMethod(edit_text_, m("setBackgroundColor", "(I)V"), argb);

        env_->CallVoidMethod(edit_text_, m("setSingleLine", "(Z)V"), JNI_TRUE);

        jclass view_group = env_->FindClass("android/view/ViewGroup$LayoutParams");
        if (view_group) {
            jfieldID wrap = env_->GetStaticFieldID(
                env_->FindClass("android/view/ViewGroup$LayoutParams"),
                "WRAP_CONTENT", "I");
            jint width = env_->GetStaticIntField(view_group, wrap);
            (void)width;
            env_->DeleteLocalRef(view_group);
        }

        // Adiciona ao content view da Activity (1x1 px, invisível mas focável).
        jclass frame_params = env_->FindClass("android/widget/FrameLayout$LayoutParams");
        if (frame_params) {
            jmethodID ctor = env_->GetMethodID(frame_params, "<init>", "(II)V");
            jobject params = env_->NewObject(frame_params, ctor, 1, 1);
            env_->CallVoidMethod(
                activity_,
                env_->GetMethodID(find_class("android/app/Activity"), "addContentView",
                                  "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V"),
                edit_text_, params);
            env_->DeleteLocalRef(params);
            env_->DeleteLocalRef(frame_params);
        }
    }

    JNIEnv* env_ = nullptr;
    jobject activity_ = nullptr;
    jobject edit_text_ = nullptr;
    bool shown_ = false;
    bool done_ = false;
    std::string last_text_;
    Callback callback_;
};

}  // namespace nativeapp
