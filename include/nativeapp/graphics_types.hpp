// nativeapp/graphics_types.hpp — Tipos comuns de gráficos (GL e Vulkan).

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace nativeapp {

/// Vértice padrão: posição (x,y,z), normal (nx,ny,nz), UV (u,v), cor (r,g,b,a).
struct Vertex {
    float x = 0, y = 0, z = 0;
    float nx = 0, ny = 0, nz = 1;
    float u = 0, v = 0;
    float r = 1, g = 1, b = 1, a = 1;
};

/// Vértice simples 2D para sprites (x,y,u,v,r,g,b,a).
struct SpriteVertex {
    float x = 0, y = 0;
    float u = 0, v = 0;
    float r = 1, g = 1, b = 1, a = 1;
};

enum class Topology {
    Triangles,
    TriangleStrip,
    TriangleFan,
    Lines,
    LineStrip,
    Points,
};

/// Geometria reutilizável (CPU-side). O backend (GL/VK) faz o upload.
struct MeshData {
    /// Último índice uint16_t é 65535; 65536 é a contagem máxima de vértices.
    static constexpr std::size_t max_vertices = 65536;

    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    Topology topology = Topology::Triangles;

    /// Aponta três vértices novos (índice = coluna uint16_t). Faz sentido
    /// para Topology::Triangles; outras topologias exigem indices manuais.
    void add_triangle(const Vertex& a, const Vertex& b, const Vertex& c) {
        if (vertices.size() > max_vertices - 3) {
            throw std::runtime_error("Mesh excede 65536 vertices (limite uint16)");
        }
        const uint16_t base = static_cast<uint16_t>(vertices.size());
        vertices.push_back(a);
        vertices.push_back(b);
        vertices.push_back(c);
        indices.push_back(base);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
    }

    /// Quatro cantos tl, tr, br, bl → dois triângulos (diagonal tl→br).
    /// Cada triângulo tem 3 vértices próprios, então o quad consome 6.
    /// A capacidade é verificada antes de qualquer push_back: um quad que
    /// não cabe não deixa a malha pela metade.
    void add_quad(const Vertex& tl, const Vertex& tr,
                  const Vertex& br, const Vertex& bl) {
        if (vertices.size() > max_vertices - 6) {
            throw std::runtime_error("Mesh excede 65536 vertices (limite uint16)");
        }
        add_triangle(tl, tr, br);
        add_triangle(tl, br, bl);
    }

    static MeshData quad(float w = 1.0f, float h = 1.0f) {
        MeshData mesh;
        const float hw = w / 2.0f, hh = h / 2.0f;
        // UVs com V invertido: stb/libwebp/LunaSVG entregam a primeira linha
        // no topo, enquanto GL_TEXTURE_2D coloca t=0 na base.
        mesh.add_quad(
            Vertex{-hw,  hh, 0, 0,0,1, 0,1, 1,1,1,1},
            Vertex{ hw,  hh, 0, 0,0,1, 1,1, 1,1,1,1},
            Vertex{ hw, -hh, 0, 0,0,1, 1,0, 1,1,1,1},
            Vertex{-hw, -hh, 0, 0,0,1, 0,0, 1,1,1,1});
        return mesh;
    }

    static MeshData triangle() {
        MeshData mesh;
        mesh.add_triangle(
            Vertex{ 0.0f,  0.5f, 0, 0,0,1, 0.5f,1, 1,0.3f,0.3f,1},
            Vertex{-0.5f, -0.5f, 0, 0,0,1, 0,0,    0.3f,1,0.3f,1},
            Vertex{ 0.5f, -0.5f, 0, 0,0,1, 1,0,    0.3f,0.3f,1,1});
        return mesh;
    }
};

/// Material simples: cor + textura opcional (nome do arquivo em assets).
struct Material {
    float color[4] = {1, 1, 1, 1};
    std::string texture;
    bool depth_test = true;
    bool blend = true;
};

/// Interface comum para cenas GL/Vulkan (troca de backend sem mudar o app).
class Scene {
public:
    virtual ~Scene() = default;
    virtual void resize(int width, int height) = 0;
    virtual void begin_frame() = 0;
    virtual void present() = 0;
    virtual void set_clear_color(float r, float g, float b, float a = 1.0f) = 0;

    /// Desenha uma mesh com material e matriz model (16 floats column-major).
    virtual void draw(const MeshData&, const Material&, const float* model = nullptr) = 0;
};

}  // namespace nativeapp
