// nativeapp/math.hpp — Matemática 3D mínima (column-major, compatível GLSL/Vulkan).

#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace nativeapp {

using Mat4 = std::array<float, 16>;
using Vec3 = std::array<float, 3>;

namespace detail {

inline bool all_finite(const Vec3& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

/// Parâmetros que produzem divisão por zero / NaN são recusados.
inline bool perspective_params_valid(float fovy_rad, float aspect,
                                     float near, float far) {
    if (!std::isfinite(fovy_rad) || !std::isfinite(aspect) ||
        !std::isfinite(near) || !std::isfinite(far)) return false;
    if (fovy_rad <= 0.0f || fovy_rad >= std::acos(-1.0f)) return false;
    if (aspect <= 0.0f || near <= 0.0f || far <= near) return false;
    const float t = std::tan(fovy_rad * 0.5f);
    return std::isfinite(t) && t > 0.0f && std::isfinite(near - far);
}

}  // namespace detail

inline Mat4 mat4_identity() {
    return Mat4{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
}

inline Mat4 mat4_multiply(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
    return out;
}

inline Mat4 mat4_translate(float x, float y, float z) {
    Mat4 m = mat4_identity();
    m[12] = x; m[13] = y; m[14] = z;
    return m;
}

inline Mat4 mat4_scale(float x, float y, float z) {
    Mat4 m = mat4_identity();
    m[0] = x; m[5] = y; m[10] = z;
    return m;
}

inline Mat4 mat4_rotate_x(float angle) {
    if (!std::isfinite(angle)) return mat4_identity();
    const float c = std::cos(angle), s = std::sin(angle);
    Mat4 m = mat4_identity();
    m[5] = c; m[6] = s; m[9] = -s; m[10] = c;
    return m;
}

inline Mat4 mat4_rotate_y(float angle) {
    if (!std::isfinite(angle)) return mat4_identity();
    const float c = std::cos(angle), s = std::sin(angle);
    Mat4 m = mat4_identity();
    m[0] = c; m[2] = -s; m[8] = s; m[10] = c;
    return m;
}

inline Mat4 mat4_rotate_z(float angle) {
    if (!std::isfinite(angle)) return mat4_identity();
    const float c = std::cos(angle), s = std::sin(angle);
    Mat4 m = mat4_identity();
    m[0] = c; m[1] = s; m[4] = -s; m[5] = c;
    return m;
}

inline Mat4 mat4_ortho(float left, float right, float bottom, float top,
                       float near = -1.0f, float far = 1.0f) {
    const float dx = right - left;
    const float dy = top - bottom;
    const float dz = far - near;
    // Volume degenerado (largura, altura ou profundidade zero) geraria
    // inf/NaN na matriz e, portanto, geometria invisível ou corrompida.
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) ||
        dx == 0.0f || dy == 0.0f || dz == 0.0f) {
        return mat4_identity();
    }
    Mat4 m = mat4_identity();
    m[0] = 2.0f / dx;
    m[5] = 2.0f / dy;
    m[10] = -2.0f / dz;
    m[12] = -(right + left) / dx;
    m[13] = -(top + bottom) / dy;
    m[14] = -(far + near) / dz;
    return m;
}

inline Mat4 mat4_ortho2d(float width, float height) {
    return mat4_ortho(0.0f, width, height, 0.0f, -1.0f, 1.0f);
}

/// Perspective com profundidade zero-to-one (Vulkan-style).
inline Mat4 mat4_perspective(float fovy_rad, float aspect, float near, float far) {
    if (!detail::perspective_params_valid(fovy_rad, aspect, near, far)) {
        return mat4_identity();
    }
    Mat4 m{};
    const float f = 1.0f / std::tan(fovy_rad / 2.0f);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = far / (near - far);
    m[11] = -1.0f;
    m[14] = (far * near) / (near - far);
    return m;
}

/// Perspective OpenGL-style (-1..1 depth).
inline Mat4 mat4_perspective_gl(float fovy_rad, float aspect, float near, float far) {
    if (!detail::perspective_params_valid(fovy_rad, aspect, near, far)) {
        return mat4_identity();
    }
    Mat4 m{};
    const float f = 1.0f / std::tan(fovy_rad / 2.0f);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (far + near) / (near - far);
    m[11] = -1.0f;
    m[14] = (2.0f * far * near) / (near - far);
    return m;
}

inline Mat4 mat4_look_at(const Vec3& eye, const Vec3& center, const Vec3& up) {
    if (!detail::all_finite(eye) || !detail::all_finite(center) ||
        !detail::all_finite(up)) {
        return mat4_identity();
    }
    const float zx = eye[0] - center[0];
    const float zy = eye[1] - center[1];
    const float zz = eye[2] - center[2];
    const float zl_raw = std::sqrt(zx*zx + zy*zy + zz*zz);
    if (!std::isfinite(zl_raw) || zl_raw <= 1e-6f) {
        // eye == center: não existe direção de visão. Preserva a translação
        // e usa orientação neutra em vez de uma base cheia de zeros/NaN.
        return mat4_translate(-eye[0], -eye[1], -eye[2]);
    }
    const float z0 = zx/zl_raw, z1 = zy/zl_raw, z2 = zz/zl_raw;

    float xx = up[1]*z2 - up[2]*z1;
    float xy = up[2]*z0 - up[0]*z2;
    float xz = up[0]*z1 - up[1]*z0;
    float xl_raw = std::sqrt(xx*xx + xy*xy + xz*xz);
    // up paralelo à direção de visão: usa eixo alternativo.
    if (!std::isfinite(xl_raw) || xl_raw < 1e-6f) {
        const Vec3 alt = (std::fabs(z1) < 0.99f) ? Vec3{0,1,0} : Vec3{0,0,1};
        xx = alt[1]*z2 - alt[2]*z1;
        xy = alt[2]*z0 - alt[0]*z2;
        xz = alt[0]*z1 - alt[1]*z0;
        xl_raw = std::sqrt(xx*xx + xy*xy + xz*xz);
    }
    if (!std::isfinite(xl_raw) || xl_raw <= 1e-6f) {
        return mat4_translate(-eye[0], -eye[1], -eye[2]);
    }
    const float x0 = xx/xl_raw, x1 = xy/xl_raw, x2 = xz/xl_raw;

    const float y0 = z1*x2 - z2*x1;
    const float y1 = z2*x0 - z0*x2;
    const float y2 = z0*x1 - z1*x0;

    Mat4 m = mat4_identity();
    m[0] = x0; m[1] = y0; m[2] = z0;
    m[4] = x1; m[5] = y1; m[6] = z1;
    m[8] = x2; m[9] = y2; m[10] = z2;
    m[12] = -(x0*eye[0] + x1*eye[1] + x2*eye[2]);
    m[13] = -(y0*eye[0] + y1*eye[1] + y2*eye[2]);
    m[14] = -(z0*eye[0] + z1*eye[1] + z2*eye[2]);
    return m;
}

}  // namespace nativeapp
