// nativeapp/text_input.hpp — Teclado virtual sem EditText e sem Java custom.
//
// Estratégia (funciona em QUALQUER dispositivo Android, API 1+):
//   1. InputMethodManager.toggleSoftInput(SHOW_FORCED) — NÃO precisa de View.
//   2. O IME envia eventos de tecla (AINPUT_EVENT_TYPE_KEY) para a Activity.
//   3. Usamos AKeyEvent_getUnicodeChar() para obter o caractere real
//      (inclui acentos, símbolos e caracteres de IMEs internacionais).
//   4. Backspace/ENTER são tratados explicitamente.
//
// Uso:
//   auto& input = nativeapp::TextInput::instance(env, activity);
//   input.show();                                  // abre o teclado
//   // no seu on_key(code, down, event):
//   input.on_key(code, down, AKeyEvent_getUnicodeChar(event));
//   std::string typed = input.text();
//   input.hide();                                  // fecha o teclado

#pragma once

#include <android/log.h>
#include <android/input.h>
#include <jni.h>

#include <functional>
#include <string>

namespace nativeapp {

class TextInput {
public:
    enum class KeyAction { None, Character, Backspace, Enter };

    static TextInput& instance(JNIEnv* env, jobject activity) {
        static TextInput* singleton = nullptr;
        if (!singleton) singleton = new TextInput(env, activity);
        if (env && activity && singleton->activity_ != activity) {
            singleton->activity_ = activity;
            singleton->env_ = env;
        }
        return *singleton;
    }

    /// Abre o teclado virtual.Não precisa de View/EditText.
    void show() {
        JNIEnv* env = env_;
        if (!env || !activity_) return;

        jclass imm_class = find_class(env, "android/view/inputmethod/InputMethodManager");
        if (!imm_class) {
            log("InputMethodManager nao encontrado");
            return;
        }

        jclass context_class = find_class(env, "android/content/Context");
        jmethodID get_service = env->GetMethodID(
            context_class, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
        jstring name = env->NewStringUTF("input_method");
        jobject service = env->CallObjectMethod(activity_, get_service, name);
        env->DeleteLocalRef(name);

        if (service) {
            // INPUT_METHOD_SERVICE.showSoftInput(view, flags) exige view;
            // toggleSoftInput(showFlags, hideFlags) NÃO exige nenhuma view.
            jmethodID toggle = env->GetMethodID(imm_class, "toggleSoftInput", "(II)V");
            if (toggle) {
                // InputMethodManager.SHOW_FORCED = 2, HIDE_IMPLICIT_ONLY = 1
                env->CallVoidMethod(service, toggle, 2, 0);
                shown_ = true;
            } else {
                log("toggleSoftInput nao encontrado");
            }
            env->DeleteLocalRef(service);
        } else {
            log("servico input_method nao obtido");
        }
        env->DeleteLocalRef(imm_class);
        env->DeleteLocalRef(context_class);
    }

    /// Fecha o teclado virtual.
    void hide() {
        JNIEnv* env = env_;
        if (!env || !activity_) return;

        jclass imm_class = find_class(env, "android/view/inputmethod/InputMethodManager");
        if (!imm_class) return;
        jclass context_class = find_class(env, "android/content/Context");
        jmethodID get_service = env->GetMethodID(
            context_class, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
        jstring name = env->NewStringUTF("input_method");
        jobject service = env->CallObjectMethod(activity_, get_service, name);
        env->DeleteLocalRef(name);

        if (service) {
            jmethodID toggle = env->GetMethodID(imm_class, "toggleSoftInput", "(II)V");
            if (toggle) {
                // SHOW_IMPLICIT = 1? Usamos HIDE_IMPLICIT_ONLY=1 via hideSoftInputFromWindow
                // alternativa: toggleSoftInput(0, HIDE_IMPLICIT_ONLY=1) esconde.
                env->CallVoidMethod(service, toggle, 0, 1);
            }
            env->DeleteLocalRef(service);
        }
        shown_ = false;
        env->DeleteLocalRef(imm_class);
        env->DeleteLocalRef(context_class);
    }

    /// Repasse eventos de tecla do App::on_key. Retorna true se consumiu.
    /// unicode = AKeyEvent_getUnicodeChar(event) (0 para teclas de controle).
    bool on_key(int32_t keycode, bool down, int32_t unicode = 0) {
        if (!down || !shown_) return false;

        // BACK: fecha o teclado
        if (keycode == 4 /* AKEYCODE_BACK */) {
            hide();
            return true;
        }

        // ENTER / DPAD_CENTER / NUM_ENTER: confirma
        if (keycode == 66 /* ENTER */ || keycode == 23 /* DPAD_CENTER */ ||
            keycode == 160 /* NUMPAD_ENTER */) {
            if (callback_) callback_(text_, true);
            hide();
            return true;
        }

        // BACKSPACE / DEL
        if (keycode == 67 /* DEL */ || keycode == 112 /* FORWARD_DEL */) {
            if (!text_.empty()) {
                // remove último codepoint UTF-8
                size_t pos = text_.size() - 1;
                while (pos > 0 && (static_cast<unsigned char>(text_[pos]) & 0xC0) == 0x80) {
                    --pos;
                }
                text_.erase(pos);
                if (callback_) callback_(text_, false);
            }
            return true;
        }

        // Caractere imprimível (unicode fornecido pelo chamador, quando disponível)
        if (unicode > 0 && unicode != 10 && unicode != 13) {
            append_utf8(static_cast<uint32_t>(unicode));
            if (callback_) callback_(text_, false);
            return true;
        }

        // Fallback: mapeia keycode -> caractere ASCII (funciona em todos devices).
        const char ch = ascii_from_keycode(keycode);
        if (ch != 0) {
            text_ += ch;
            if (callback_) callback_(text_, false);
            return true;
        }

        return false;
    }

    void set_text(const std::string& text) { text_ = text; }
    void clear() { text_.clear(); }

    std::string text() const { return text_; }
    bool is_shown() const { return shown_; }
    JNIEnv* env() const { return env_; }

    /// Obtém o Unicode real do evento de tecla, compatível com API 21+.
    /// - API 35+: converte o AInputEvent direto via AInputEvent_toJava.
    /// - API 21..34: reconstrói um android.view.KeyEvent via JNI com os dados
    ///   nativos (todos disponíveis desde API 9) e chama getUnicodeChar(meta).
    int32_t unicode_from_event(AInputEvent* event) const {
        if (!event || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_KEY) return 0;
        JNIEnv* env = env_;
        if (!env) return 0;

#if __ANDROID_API__ >= 35 && !defined(APKFAST_TEST_LEGACY_IME)
        jobject java_event = AInputEvent_toJava(env, event);
        if (java_event) {
            jclass clazz = env->GetObjectClass(java_event);
            jmethodID get_unicode = env->GetMethodID(clazz, "getUnicodeChar", "()I");
            const int32_t unicode = get_unicode
                ? env->CallIntMethod(java_event, get_unicode) : 0;
            env->DeleteLocalRef(clazz);
            env->DeleteLocalRef(java_event);
            if (unicode != 0) return unicode;
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
#endif

        // Fallback universal: monta KeyEvent com a assinatura de 8 argumentos
        // existente desde API 1. getUnicodeChar(meta) resolve acentos e IMEs.
        jclass key_class = env->FindClass("android/view/KeyEvent");
        if (!key_class) { if (env->ExceptionCheck()) env->ExceptionClear(); return 0; }

        jmethodID ctor = env->GetMethodID(
            key_class, "<init>", "(JJIIIIII)V");
        if (!ctor) { env->DeleteLocalRef(key_class); return 0; }

        const jlong down_time = static_cast<jlong>(AKeyEvent_getDownTime(event));
        const jlong event_time = static_cast<jlong>(AKeyEvent_getEventTime(event));
        const jint action = AKeyEvent_getAction(event);
        const jint code = AKeyEvent_getKeyCode(event);
        const jint repeat = AKeyEvent_getRepeatCount(event);
        const jint meta = AKeyEvent_getMetaState(event);
        const jint device_id = AInputEvent_getDeviceId(event);
        const jint scan_code = AKeyEvent_getScanCode(event);

        jobject key_event = env->NewObject(
            key_class, ctor,
            down_time, event_time, action, code, repeat, meta, device_id, scan_code);
        int32_t unicode = 0;
        if (key_event) {
            jmethodID get_unicode = env->GetMethodID(
                key_class, "getUnicodeChar", "(I)I");
            if (get_unicode) {
                unicode = env->CallIntMethod(key_event, get_unicode, meta);
            }
            env->DeleteLocalRef(key_event);
        }
        env->DeleteLocalRef(key_class);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            return 0;
        }
        return unicode;
    }

    using Callback = std::function<void(const std::string&, bool /*done*/)>;
    void set_callback(Callback callback) { callback_ = std::move(callback); }

    void destroy() { shown_ = false; text_.clear(); }

private:
    explicit TextInput(JNIEnv* env, jobject activity) : env_(env), activity_(activity) {}

    static jclass find_class(JNIEnv* env, const char* name) {
        jclass local = env->FindClass(name);
        return local; // chamador faz DeleteLocalRef
    }

    static void append_utf8(std::string& out, uint32_t codepoint) {
        if (codepoint <= 0x7F) {
            out += static_cast<char>(codepoint);
        } else if (codepoint <= 0x7FF) {
            out += static_cast<char>(0xC0 | (codepoint >> 6));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
        } else if (codepoint <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (codepoint >> 12));
            out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (codepoint >> 18));
            out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
    }

    void append_utf8(uint32_t codepoint) { append_utf8(text_, codepoint); }

    static char ascii_from_keycode(int32_t code) {
        // Android KeyEvent.KEYCODE_A..Z = 29..54, 0..9 = 7..16
        if (code >= 29 && code <= 54) return static_cast<char>('a' + (code - 29));
        if (code >= 7 && code <= 16) return static_cast<char>('0' + (code - 7));
        switch (code) {
            case 62: return ' ';   // SPACE
            case 55: return ',';   // COMMA
            case 56: return '.';   // PERIOD
            case 57: return '-';   // MINUS
            case 58: return ';';   // SEMICOLON
            case 59: return '/';   // SLASH
            case 60: return '`';   // GRAVE
            case 68: return '\'';  // QUOTE
            case 69: return '[';   // LEFT_BRACKET
            case 70: return ']';   // RIGHT_BRACKET
            case 71: return '\\';  // BACKSLASH
            default: return 0;
        }
    }

    static void log(const char* message) {
        __android_log_print(ANDROID_LOG_WARN, "nativeapp-text", "%s", message);
    }

    JNIEnv* env_ = nullptr;
    jobject activity_ = nullptr;
    bool shown_ = false;
    std::string text_;
    Callback callback_;
};

}  // namespace nativeapp
