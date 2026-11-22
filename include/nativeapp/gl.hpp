// nativeapp/gl.hpp — Cena OpenGL ES 2/3 pronta para NativeActivity.
//
// Exemplo:
//   class App : public nativeapp::App {
//     nativeapp::GLES2Scene scene_;
//     void on_ready() override { scene_.init(window()); }
//     void on_resize(int w, int h) override { scene_.resize(w, h); }
//     void on_draw() override { scene_.render(); }
//   };

#pragma once

#include <android/log.h>
#include <android/native_window.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace nativeapp {

class GLError : public std::runtime_error {
public:
    explicit GLError(const std::string& message) : std::runtime_error(message) {}
};

class GLES2Scene {
public:
    struct Vertex {
        float x, y;
        float r, g, b;
    };

    GLES2Scene() = default;
    ~GLES2Scene() { destroy(); }

    GLES2Scene(const GLES2Scene&) = delete;
    GLES2Scene& operator=(const GLES2Scene&) = delete;

    void init(ANativeWindow* window) {
        const EGLint attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NONE,
        };
        EGLint num_configs = 0;
        display_ = egl_get_display();
        if (display_ == EGL_NO_DISPLAY) throw GLError("eglGetDisplay falhou");
        if (!eglInitialize(display_, nullptr, nullptr)) throw GLError("eglInitialize falhou");
        if (!eglChooseConfig(display_, attribs, &config_, 1, &num_configs) || num_configs < 1) {
            throw GLError("eglChooseConfig falhou");
        }

        const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attribs);
        if (context_ == EGL_NO_CONTEXT) throw GLError("eglCreateContext falhou");

        surface_ = eglCreateWindowSurface(display_, config_, window, nullptr);
        if (surface_ == EGL_NO_SURFACE) throw GLError("eglCreateWindowSurface falhou");
        make_current();

        glEnable(GL_DEPTH_TEST);
        glClearColor(0.08f, 0.08f, 0.12f, 1.0f);

        program_ = create_program(kVertexShader, kFragmentShader);
        position_loc_ = glGetAttribLocation(program_, "a_position");
        color_loc_ = glGetAttribLocation(program_, "a_color");
        offset_loc_ = glGetUniformLocation(program_, "u_offset");

        glGenBuffers(1, &vbo_);
        set_vertices(default_vertices());
        initialized_ = true;
    }

    void set_vertices(const std::vector<Vertex>& vertices) {
        make_current();
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
                     vertices.data(), GL_STATIC_DRAW);
    }

    void resize(int width, int height) {
        width_ = width;
        height_ = height;
        make_current();
        glViewport(0, 0, width_, height_);
    }

    void set_offset(float x, float y) {
        offset_[0] = x;
        offset_[1] = y;
    }

    void set_clear_color(float r, float g, float b, float a = 1.0f) {
        clear_[0] = r;
        clear_[1] = g;
        clear_[2] = b;
        clear_[3] = a;
    }

    void render() {
        if (!initialized_) return;
        make_current();

        glClearColor(clear_[0], clear_[1], clear_[2], clear_[3]);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(program_);
        glUniform2f(offset_loc_, offset_[0], offset_[1]);

        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        const GLsizei stride = static_cast<GLsizei>(sizeof(Vertex));
        glEnableVertexAttribArray(position_loc_);
        glVertexAttribPointer(position_loc_, 2, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(Vertex, x)));
        glEnableVertexAttribArray(color_loc_);
        glVertexAttribPointer(color_loc_, 3, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(Vertex, r)));
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertex_count_));

        // Drena erros GL/EGL para diagnóstico (chamadas são baratas).
        const GLenum gl_error = glGetError();
        const EGLint egl_error = eglGetError();
        if (gl_error != GL_NO_ERROR || egl_error != EGL_SUCCESS) {
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                                "glGetError=0x%x eglGetError=0x%x",
                                gl_error, egl_error);
        }

        if (!eglSwapBuffers(display_, surface_)) {
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                                "eglSwapBuffers falhou: 0x%x", eglGetError());
        }
    }

    void destroy() {
        if (!initialized_) return;
        make_current();
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (program_) glDeleteProgram(program_);
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_) eglDestroySurface(display_, surface_);
        if (context_) eglDestroyContext(display_, context_);
        if (display_) eglTerminate(display_);
        initialized_ = false;
    }

private:
    static EGLDisplay egl_get_display() {
        return eglGetDisplay(static_cast<EGLNativeDisplayType>(EGL_DEFAULT_DISPLAY));
    }

    void make_current() {
        if (display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT &&
            surface_ != EGL_NO_SURFACE) {
            eglMakeCurrent(display_, surface_, surface_, context_);
        }
    }

    static GLuint create_program(const char* vs, const char* fs) {
        GLuint vertex = compile_shader(GL_VERTEX_SHADER, vs);
        GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fs);
        GLuint program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[1024];
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            throw GLError(std::string("Link do shader falhou: ") + log);
        }
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return program;
    }

    static GLuint compile_shader(GLenum type, const char* source) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            char log[1024];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            throw GLError(std::string("Compilação do shader falhou: ") + log);
        }
        return shader;
    }

    static std::vector<Vertex> default_vertices() {
        return {
            {-0.5f, -0.5f, 1.0f, 0.3f, 0.3f},
            { 0.5f, -0.5f, 0.3f, 1.0f, 0.3f},
            { 0.0f,  0.5f, 0.3f, 0.3f, 1.0f},
        };
    }

    static constexpr const char* kVertexShader = R"(
        attribute vec2 a_position;
        attribute vec3 a_color;
        uniform vec2 u_offset;
        varying vec3 v_color;
        void main() {
            v_color = a_color;
            gl_Position = vec4(a_position + u_offset, 0.0, 1.0);
        }
    )";

    static constexpr const char* kFragmentShader = R"(
        precision mediump float;
        varying vec3 v_color;
        void main() { gl_FragColor = vec4(v_color, 1.0); }
    )";

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    GLuint program_ = 0;
    GLuint vbo_ = 0;
    GLint position_loc_ = -1;
    GLint color_loc_ = -1;
    GLint offset_loc_ = -1;
    std::array<float, 2> offset_{0.0f, 0.0f};
    std::array<float, 4> clear_{0.08f, 0.08f, 0.12f, 1.0f};
    int width_ = 0;
    int height_ = 0;
    size_t vertex_count_ = 3;
    bool initialized_ = false;
};

}  // namespace nativeapp
