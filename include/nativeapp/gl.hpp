// nativeapp/gl.hpp — OpenGL ES 2/3 de alto nível para NativeActivity.
//
// Camadas:
//   GLContext  — gerencia display/context/surface (sobrevive a cenas)
//   Shader      — compila vertex/fragment, uniforms
//   Mesh        — VBO + IBO reutilizável
//   Texture     — carrega PNG/JPEG de assets/
//   GLES2Scene  — orquestra tudo (implementa Scene)
//   GLES3Scene  — VAO + instancing real (GLES 3.0)

#pragma once

#include <android/log.h>
#include <android/native_window.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>

#include "assets.hpp"
#include "font.hpp"
#include "graphics_types.hpp"
#include "image_codecs.hpp"
#include "math.hpp"

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// As implementações das libs third-party vivem em src/nativeapp_impl.cpp,
// injetado automaticamente pelo builder uma única vez por app.
#include "third_party/stb_image.h"

namespace nativeapp {

class GLError : public std::runtime_error {
public:
    explicit GLError(const std::string& message) : std::runtime_error(message) {}
};

inline void gl_check(const char* operation) {
#ifdef APKFAST_DEBUG
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                            "%s: glGetError=0x%x", operation, error);
    }
#else
    (void)operation;
#endif
}

namespace gl_detail {

/// EGL_CONTEXT_LOST é 0x300E (EGL_CONTEXT_LOST_KHR). Alguns headers antigos
/// só expõem o sufixo _KHR; o literal mantém a detecção equivalente.
#ifdef EGL_CONTEXT_LOST
inline constexpr EGLint kContextLostError = EGL_CONTEXT_LOST;
#else
inline constexpr EGLint kContextLostError = 0x300E;
#endif

inline bool egl_error_is_context_lost(EGLint error) {
    return error == kContextLostError;
}

/// Registra o último erro EGL e devolve true quando ele significa perda de
/// contexto. Chamadas críticas (makeCurrent/swap) usam o retorno para marcar
/// o estado; as demais apenas ignoram o bool.
inline bool egl_log_error(const char* operation) {
    const EGLint error = eglGetError();
    if (error == EGL_SUCCESS) return false;
    const bool lost = egl_error_is_context_lost(error);
    __android_log_print(lost ? ANDROID_LOG_ERROR : ANDROID_LOG_WARN,
                        "nativeapp-gl", "%s: eglGetError=0x%x%s", operation, error,
                        lost ? " (EGL_CONTEXT_LOST)" : "");
    return lost;
}

inline void egl_require(bool success, const char* operation) {
    if (!success) {
        const EGLint error = eglGetError();
        throw GLError(std::string(operation) + " falhou (EGL=" +
                      std::to_string(error) + ")");
    }
}

/// Nomes de objetos GL são válidos apenas no contexto que os criou. A geração
/// muda a cada init/destroy de GLContext para que Textures de fora da cena
/// não reutilizem (nem apaguem) nomes de um contexto morto.
inline uint64_t& context_generation_ref() {
    static uint64_t generation = 0;
    return generation;
}
inline uint64_t context_generation() { return context_generation_ref(); }
inline void bump_context_generation() { ++context_generation_ref(); }

}  // namespace gl_detail

// ============================================================================
// GLContext
// ============================================================================

class GLContext {
public:
    GLContext() = default;
    ~GLContext() { destroy(); }
    GLContext(const GLContext&) = delete;
    GLContext& operator=(const GLContext&) = delete;

    /// Inicialização idempotente e transacional: qualquer falha libera o que
    /// já foi criado, deixando o objeto pronto para uma nova tentativa.
    /// Depois de uma perda de contexto, chame init() (ou GLES2Scene::recreate())
    /// para recuperar: os handles antigos são descartados sem serem usados.
    void init(ANativeWindow* window, int client_version = 2) {
        const bool recovering = context_lost_;
        if (recovering) discard_lost_context();
        else if (display_ != EGL_NO_DISPLAY) destroy();

        requested_version_ = client_version;
        const EGLint attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
            EGL_RENDERABLE_TYPE,
                client_version >= 3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT,
            EGL_NONE,
        };
        EGLint num_configs = 0;
        EGLDisplay display = eglGetDisplay(
            static_cast<EGLNativeDisplayType>(EGL_DEFAULT_DISPLAY));
        gl_detail::egl_require(display != EGL_NO_DISPLAY, "eglGetDisplay");
        gl_detail::egl_require(eglInitialize(display, nullptr, nullptr) == EGL_TRUE,
                               "eglInitialize");
        display_ = display;  // a partir daqui destroy() sabe como limpar

        EGLConfig config = nullptr;
        int version = client_version;
        if (version >= 3) {
            EGLint es3_configs[] = {
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                EGL_DEPTH_SIZE, 24, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                EGL_NONE,
            };
            if (eglChooseConfig(display_, es3_configs, &config, 1, &num_configs) !=
                    EGL_TRUE ||
                num_configs < 1) {
                config = nullptr;
                version = 2;  // fallback
                gl_detail::egl_log_error("eglChooseConfig(ES3)");
            }
        }
        if (!config &&
            (eglChooseConfig(display_, attribs, &config, 1, &num_configs) != EGL_TRUE ||
             num_configs < 1)) {
            gl_detail::egl_log_error("eglChooseConfig");
            destroy();
            throw GLError("eglChooseConfig falhou");
        }
        config_ = config;

        const EGLint context_attribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE,
        };
        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attribs);
        if (context_ == EGL_NO_CONTEXT) {
            gl_detail::egl_log_error("eglCreateContext");
            destroy();
            throw GLError("eglCreateContext falhou");
        }
        version_ = version;

        window_ = window;
        surface_ = eglCreateWindowSurface(display_, config_, window, nullptr);
        if (surface_ == EGL_NO_SURFACE) {
            gl_detail::egl_log_error("eglCreateWindowSurface");
            destroy();
            throw GLError("eglCreateWindowSurface falhou");
        }
        if (!make_current()) {
            destroy();
            throw GLError("eglMakeCurrent falhou");
        }
        ++gl_detail::context_generation_ref();
        context_lost_ = false;
        needs_asset_reload_ = false;
        if (recovering) {
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                                "contexto EGL reinicializado apos perda");
        }
    }

    /// Retorna false (e registra o erro) se não foi possível tornar o contexto
    /// atual. Não lança: é chamado também durante a destruição. Um
    /// EGL_CONTEXT_LOST detectado aqui marca a perda e invalida os nomes GL.
    bool make_current() const {
        if (context_lost_) return false;
        if (display_ == EGL_NO_DISPLAY || context_ == EGL_NO_CONTEXT ||
            surface_ == EGL_NO_SURFACE) {
            return false;
        }
        const EGLBoolean ok = eglMakeCurrent(display_, surface_, surface_, context_);
        if (ok != EGL_TRUE && gl_detail::egl_log_error("eglMakeCurrent")) {
            mark_context_lost("eglMakeCurrent");
        }
        return ok == EGL_TRUE;
    }

    void recreate_surface(ANativeWindow* window) {
        if (context_lost_ || display_ == EGL_NO_DISPLAY || !config_) return;
        if (surface_ != EGL_NO_SURFACE) {
            if (eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               EGL_NO_CONTEXT) != EGL_TRUE &&
                gl_detail::egl_log_error("eglMakeCurrent(unbind)")) {
                mark_context_lost("eglMakeCurrent(unbind)");
                return;
            }
            if (eglDestroySurface(display_, surface_) != EGL_TRUE &&
                gl_detail::egl_log_error("eglDestroySurface")) {
                mark_context_lost("eglDestroySurface");
            }
            surface_ = EGL_NO_SURFACE;
        }
        window_ = window;
        if (window_) {
            surface_ = eglCreateWindowSurface(display_, config_, window_, nullptr);
            if (surface_ == EGL_NO_SURFACE) {
                if (gl_detail::egl_log_error("eglCreateWindowSurface")) {
                    mark_context_lost("eglCreateWindowSurface");
                }
            }
        }
        make_current();
    }

    void set_swap_interval(int interval) const {
        if (!context_lost_ && display_ != EGL_NO_DISPLAY &&
            eglSwapInterval(display_, interval) != EGL_TRUE &&
            gl_detail::egl_log_error("eglSwapInterval")) {
            mark_context_lost("eglSwapInterval");
        }
    }

    /// eglSwapBuffers também é um ponto onde o driver reporta
    /// EGL_CONTEXT_LOST; falhas por surface inválida não marcam perda.
    bool swap() const {
        if (context_lost_ || display_ == EGL_NO_DISPLAY ||
            surface_ == EGL_NO_SURFACE) {
            return false;
        }
        const EGLBoolean ok = eglSwapBuffers(display_, surface_);
        if (ok != EGL_TRUE && gl_detail::egl_log_error("eglSwapBuffers")) {
            mark_context_lost("eglSwapBuffers");
        }
        return ok == EGL_TRUE;
    }

    void clear_current() const {
        if (context_lost_ || display_ == EGL_NO_DISPLAY) return;
        if (eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                           EGL_NO_CONTEXT) != EGL_TRUE &&
            gl_detail::egl_log_error("eglMakeCurrent(clear)")) {
            mark_context_lost("eglMakeCurrent(clear)");
        }
    }

    void destroy() {
        if (context_lost_) {
            discard_lost_context();
            return;
        }
        if (display_ == EGL_NO_DISPLAY) {
            surface_ = EGL_NO_SURFACE;
            context_ = EGL_NO_CONTEXT;
            config_ = nullptr;
            window_ = nullptr;
            version_ = 2;
            return;
        }
        // Nunca destrói um contexto que ainda está atual.
        clear_current();
        if (context_lost_) {
            // clear_current() detectou a perda: não usa mais os handles antigos.
            discard_lost_context();
            return;
        }
        if (surface_ != EGL_NO_SURFACE) {
            if (eglDestroySurface(display_, surface_) != EGL_TRUE) {
                gl_detail::egl_log_error("eglDestroySurface");
            }
            surface_ = EGL_NO_SURFACE;
        }
        if (context_ != EGL_NO_CONTEXT) {
            if (eglDestroyContext(display_, context_) != EGL_TRUE) {
                gl_detail::egl_log_error("eglDestroyContext");
            }
            context_ = EGL_NO_CONTEXT;
        }
        if (eglTerminate(display_) != EGL_TRUE) {
            gl_detail::egl_log_error("eglTerminate");
        }
        display_ = EGL_NO_DISPLAY;
        config_ = nullptr;
        window_ = nullptr;
        version_ = 2;
        ++gl_detail::context_generation_ref();
    }

    /// Estado de perda. Equivale a VulkanScene::device_lost().
    bool context_lost() const { return context_lost_; }
    bool device_lost() const { return context_lost_; }
    /// True até uma reinicialização bem-sucedida via init()/recreate().
    bool needs_asset_reload() const { return needs_asset_reload_; }

    int version() const { return version_; }
    int requested_version() const { return requested_version_; }
    EGLDisplay display() const { return display_; }
    EGLContext context() const { return context_; }
    ANativeWindow* window() const { return window_; }

private:
    /// Marca a perda sem destruir nada: nomes GL/EGL antigos ficam inválidos,
    /// mas não são passados para glDelete*/eglDestroy* num contexto morto.
    void mark_context_lost(const char* operation) const {
        if (context_lost_) return;
        __android_log_print(ANDROID_LOG_ERROR, "nativeapp-gl",
                            "%s: EGL_CONTEXT_LOST; recursos GL invalidos, "
                            "chame init()/recreate() e recarregue assets", operation);
        context_lost_ = true;
        needs_asset_reload_ = true;
        gl_detail::bump_context_generation();
    }

    /// Descarta apenas o estado hospedeiro. Termina o display (seguro mesmo
    /// após perda), mas não chama eglDestroySurface/eglDestroyContext sobre
    /// handles que o driver já invalidou.
    void discard_lost_context() {
        if (display_ != EGL_NO_DISPLAY) {
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                                "descartando handles EGL de contexto perdido "
                                "(sem eglDestroySurface/eglDestroyContext)");
            if (eglTerminate(display_) != EGL_TRUE) {
                gl_detail::egl_log_error("eglTerminate(lost)");
            }
        }
        display_ = EGL_NO_DISPLAY;
        surface_ = EGL_NO_SURFACE;
        context_ = EGL_NO_CONTEXT;
        config_ = nullptr;
        window_ = nullptr;
        version_ = 2;
        requested_version_ = 2;
        context_lost_ = false;
        needs_asset_reload_ = false;
    }

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    ANativeWindow* window_ = nullptr;
    int version_ = 2;
    int requested_version_ = 2;
    mutable bool context_lost_ = false;
    mutable bool needs_asset_reload_ = false;
};

// ============================================================================
// Shader
// ============================================================================

class Shader {
public:
    Shader() = default;
    ~Shader() { destroy(); }
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    /// Compila a partir de código-fonte (GLSL).
    void compile(const std::string& vertex_source, const std::string& fragment_source) {
        destroy();  // recompilar não deve vazar o program anterior
        const GLuint vs = compile_stage(GL_VERTEX_SHADER, vertex_source.c_str());
        GLuint fs = 0;
        try {
            fs = compile_stage(GL_FRAGMENT_SHADER, fragment_source.c_str());
        } catch (...) {
            glDeleteShader(vs);
            throw;
        }
        link(vs, fs);
    }

    /// Carrega de assets/ (caminhos relativos ao diretório assets).
    void load(const std::string& vertex_path, const std::string& fragment_path) {
        auto& assets = Assets::instance(nullptr);
        if (!assets.manager()) {
            throw GLError("AAssetManager indisponível");
        }
        compile(assets.read_text(vertex_path), assets.read_text(fragment_path));
    }

    void use() const {
        if (valid()) glUseProgram(program_);
    }

    void set_uniform(const std::string& name, float value) {
        glUniform1f(loc(name), value);
    }
    void set_uniform(const std::string& name, int value) {
        glUniform1i(loc(name), value);
    }
    void set_uniform(const std::string& name, float x, float y) {
        glUniform2f(loc(name), x, y);
    }
    void set_uniform(const std::string& name, float x, float y, float z) {
        glUniform3f(loc(name), x, y, z);
    }
    void set_uniform(const std::string& name, float x, float y, float z, float w) {
        glUniform4f(loc(name), x, y, z, w);
    }
    void set_uniform_mat4(const std::string& name, const float* matrix) {
        if (!matrix) {
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                                "set_uniform_mat4(%s): matriz nula", name.c_str());
            return;
        }
        glUniformMatrix4fv(loc(name), 1, GL_FALSE, matrix);
    }

    GLint attrib(const std::string& name) {
        if (!valid()) return -1;
        return glGetAttribLocation(program_, name.c_str());
    }
    GLuint program() const { return valid() ? program_ : 0; }

    /// Objeto utilizável apenas na geração de contexto que o criou.
    bool valid() const {
        return program_ != 0 && generation_ == gl_detail::context_generation();
    }

    /// Descarta o nome sem glDeleteProgram (contexto perdido). Um posterior
    /// compile() reconstrói o programa automaticamente.
    void release_without_gl() {
        program_ = 0;
        locations_.clear();
        generation_ = gl_detail::context_generation();
    }

    void destroy() {
        const bool same_generation = generation_ == gl_detail::context_generation();
        if (program_ && same_generation) glDeleteProgram(program_);
        program_ = 0;
        locations_.clear();
        generation_ = gl_detail::context_generation();
    }

private:
    GLint loc(const std::string& name) {
        if (!valid()) {
            locations_.clear();
            return -1;  // glUniform* com -1 é no-op
        }
        auto it = locations_.find(name);
        if (it != locations_.end()) return it->second;
        const GLint location = glGetUniformLocation(program_, name.c_str());
        locations_[name] = location;
        return location;
    }

    static GLuint compile_stage(GLenum type, const char* source) {
        const GLuint shader = glCreateShader(type);
        if (!shader) throw GLError("glCreateShader falhou");
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            char log[2048] = {};
            glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
            glDeleteShader(shader);
            throw GLError(std::string("Shader falhou: ") + log);
        }
        return shader;
    }

    void link(GLuint vs, GLuint fs) {
        const GLuint program = glCreateProgram();
        if (!program) {
            glDeleteShader(vs);
            glDeleteShader(fs);
            throw GLError("glCreateProgram falhou");
        }
        try {
            glAttachShader(program, vs);
            glAttachShader(program, fs);
            // Fixa as locations para casar com Mesh::draw() (0=pos,1=normal,
            // 2=uv,3=color). a_uv no location 2 é parte do contrato do texto.
            glBindAttribLocation(program, 0, "a_position");
            glBindAttribLocation(program, 1, "a_normal");
            glBindAttribLocation(program, 2, "a_uv");
            glBindAttribLocation(program, 3, "a_color");
            glLinkProgram(program);
            GLint linked = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (!linked) {
                char log[2048] = {};
                glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                throw GLError(std::string("Link falhou: ") + log);
            }
        } catch (...) {
            glDeleteProgram(program);
            glDeleteShader(vs);
            glDeleteShader(fs);
            throw;
        }
        glDeleteShader(vs);
        glDeleteShader(fs);
        program_ = program;
        locations_.clear();
        generation_ = gl_detail::context_generation();
    }

    GLuint program_ = 0;
    std::unordered_map<std::string, GLint> locations_;
    uint64_t generation_ = gl_detail::context_generation();
};

// ============================================================================
// Mesh
// ============================================================================

class Mesh {
public:
    Mesh() = default;
    ~Mesh() { destroy(); }
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    void upload(const MeshData& data) {
        refresh_generation();  // contexto novo: nomes antigos são descartados
        if (!vbo_) glGenBuffers(1, &vbo_);
        if (!vbo_) { gl_check("mesh_gen_buffer"); return; }

        // Índice fora do intervalo faria o GPU ler memória além do VBO.
        bool indices_valid = true;
        for (uint16_t index : data.indices) {
            if (index >= data.vertices.size()) { indices_valid = false; break; }
        }
        if (!indices_valid) {
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                                "Mesh com índice >= nº de vértices; usando drawArrays");
        }
        if (!ibo_ && indices_valid && !data.indices.empty()) glGenBuffers(1, &ibo_);

        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(data.vertices.size() * sizeof(Vertex)),
                     data.vertices.data(), GL_STATIC_DRAW);

        vertex_count_ = static_cast<GLsizei>(data.vertices.size());
        index_count_ = indices_valid ? static_cast<GLsizei>(data.indices.size()) : 0;
        if (ibo_ && index_count_ > 0) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(data.indices.size() * sizeof(uint16_t)),
                         data.indices.data(), GL_STATIC_DRAW);
        }
        topology_ = data.topology;
    }

    void draw() const {
        if (!valid() || !vbo_) return;
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        const GLsizei stride = static_cast<GLsizei>(sizeof(Vertex));
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(Vertex, x)));
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(Vertex, nx)));
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(Vertex, u)));
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(Vertex, r)));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glEnableVertexAttribArray(3);

        const GLenum mode = gl_mode();
        if (ibo_ && index_count_ > 0) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
            glDrawElements(mode, index_count_, GL_UNSIGNED_SHORT, nullptr);
        } else {
            glDrawArrays(mode, 0, vertex_count_);
        }
        gl_check("mesh_draw");
    }

    void destroy() {
        const bool same_generation = generation_ == gl_detail::context_generation();
        if (same_generation) {
            if (vbo_) glDeleteBuffers(1, &vbo_);
            if (ibo_) glDeleteBuffers(1, &ibo_);
        }
        vbo_ = 0;
        ibo_ = 0;
        generation_ = gl_detail::context_generation();
    }

    /// Descarta VBO/IBO sem chamar glDeleteBuffers (contexto perdido).
    /// vertex_count_/topology_ permanecem para um novo upload() explícito.
    void release_without_gl() {
        vbo_ = 0;
        ibo_ = 0;
        index_count_ = 0;
        generation_ = gl_detail::context_generation();
    }

    bool valid() const {
        return generation_ == gl_detail::context_generation();
    }

    GLsizei vertex_count() const { return vertex_count_; }
    void set_vertex_count(GLsizei count) { vertex_count_ = count; }

private:
    // GLES3Scene configura os atributos de instância diretamente nos buffers
    // desta Mesh (um draw call); sem isso seria preciso duplicar o upload.
    friend class GLES3Scene;

    void refresh_generation() {
        if (generation_ == gl_detail::context_generation()) return;
        vbo_ = 0;
        ibo_ = 0;
        index_count_ = 0;
        generation_ = gl_detail::context_generation();
    }

    GLenum gl_mode() const {
        switch (topology_) {
            case Topology::TriangleStrip: return GL_TRIANGLE_STRIP;
            case Topology::TriangleFan: return GL_TRIANGLE_FAN;
            case Topology::Lines: return GL_LINES;
            case Topology::LineStrip: return GL_LINE_STRIP;
            case Topology::Points: return GL_POINTS;
            default: return GL_TRIANGLES;
        }
    }

    GLuint vbo_ = 0;
    GLuint ibo_ = 0;
    GLsizei index_count_ = 0;
    GLsizei vertex_count_ = 0;
    Topology topology_ = Topology::Triangles;
    uint64_t generation_ = gl_detail::context_generation();
};

// ============================================================================
// Texture
// ============================================================================

class Texture {
public:
    Texture() = default;
    ~Texture() { destroy(); }
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    /// Carrega PNG/JPEG/WebP/SVG de assets/ (formato detectado por magic bytes).
    bool load(const std::string& path) { return load_with_size(path, 0, 0); }

    /// Alias explícito: útil para documentar intenção em código de usuário.
    bool load_webp_asset(const std::string& path) { return load(path); }

    /// Rasteriza SVG para width x height (0,0 = tamanho natural do documento).
    bool load_svg_asset(const std::string& path, int width = 0, int height = 0) {
        return load_with_size(path, width, height);
    }

    /// Sobe RGBA já decodificado (uint8_t, 4 bytes por pixel).
    bool load_rgba(const uint8_t* rgba, int width, int height) {
        if (!rgba || width <= 0 || height <= 0) return false;
        refresh_generation();
        return upload_rgba(rgba, width, height);
    }

    void bind(unsigned int unit = 0) const {
        if (!texture_ || generation_ != gl_detail::context_generation()) return;
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, texture_);
    }

    /// Nome utilizável apenas na geração que criou a textura.
    bool valid() const {
        return texture_ != 0 && generation_ == gl_detail::context_generation();
    }

    /// Descarta o nome sem glDeleteTextures (contexto perdido).
    void release_without_gl() {
        texture_ = 0;
        width_ = height_ = 0;
        generation_ = gl_detail::context_generation();
    }

    void destroy() {
        if (valid()) glDeleteTextures(1, &texture_);
        texture_ = 0;
        width_ = height_ = 0;
        generation_ = gl_detail::context_generation();
    }

    int width() const { return width_; }
    int height() const { return height_; }
    GLuint id() const { return valid() ? texture_ : 0; }

private:
    void refresh_generation() {
        const uint64_t generation = gl_detail::context_generation();
        if (generation_ == generation) return;
        // Contexto novo: o nome antigo pode pertencer a outro objeto.
        texture_ = 0;
        width_ = height_ = 0;
        generation_ = generation;
    }

    bool upload_rgba(const uint8_t* rgba, int width, int height) {
        // Só destrói a textura antiga depois de ter pixels válidos em mãos e
        // se ela pertencer a esta geração de contexto.
        if (texture_ && generation_ == gl_detail::context_generation()) {
            glDeleteTextures(1, &texture_);
        }
        if (texture_) {
            texture_ = 0;
            width_ = height_ = 0;
        }
        glGenTextures(1, &texture_);
        if (!texture_) return false;
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        width_ = width;
        height_ = height;
        generation_ = gl_detail::context_generation();
        gl_check("texture_load");
        return true;
    }

    bool load_with_size(const std::string& path, int svg_w, int svg_h) {
        refresh_generation();
        DecodedImage image;
        if (!decode_asset_image(path, image, svg_w, svg_h)) return false;
        if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) return false;
        return upload_rgba(image.rgba.data(), image.width, image.height);
    }

    GLuint texture_ = 0;
    int width_ = 0;
    int height_ = 0;
    uint64_t generation_ = gl_detail::context_generation();
};

// ============================================================================
// GLES2Scene — implementa Scene
// ============================================================================

class GLES2Scene : public Scene {
public:
    GLES2Scene() = default;
    ~GLES2Scene() override { destroy(); }
    GLES2Scene(const GLES2Scene&) = delete;
    GLES2Scene& operator=(const GLES2Scene&) = delete;

    virtual void init(ANativeWindow* window) { init(window, 2); }

    /// true depois de EGL_CONTEXT_LOST; a cena para de desenhar até
    /// init()/recreate(). Equivale a VulkanScene::device_lost().
    bool context_lost() const { return egl_.context_lost(); }
    bool device_lost() const { return context_lost(); }
    /// true quando fontes/texturas precisam ser recarregadas pelo app.
    bool needs_asset_reload() const { return egl_.needs_asset_reload(); }

    /// Recuperação explícita. Sem argumentos reusa a janela e a versão
    /// solicitadas no init() anterior. Reconstrói shader/mesh/fontes internas
    /// e recarrega as texturas do cache por caminho; shaders/malhas/fontes
    /// externas devem ser reenviadas pelo app (needs_asset_reload()).
    void recreate(ANativeWindow* window = nullptr, int client_version = -1) {
        ANativeWindow* target = window ? window : window_;
        const int version = client_version >= 0 ? client_version : requested_client_version_;
        if (initialized_ && !egl_.context_lost()) destroy();
        else handle_context_loss();
        init(target, version);
    }

protected:
    /// Acesso para subclasses (usado pelo shader de instancing da GLES3Scene).
    const Mat4& projection() const { return projection_; }

    /// Resolve material.texture no cache. Retorna nullptr em caso de falha —
    /// nunca devolve um iterator end() desreferenciado.
    Texture* cached_texture(const std::string& path) {
        if (path.empty()) return nullptr;
        auto it = textures_.find(path);
        if (it != textures_.end()) return it->second.get();
        auto texture = std::make_unique<Texture>();
        if (!texture->load(path)) {
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                                "Textura nao carregada: %s", path.c_str());
            return nullptr;
        }
        it = textures_.emplace(path, std::move(texture)).first;
        return it->second.get();
    }

    /// Chamado quando EGL_CONTEXT_LOST (ou uma geração de contexto divergente)
    /// é observado. Não chama glDelete* sobre nomes do contexto morto.
    /// GLES3Scene estende este hook para seus VAO/VBO/shader de instancing.
    virtual void invalidate_resources_after_loss() {
        shader_.release_without_gl();
        mesh_.release_without_gl();
        temporary_mesh_.release_without_gl();
        for (auto& entry : textures_) entry.second->release_without_gl();
        text_shader_.release_without_gl();
        font_texture_ = 0;
        text_vbo_ = 0;
        // font_ (FontAtlas) é dado CPU-side: preservado para reupload em init().
    }

    void handle_context_loss() {
        const uint64_t generation = gl_detail::context_generation();
        if (!egl_.context_lost() && scene_generation_ == generation) return;
        if (!loss_handled_) {
            __android_log_print(ANDROID_LOG_ERROR, "nativeapp-gl",
                                "contexto GLES perdido ou substituido: cena invalidada; "
                                "chame init()/recreate() e recarregue assets");
            loss_handled_ = true;
        }
        invalidate_resources_after_loss();
        scene_generation_ = generation;
        initialized_ = false;
    }

    /// Ponto único de entrada GL da cena: detecta perda antes e depois de
    /// eglMakeCurrent, para que nenhum draw use um nome de contexto morto.
    bool ensure_current() {
        if (egl_.context_lost() || scene_generation_ != gl_detail::context_generation()) {
            handle_context_loss();
            return false;
        }
        if (!egl_.make_current()) {
            if (egl_.context_lost()) handle_context_loss();
            return false;
        }
        if (egl_.context_lost()) {
            handle_context_loss();
            return false;
        }
        return true;
    }

    void init(ANativeWindow* window, int client_version) {
        window_ = window;
        requested_client_version_ = client_version;
        const bool recovering = egl_.context_lost() || loss_handled_;
        if (recovering) handle_context_loss();
        if (initialized_) return;

        egl_.init(window, client_version);
        scene_generation_ = gl_detail::context_generation();
        try {
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glClearColor(clear_[0], clear_[1], clear_[2], clear_[3]);

            shader_.compile(kVertexShader, kFragmentShader);
            mesh_.upload(default_mesh());
            const int w = window ? ANativeWindow_getWidth(window) : 0;
            const int h = window ? ANativeWindow_getHeight(window) : 0;
            if (w > 0 && h > 0) resize(w, h);
        } catch (...) {
            // init() pode ser chamado de novo após uma falha (ex.: shader):
            // não deixa EGL nem objetos GL parcialmente criados vazando.
            shader_.destroy();
            mesh_.destroy();
            egl_.destroy();
            scene_generation_ = gl_detail::context_generation();
            loss_handled_ = false;
            initialized_ = false;
            throw;
        }
        if (recovering) {
            reload_cached_textures();
            if (font_) restore_font_atlas();
            if (width_ > 0 && height_ > 0) resize(width_, height_);
        }
        loss_handled_ = false;
        initialized_ = true;
        if (recovering) {
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                                "contexto GLES reinicializado; recursos internos restaurados");
        }
    }

public:
    void set_shader(Shader* shader) { shader_override_ = shader; }
    void set_mesh(Mesh* mesh) { mesh_override_ = mesh; }

    void resize(int width, int height) override {
        width_ = width;
        height_ = height;
        // Janela em destruição pode reportar dimensões <= 0; glViewport com
        // valores negativos geraria GL_INVALID_VALUE e estragaria a projeção.
        // Com contexto perdido guardamos as dimensões para restore no init().
        if (width <= 0 || height <= 0 || !ensure_current()) return;
        glViewport(0, 0, width_, height_);
        projection_ = mat4_ortho2d(static_cast<float>(width_), static_cast<float>(height_));
    }

    void set_clear_color(float r, float g, float b, float a = 1.0f) override {
        clear_ = {r, g, b, a};
    }

    /// Compatibilidade: define matriz model com translação 2D.
    void set_offset(float x, float y) {
        model_ = mat4_translate(x, y, 0.0f);
    }

    void set_projection(const Mat4& projection) { projection_ = projection; }
    void set_swap_interval(int interval) { egl_.set_swap_interval(interval); }

    void draw(const MeshData& data, const Material& material,
              const float* model = nullptr) override {
        draw_with_texture(data, material, model, nullptr);
    }

    /// Desenha com uma Texture externa (ex.: frame de WebP animado).
    void draw_with_texture(const MeshData& data, const Material& material,
                           const float* model, const Texture* texture_override) {
        if (!initialized_ || !ensure_current()) return;
        temporary_mesh_.upload(data);
        draw_mesh(temporary_mesh_, material, model, texture_override);
    }

    void draw_mesh(const Mesh& mesh, const Material& material,
                   const float* model = nullptr,
                   const Texture* texture_override = nullptr) {
        if (!initialized_ || !ensure_current()) return;
        Shader& shader = shader_override_ ? *shader_override_ : shader_;
        if (!shader.program()) return;  // shader de outra geração de contexto
        shader.use();
        shader.set_uniform_mat4("u_projection", projection_.data());
        const Mat4 identity = mat4_identity();
        shader.set_uniform_mat4("u_model", model ? model : identity.data());
        shader.set_uniform("u_color", material.color[0], material.color[1],
                           material.color[2], material.color[3]);
        // cached_texture() devolve nullptr com segurança se o load falhar.
        const Texture* texture =
            texture_override ? texture_override : cached_texture(material.texture);
        shader.set_uniform("u_use_texture", texture ? 1 : 0);
        if (texture) {
            texture->bind(0);
            shader.set_uniform("u_texture", 0);
        }

        if (material.depth_test) glEnable(GL_DEPTH_TEST);
        else glDisable(GL_DEPTH_TEST);
        if (material.blend) glEnable(GL_BLEND);
        else glDisable(GL_BLEND);

        mesh.draw();
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(2);
        glDisableVertexAttribArray(3);
        gl_check("draw_mesh");
    }

    void begin_frame() override {
        if (!initialized_ || !ensure_current()) return;
        // Restaura defaults para que um material anterior não vaze estado.
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glActiveTexture(GL_TEXTURE0);
        glClearColor(clear_[0], clear_[1], clear_[2], clear_[3]);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    void present() override {
        if (!initialized_ || !ensure_current()) return;
        if (!egl_.swap()) {
            if (egl_.context_lost()) handle_context_loss();
            else __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl", "eglSwapBuffers falhou");
        }
    }

    void render() {
        // Compatibilidade: clear + desenha a mesh padrão em NDC + present.
        // A mesh legada (default_mesh) usa -0.5..0.5; a projection ortho de
        // resize() não se aplica a ela.
        const Mat4 saved = projection_;
        projection_ = mat4_identity();
        begin_frame();
        Material material;
        draw_mesh(mesh_override_ ? *mesh_override_ : mesh_, material, model_.data());
        projection_ = saved;
        present();
    }

    /// Carrega uma fonte TTF/OTF de assets/ e sobe o atlas para a GPU.
    bool load_font(const std::string& path, int pixel_size = 24) {
        if (!initialized_ || !ensure_current()) return false;
        FontAtlas atlas;
        if (!atlas.load(path, pixel_size)) return false;
        if (!atlas.valid() || atlas.atlas_size() <= 0 || !atlas.atlas()) return false;
        font_ = std::make_unique<FontAtlas>(std::move(atlas));
        return restore_font_atlas();
    }

    /// Desenha texto usando a fonte carregada (coordenadas de tela).
    void draw_text(const std::string& text, float x, float y, float scale = 1.0f,
                   float r = 1.0f, float g = 1.0f, float b = 1.0f, float a = 1.0f) {
        if (!font_ || !font_texture_ || !ensure_current()) return;
        if (!text_shader_.program()) return;
        text_shader_.use();
        glDisable(GL_DEPTH_TEST);  // texto é 2D sobreposto
        const Mat4 identity = mat4_identity();
        text_shader_.set_uniform_mat4("u_projection", projection_.data());
        text_shader_.set_uniform_mat4("u_model", identity.data());
        text_shader_.set_uniform("u_color", r, g, b, a);
        text_shader_.set_uniform("u_texture", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, font_texture_);

        struct GlyphVert { float x, y, u, v; };
        std::vector<GlyphVert> verts;
        verts.reserve(text.size() * 6);
        float cx = x, cy = y;
        for (char c : text) {
            const GlyphQuad q = font_->quad(c, cx, cy, scale);
            if (q.x1 <= q.x0) continue;
            // Dois triângulos por glyph (UVs do atlas stb = GL, y invertido).
            verts.push_back({q.x0, q.y0, q.u0, q.v0});
            verts.push_back({q.x1, q.y0, q.u1, q.v0});
            verts.push_back({q.x1, q.y1, q.u1, q.v1});
            verts.push_back({q.x0, q.y0, q.u0, q.v0});
            verts.push_back({q.x1, q.y1, q.u1, q.v1});
            verts.push_back({q.x0, q.y1, q.u0, q.v1});
        }
        if (verts.empty()) return;

        // Um único VBO persistente para todo o texto evita glGenBuffers/
        // glDeleteBuffers a cada linha desenhada (media-demo desenha 4).
        if (!text_vbo_) glGenBuffers(1, &text_vbo_);
        if (!text_vbo_) return;
        glBindBuffer(GL_ARRAY_BUFFER, text_vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(verts.size() * sizeof(GlyphVert)),
                     verts.data(), GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVert),
                              reinterpret_cast<void*>(offsetof(GlyphVert, x)));
        // Shader::link() fixa a_uv no location 2 (a_normal é 1).
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVert),
                              reinterpret_cast<void*>(offsetof(GlyphVert, u)));
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size()));
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(2);
        gl_check("draw_text");
    }

    bool has_font() const { return font_ != nullptr; }

    virtual void destroy() {
        if (egl_.context_lost() || scene_generation_ != gl_detail::context_generation()) {
            // Perda detectada: descarta nomes sem glDelete* e desfaz EGL sem
            // eglDestroySurface/eglDestroyContext sobre handles inválidos.
            handle_context_loss();
            egl_.destroy();
            font_.reset();
            textures_.clear();
            loss_handled_ = false;
            scene_generation_ = gl_detail::context_generation();
            initialized_ = false;
            return;
        }
        if (egl_.display() == EGL_NO_DISPLAY || !initialized_) {
            // init() nunca chegou a criar contexto (ou já foi destruído):
            // apenas zera o estado lógico, sem chamar GL sem contexto atual.
            font_.reset();
            font_texture_ = 0;
            text_vbo_ = 0;
            textures_.clear();
            initialized_ = false;
            return;
        }
        if (!ensure_current()) {
            // Falha de current sem perda explícita: não arrisca glDelete*.
            font_.reset();
            font_texture_ = 0;
            text_vbo_ = 0;
            textures_.clear();
            initialized_ = false;
            egl_.destroy();
            loss_handled_ = false;
            scene_generation_ = gl_detail::context_generation();
            return;
        }
        if (font_texture_) { glDeleteTextures(1, &font_texture_); font_texture_ = 0; }
        if (text_vbo_) { glDeleteBuffers(1, &text_vbo_); text_vbo_ = 0; }
        font_.reset();
        text_shader_.destroy();
        textures_.clear();
        temporary_mesh_.destroy();
        mesh_.destroy();
        shader_.destroy();
        egl_.destroy();
        initialized_ = false;
        loss_handled_ = false;
        scene_generation_ = gl_detail::context_generation();
    }

    GLContext& egl() { return egl_; }
    const GLContext& egl() const { return egl_; }

private:
    static MeshData default_mesh() { return MeshData::triangle(); }

    /// Reenvia o atlas CPU-side (FontAtlas) e recria o shader de texto depois
    /// de uma perda. text_vbo_ é recriado preguiçosamente por draw_text().
    bool restore_font_atlas() {
        if (!font_ || !font_->valid() || font_->atlas_size() <= 0 || !font_->atlas()) {
            return false;
        }
        try {
            if (!text_shader_.program()) {
                text_shader_.compile(kTextVertexShader, kTextFragmentShader);
            }
        } catch (const GLError&) {
            return false;
        }
        if (!text_shader_.program()) return false;
        if (!font_texture_) glGenTextures(1, &font_texture_);
        if (!font_texture_) return false;
        glBindTexture(GL_TEXTURE_2D, font_texture_);
        // Expande o atlas de 1 canal para RGBA: GL_ALPHA é problemático em
        // alguns drivers GLES3 e o RGBA é universal.
        const std::size_t side = static_cast<std::size_t>(font_->atlas_size());
        const std::size_t n = side * side;
        if (n > std::numeric_limits<std::size_t>::max() / 4) return false;
        std::vector<uint8_t> rgba(n * 4, 255);
        const uint8_t* pixels = font_->atlas();
        for (std::size_t i = 0; i < n; ++i) rgba[i * 4 + 3] = pixels[i];
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, font_->atlas_size(), font_->atlas_size(),
                     0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl_check("font_atlas");
        text_vbo_ = 0;
        return true;
    }

    /// Texturas da cena são identificadas por caminho; o reload usa o mesmo
    /// caminho do cache, com a geração nova de contexto.
    void reload_cached_textures() {
        for (auto& entry : textures_) {
            if (entry.first.empty() || !entry.second) continue;
            if (!entry.second->load(entry.first)) {
                __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                                    "Textura nao recarregada: %s", entry.first.c_str());
            }
        }
    }

    static constexpr const char* kVertexShader = R"(
        attribute vec3 a_position;
        attribute vec3 a_normal;
        attribute vec2 a_uv;
        attribute vec4 a_color;
        uniform mat4 u_projection;
        uniform mat4 u_model;
        varying vec2 v_uv;
        varying vec4 v_color;
        void main() {
            v_uv = a_uv;
            v_color = a_color;
            gl_Position = u_projection * u_model * vec4(a_position, 1.0);
        }
    )";

    static constexpr const char* kFragmentShader = R"(
        precision mediump float;
        varying vec2 v_uv;
        varying vec4 v_color;
        uniform vec4 u_color;
        uniform sampler2D u_texture;
        uniform int u_use_texture;
        void main() {
            vec4 base = u_use_texture == 1 ? texture2D(u_texture, v_uv) : vec4(1.0);
            gl_FragColor = base * v_color * u_color;
        }
    )";

    static constexpr const char* kTextVertexShader = R"(
        attribute vec2 a_position;
        attribute vec2 a_uv;
        uniform mat4 u_projection;
        uniform mat4 u_model;
        varying vec2 v_uv;
        void main() {
            v_uv = a_uv;
            gl_Position = u_projection * u_model * vec4(a_position, 0.0, 1.0);
        }
    )";

    static constexpr const char* kTextFragmentShader = R"(
        precision mediump float;
        varying vec2 v_uv;
        uniform vec4 u_color;
        uniform sampler2D u_texture;
        void main() {
            float alpha = texture2D(u_texture, v_uv).a;
            gl_FragColor = vec4(u_color.rgb, u_color.a * alpha);
        }
    )";

    GLContext egl_;
    Shader shader_;
    Shader* shader_override_ = nullptr;
    Mesh mesh_;
    Mesh* mesh_override_ = nullptr;
    Mesh temporary_mesh_;
    std::unordered_map<std::string, std::unique_ptr<Texture>> textures_;
    Shader text_shader_;
    std::unique_ptr<FontAtlas> font_;
    GLuint font_texture_ = 0;
    GLuint text_vbo_ = 0;
    Mat4 projection_ = mat4_identity();
    Mat4 model_ = mat4_identity();
    std::array<float, 4> clear_{0.08f, 0.08f, 0.12f, 1.0f};
    int width_ = 0;
    int height_ = 0;
    ANativeWindow* window_ = nullptr;
    int requested_client_version_ = 2;
    uint64_t scene_generation_ = gl_detail::context_generation();
    bool initialized_ = false;
    bool loss_handled_ = false;
};

// ============================================================================
// GLES3Scene — VAO + instancing real (glDraw*Instanced)
// ============================================================================

class GLES3Scene : public GLES2Scene {
public:
    /// Transformação de uma instância: matriz model (column-major) e cor RGBA.
    /// A cor multiplica a cor do vértice e a do Material, permitindo distinguir
    /// cada cópia sem uniforms por desenho.
    struct Instance {
        Mat4 model = mat4_identity();
        std::array<float, 4> color{{1.0f, 1.0f, 1.0f, 1.0f}};
    };
    // Layout rigoroso: 4 colunas da Mat4 (16 floats) + RGBA = 80 bytes.
    static_assert(sizeof(Instance) == 80, "Instance layout must be 80 bytes");

    using GLES2Scene::init;  // expõe a sobrecarga init(window, version)

    void init(ANativeWindow* window) override {
        GLES2Scene::init(window, 3);  // tenta GLES3; GLContext faz fallback para 2
        if (egl().version() < 3) {
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                                "GLES3 solicitado mas contexto GLES%d em uso",
                                egl().version());
        }
    }

    ~GLES3Scene() override { destroy(); }

    /// Libera os objetos próprios do instancing antes da base. `destroy()` é
    /// virtual para que chamadas através de Scene* também limpem este estado.
    void destroy() override {
        if (egl().context_lost() || instance_generation_ != gl_detail::context_generation()) {
            invalidate_resources_after_loss();  // sem glDelete* em contexto morto
        } else if (egl().display() != EGL_NO_DISPLAY && ensure_current()) {
            if (instance_vbo_) glDeleteBuffers(1, &instance_vbo_);
            if (instance_vao_) glDeleteVertexArrays(1, &instance_vao_);
            instance_vbo_ = 0;
            instance_vao_ = 0;
            instanced_shader_.destroy();
        } else {
            instance_vbo_ = 0;
            instance_vao_ = 0;
        }
        instanced_shader_ready_ = false;
        GLES2Scene::destroy();
        instance_generation_ = gl_detail::context_generation();
    }

    /// Instanciamento real: UM glDrawElementsInstanced/glDrawArraysInstanced
    /// para todas as instâncias, com matriz e cor vindas de VBO com divisor 1.
    /// Retorna true se o caminho GLES3 foi usado; false se houve fallback para
    /// o loop legado (contexto GLES2 ou shader GLES3 indisponível).
    bool draw_instanced(const Mesh& mesh, const Material& material,
                        const std::vector<Instance>& instances) {
        if (instances.empty()) return false;
        if (draw_instanced_impl(mesh, material, instances.data(), instances.size())) {
            return true;
        }
        fallback_loop(mesh, material, instances);
        return false;
    }

    /// Mesma API, mas a geometria é enviada uma única vez para a GPU.
    /// Use a sobrecarga com `Mesh` para geometria estática reutilizável.
    bool draw_instanced(const MeshData& data, const Material& material,
                        const std::vector<Instance>& instances) {
        if (instances.empty()) return false;
        if (egl().display() != EGL_NO_DISPLAY && egl().version() >= 3 &&
            ensure_current()) {
            Mesh mesh;
            mesh.upload(data);
            if (draw_instanced_impl(mesh, material, instances.data(), instances.size())) {
                return true;
            }
        }
        fallback_loop(data, material, instances);
        return false;
    }

    /// Compatibilidade: assinatura original preservada. Converte cada Mat4 em
    /// Instance com cor branca. Não sinaliza fallback (use a sobrecarga nova
    /// com `std::vector<Instance>` se precisar saber se houve instancing real).
    void draw_instanced(const MeshData& data, const Material& material,
                        const std::vector<Mat4>& models) {
        std::vector<Instance> instances;
        instances.reserve(models.size());
        for (const Mat4& model : models) instances.push_back(Instance{model});
        draw_instanced(data, material, instances);
    }

protected:
    /// Estende a invalidação da base com os objetos do instancing.
    void invalidate_resources_after_loss() override {
        instanced_shader_.release_without_gl();
        instance_vbo_ = 0;
        instance_vao_ = 0;
        instanced_shader_ready_ = false;
        instance_generation_ = gl_detail::context_generation();
        GLES2Scene::invalidate_resources_after_loss();
    }

private:
    void refresh_instance_generation() {
        if (instance_generation_ == gl_detail::context_generation()) return;
        instance_vbo_ = 0;
        instance_vao_ = 0;
        instanced_shader_ready_ = false;
        instance_generation_ = gl_detail::context_generation();
    }

    bool ensure_instanced_shader() {
        if (instanced_shader_ready_ && instanced_shader_.program()) return true;
        try {
            instanced_shader_.compile(kInstancedVertexShader,
                                      kInstancedFragmentShader);
        } catch (const GLError& e) {
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                                "shader de instancing indisponivel: %s", e.what());
            return false;
        }
        instanced_shader_ready_ = true;
        return true;
    }

    bool draw_instanced_impl(const Mesh& mesh, const Material& material,
                             const Instance* instances, std::size_t count) {
        if (!instances || count == 0 || !mesh.valid() || !mesh.vbo_) return false;
        if (count > static_cast<std::size_t>(std::numeric_limits<GLsizei>::max())) {
            return false;
        }
        if (egl().display() == EGL_NO_DISPLAY || egl().version() < 3) return false;
        if (!ensure_current()) return false;
        refresh_instance_generation();
        if (!ensure_instanced_shader()) return false;
        if (!instance_vao_) glGenVertexArrays(1, &instance_vao_);
        if (!instance_vbo_) glGenBuffers(1, &instance_vbo_);
        if (!instance_vao_ || !instance_vbo_) {
            gl_check("instancing_alloc");
            return false;
        }

        const GLsizei n = static_cast<GLsizei>(count);
        Shader& shader = instanced_shader_;
        shader.use();
        shader.set_uniform_mat4("u_projection", projection().data());
        shader.set_uniform("u_color", material.color[0], material.color[1],
                           material.color[2], material.color[3]);
        const Texture* texture = cached_texture(material.texture);
        shader.set_uniform("u_use_texture", texture ? 1 : 0);
        if (texture) {
            texture->bind(0);
            shader.set_uniform("u_texture", 0);
        }
        if (material.depth_test) glEnable(GL_DEPTH_TEST);
        else glDisable(GL_DEPTH_TEST);
        if (material.blend) glEnable(GL_BLEND);
        else glDisable(GL_BLEND);

        glBindVertexArray(instance_vao_);

        // Atributos por vértice: divisor 0. A Mesh é amiga desta classe para
        // que seus buffers sejam reaproveitados sem emitir um glDraw normal.
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_);
        const GLsizei stride = static_cast<GLsizei>(sizeof(Vertex));
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(Vertex, x)));
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(Vertex, nx)));
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(Vertex, u)));
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(Vertex, r)));
        for (GLuint i = 0; i < 4; ++i) {
            glEnableVertexAttribArray(i);
            glVertexAttribDivisor(i, 0);
        }

        // Atributos por instância: uma matriz 4x4 ocupa 4 locations (4..7) e a
        // cor o location 8; todos com divisor 1 (um valor por instância).
        glBindBuffer(GL_ARRAY_BUFFER, instance_vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(count * sizeof(Instance)),
                     instances, GL_STREAM_DRAW);
        for (GLuint i = 0; i < 4; ++i) {
            const GLuint location = 4 + i;
            glVertexAttribPointer(location, 4, GL_FLOAT, GL_FALSE,
                                  static_cast<GLsizei>(sizeof(Instance)),
                                  reinterpret_cast<void*>(i * 4 * sizeof(float)));
            glEnableVertexAttribArray(location);
            glVertexAttribDivisor(location, 1);
        }
        glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE,
                              static_cast<GLsizei>(sizeof(Instance)),
                              reinterpret_cast<void*>(offsetof(Instance, color)));
        glEnableVertexAttribArray(8);
        glVertexAttribDivisor(8, 1);

        const GLenum mode = mesh.gl_mode();
        if (mesh.ibo_ && mesh.index_count_ > 0) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo_);
            glDrawElementsInstanced(mode, mesh.index_count_, GL_UNSIGNED_SHORT,
                                    nullptr, n);
        } else {
            glDrawArraysInstanced(mode, 0, mesh.vertex_count_, n);
        }

        // Não deixa divisor/arrays ligados vazarem para o caminho GLES2.
        for (GLuint i = 0; i < 9; ++i) {
            glVertexAttribDivisor(i, 0);
            glDisableVertexAttribArray(i);
        }
        glBindVertexArray(0);
        gl_check("draw_instanced");
        return true;
    }

    static Material tinted(const Material& material,
                           const std::array<float, 4>& color) {
        Material out = material;
        for (int i = 0; i < 4; ++i) out.color[i] = material.color[i] * color[i];
        return out;
    }

    void fallback_loop(const Mesh& mesh, const Material& material,
                       const std::vector<Instance>& instances) {
        __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                            "draw_instanced: GLES%d sem instancing real; fallback com %zu draws",
                            egl().version(), instances.size());
        for (const Instance& instance : instances) {
            draw_mesh(mesh, tinted(material, instance.color), instance.model.data());
        }
    }

    void fallback_loop(const MeshData& data, const Material& material,
                       const std::vector<Instance>& instances) {
        __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                            "draw_instanced: GLES%d sem instancing real; fallback com %zu draws",
                            egl().version(), instances.size());
        for (const Instance& instance : instances) {
            draw(data, tinted(material, instance.color), instance.model.data());
        }
    }

    // GLSL ES 3.00: `layout(location=...)` fixa as colunas da matriz e a cor.
    // Shader::link() continua fixando 0..3 para os atributos de vértice.
    static constexpr const char* kInstancedVertexShader = R"(#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec4 a_color;
layout(location = 4) in mat4 a_instance_model;
layout(location = 8) in vec4 a_instance_color;
uniform mat4 u_projection;
out vec2 v_uv;
out vec4 v_vertex_color;
out vec4 v_instance_color;
void main() {
    v_uv = a_uv;
    v_vertex_color = a_color;
    v_instance_color = a_instance_color;
    gl_Position = u_projection * a_instance_model * vec4(a_position, 1.0);
}
)";

    static constexpr const char* kInstancedFragmentShader = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
in vec4 v_vertex_color;
in vec4 v_instance_color;
uniform vec4 u_color;
uniform sampler2D u_texture;
uniform int u_use_texture;
out vec4 frag_color;
void main() {
    vec4 base = u_use_texture == 1 ? texture(u_texture, v_uv) : vec4(1.0);
    frag_color = base * v_vertex_color * u_color * v_instance_color;
}
)";

    Shader instanced_shader_;
    bool instanced_shader_ready_ = false;
    GLuint instance_vbo_ = 0;
    GLuint instance_vao_ = 0;
    uint64_t instance_generation_ = gl_detail::context_generation();
};

}  // namespace nativeapp
