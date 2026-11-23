// nativeapp/jni_bitmap.hpp — acesso JNI compartilhado para decode de imagens.
//
// O Texture não conhece App, então o App injeta aqui o ANativeActivity uma
// única vez (ao lado do Assets/TextInput). O decode WebP usa BitmapFactory +
// AndroidBitmap_*; SVG é puro C++ e não precisa disto.

#pragma once

#include <android/native_activity.h>
#include <jni.h>

namespace nativeapp {

class JniBitmap {
public:
    static JniBitmap& instance(ANativeActivity* activity = nullptr) {
        static JniBitmap self;
        if (activity) self.set_activity(activity);
        return self;
    }

    void set_activity(ANativeActivity* activity) {
        if (activity_ref_ && env_) {
            env_->DeleteGlobalRef(activity_ref_);
            activity_ref_ = nullptr;
        }
        native_activity_ = activity;
        env_ = nullptr;
        if (!activity || !activity->vm) return;

        if (activity->vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) != JNI_OK) {
            env_ = nullptr;
        }
        if (!env_ && activity->vm->AttachCurrentThread(&env_, nullptr) != JNI_OK) {
            env_ = nullptr;
        }
        if (env_ && activity->clazz) {
            activity_ref_ = env_->NewGlobalRef(activity->clazz);
        }
    }

    JNIEnv* env() const { return env_; }
    jobject activity() const { return activity_ref_; }
    ANativeActivity* native_activity() const { return native_activity_; }

    void shutdown() {
        if (env_ && activity_ref_) {
            env_->DeleteGlobalRef(activity_ref_);
        }
        activity_ref_ = nullptr;
        env_ = nullptr;
        native_activity_ = nullptr;
    }

private:
    JniBitmap() = default;
    JNIEnv* env_ = nullptr;
    jobject activity_ref_ = nullptr;
    ANativeActivity* native_activity_ = nullptr;
};

}  // namespace nativeapp
