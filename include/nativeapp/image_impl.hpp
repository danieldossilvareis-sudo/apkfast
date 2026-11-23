// nativeapp/image_impl.hpp — implementações WebP/SVG (somente o TU injetado).
//
// Este header NÃO deve ser incluído por código de usuário: ele define símbolos
// e liga as implementações NanoSVG. É incluído apenas por
// src/nativeapp_impl.cpp / nativeapp_impl_no_image.cpp.

#pragma once

#include "animated_image.hpp"
#include "animated_svg.hpp"
#include "image_codecs.hpp"
#include "jni_bitmap.hpp"

#ifdef APKFAST_WEBP_FULL
#include <webp/demux.h>
#endif

#ifdef APKFAST_SVG_FULL
#include <lunasvg.h>
#endif

#include <android/bitmap.h>
#include <android/log.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// NanoSVG (zlib). Uma única TU por app define as implementações.
#define NANOSVG_IMPLEMENTATION
#include "third_party/nanosvg.h"
#undef NANOSVG_IMPLEMENTATION

#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvgrast.h"
#undef NANOSVGRAST_IMPLEMENTATION

namespace nativeapp {
namespace image_detail {

inline void warn(const char* message) {
    __android_log_print(ANDROID_LOG_WARN, "nativeapp-image", "%s", message);
}

}  // namespace image_detail

bool decode_webp_jni(const uint8_t* data, size_t size, DecodedImage& out) {
    JniBitmap& holder = JniBitmap::instance();
    JNIEnv* env = holder.env();
    const jobject activity = holder.activity();
    if (!env || !activity) {
        image_detail::warn("WebP: JNI não inicializado (App não injetou o activity)");
        return false;
    }
    if (size == 0 || size > static_cast<size_t>(INT_MAX)) return false;

    jbyteArray bytes = nullptr;
    jclass factory = nullptr;
    jclass options_class = nullptr;
    jobject options = nullptr;
    jclass config_class = nullptr;
    jobject argb = nullptr;
    jobject bitmap = nullptr;

    const auto cleanup = [&]() {
        if (bitmap) env->DeleteLocalRef(bitmap);
        if (argb) env->DeleteLocalRef(argb);
        if (config_class) env->DeleteLocalRef(config_class);
        if (options) env->DeleteLocalRef(options);
        if (options_class) env->DeleteLocalRef(options_class);
        if (factory) env->DeleteLocalRef(factory);
        if (bytes) env->DeleteLocalRef(bytes);
    };
    const auto fail = [&](const char* message) -> bool {
        if (env->ExceptionCheck()) env->ExceptionClear();
        image_detail::warn(message);
        cleanup();
        return false;
    };
    const auto clear_if_exception = [&]() -> bool {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return false;
        }
        return true;
    };

    bytes = env->NewByteArray(static_cast<jsize>(size));
    if (!bytes) return fail("WebP: NewByteArray falhou");
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(size),
                            reinterpret_cast<const jbyte*>(data));
    if (!clear_if_exception()) return fail("WebP: SetByteArrayRegion falhou");

    factory = env->FindClass("android/graphics/BitmapFactory");
    config_class = env->FindClass("android/graphics/Bitmap$Config");
    options_class = env->FindClass("android/graphics/BitmapFactory$Options");
    if (!factory || !config_class || !options_class || !clear_if_exception()) {
        return fail("WebP: classes Android não encontradas");
    }

    jfieldID argb_field = env->GetStaticFieldID(
        config_class, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    if (!argb_field || !clear_if_exception()) return fail("WebP: Config.ARGB_8888 ausente");
    argb = env->GetStaticObjectField(config_class, argb_field);

    jmethodID options_ctor = env->GetMethodID(options_class, "<init>", "()V");
    if (!options_ctor || !clear_if_exception()) return fail("WebP: construtor de Options ausente");
    options = env->NewObject(options_class, options_ctor);

    jfieldID preferred = env->GetFieldID(
        options_class, "inPreferredConfig", "Landroid/graphics/Bitmap$Config;");
    if (preferred) env->SetObjectField(options, preferred, argb);
    jfieldID scaled = env->GetFieldID(options_class, "inScaled", "Z");
    if (scaled) env->SetBooleanField(options, scaled, JNI_FALSE);
    // Evita halo de premultiplicação com glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA).
    jfieldID premultiplied = env->GetFieldID(options_class, "inPremultiplied", "Z");
    if (premultiplied) env->SetBooleanField(options, premultiplied, JNI_FALSE);
    if (!clear_if_exception()) return fail("WebP: Options inválidas");

    jmethodID decode = env->GetStaticMethodID(
        factory, "decodeByteArray",
        "([BIILandroid/graphics/BitmapFactory$Options;)Landroid/graphics/Bitmap;");
    if (!decode || !clear_if_exception()) return fail("WebP: decodeByteArray ausente");
    bitmap = env->CallStaticObjectMethod(factory, decode, bytes, 0,
                                         static_cast<jint>(size), options);
    if (!clear_if_exception()) return fail("WebP: BitmapFactory falhou");
    if (!bitmap) return fail("WebP: BitmapFactory retornou null");

    AndroidBitmapInfo info{};
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        info.width <= 0 || info.height <= 0 ||
        info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        return fail("WebP: bitmap não é RGBA_8888");
    }
    void* pixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS ||
        !pixels) {
        return fail("WebP: lockPixels falhou");
    }

    const size_t row_bytes = static_cast<size_t>(info.width) * 4;
    out.width = info.width;
    out.height = info.height;
    out.rgba.resize(row_bytes * static_cast<size_t>(info.height));
    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    for (int y = 0; y < info.height; ++y) {
        std::memcpy(out.rgba.data() + static_cast<size_t>(y) * row_bytes,
                    src + static_cast<size_t>(y) * info.stride, row_bytes);
    }
    AndroidBitmap_unlockPixels(env, bitmap);

    // Se o device ignorou inPremultiplied=false, desmultiplica aqui.
    if ((info.flags & ANDROID_BITMAP_FLAGS_ALPHA_MASK) ==
        ANDROID_BITMAP_FLAGS_ALPHA_PREMUL) {
        for (size_t offset = 0; offset < out.rgba.size(); offset += 4) {
            const uint8_t alpha = out.rgba[offset + 3];
            if (alpha == 0) {
                out.rgba[offset] = out.rgba[offset + 1] = out.rgba[offset + 2] = 0;
            } else if (alpha != 255) {
                for (int channel = 0; channel < 3; ++channel) {
                    out.rgba[offset + channel] = static_cast<uint8_t>(
                        std::min(255, (out.rgba[offset + channel] * 255) / alpha));
                }
            }
        }
    }

    cleanup();
    return true;
}

bool decode_svg_nano(const uint8_t* data, size_t size, DecodedImage& out,
                     int svg_w, int svg_h) {
    if (!data || size == 0) return false;

    // nsvgParse modifica a string, então trabalhamos numa cópia.
    std::vector<char> text(reinterpret_cast<const char*>(data),
                           reinterpret_cast<const char*>(data) + size);
    text.push_back('\0');
    NSVGimage* image = nsvgParse(text.data(), "px", 96.0f);
    if (!image || image->width <= 0 || image->height <= 0) {
        if (image) nsvgDelete(image);
        image_detail::warn("SVG: documento inválido ou sem dimensões");
        return false;
    }

    const float natural_w = image->width;
    const float natural_h = image->height;
    int width = svg_w > 0 ? svg_w
                          : (svg_h > 0 ? static_cast<int>(std::ceil(svg_h * natural_w / natural_h))
                                       : static_cast<int>(std::ceil(natural_w)));
    int height = svg_h > 0 ? svg_h : static_cast<int>(std::ceil(natural_h * width / natural_w));
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        nsvgDelete(image);
        image_detail::warn("SVG: dimensão de rasterização inválida");
        return false;
    }

    const float scale = std::min(static_cast<float>(width) / natural_w,
                                 static_cast<float>(height) / natural_h);
    const float tx = (static_cast<float>(width) - natural_w * scale) * 0.5f;
    const float ty = (static_cast<float>(height) - natural_h * scale) * 0.5f;

    out.width = width;
    out.height = height;
    out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);

    NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
    if (!rasterizer) {
        nsvgDelete(image);
        return false;
    }
    nsvgRasterize(rasterizer, image, tx, ty, scale,
                  out.rgba.data(), width, height, width * 4);
    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(image);
    return true;
}

// ---------------------------------------------------------------------------
// SVG completo (LunaSVG). nano continua como fallback para ícones simples.
// ---------------------------------------------------------------------------

bool decode_svg_full(const uint8_t* data, size_t size, DecodedImage& out,
                     int svg_w, int svg_h) {
#ifdef APKFAST_SVG_FULL
    if (!data || size == 0) return false;
    auto document = lunasvg::Document::loadFromData(
        reinterpret_cast<const char*>(data), size);
    if (!document) return false;

    const float natural_w = document->width();
    const float natural_h = document->height();
    if (natural_w <= 0 || natural_h <= 0) return false;

    int width = svg_w > 0 ? svg_w
                          : (svg_h > 0 ? static_cast<int>(std::ceil(svg_h * natural_w / natural_h))
                                       : static_cast<int>(std::ceil(natural_w)));
    int height = svg_h > 0 ? svg_h : static_cast<int>(std::ceil(natural_h * width / natural_w));
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;

    const float scale = std::min(static_cast<float>(width) / natural_w,
                                 static_cast<float>(height) / natural_h);
    const float tx = (static_cast<float>(width) - natural_w * scale) * 0.5f;
    const float ty = (static_cast<float>(height) - natural_h * scale) * 0.5f;

    lunasvg::Bitmap bitmap(width, height);
    document->render(bitmap, lunasvg::Matrix(scale, 0, 0, scale, tx, ty));
    bitmap.convertToRGBA();

    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        std::memcpy(out.rgba.data() + static_cast<size_t>(y) * width * 4,
                    bitmap.data() + static_cast<size_t>(y) * bitmap.stride(),
                    static_cast<size_t>(width) * 4);
    }
    return true;
#else
    (void)data; (void)size; (void)out; (void)svg_w; (void)svg_h;
    return false;
#endif
}

bool svg_register_font_asset(const char* family, const std::string& asset,
                             bool bold, bool italic) {
#ifdef APKFAST_SVG_FULL
    if (!family || asset.empty()) return false;
    auto& assets = Assets::instance(nullptr);
    if (!assets.manager()) return false;
    auto bytes = assets.read(asset);
    if (bytes.empty()) return false;
    auto* held = new std::vector<uint8_t>(std::move(bytes));
    return lunasvg_add_font_face_from_data(
        family, bold, italic, held->data(), held->size(),
        [](void* pointer) { delete static_cast<std::vector<uint8_t>*>(pointer); }, held);
#else
    (void)family; (void)asset; (void)bold; (void)italic;
    return false;
#endif
}

bool decode_svg_auto(const uint8_t* data, size_t size, DecodedImage& out,
                     int svg_w, int svg_h) {
#ifdef APKFAST_SVG_FULL
    // Marcadores de recursos avançados: usa LunaSVG; senão mantém NanoSVG
    // (compatibilidade visual com ícones simples da v2.2).
    static const char* markers[] = {
        "<style", "style=", "<text", "<tspan", "clip-path", "mask=",
        "<pattern", "<use", "<marker", "filter=", "<filter", "<image",
        "font-family",
    };
    std::string text(reinterpret_cast<const char*>(data), size);
    for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    bool advanced = false;
    for (const char* marker : markers) {
        if (text.find(marker) != std::string::npos) {
            advanced = true;
            break;
        }
    }
    if (advanced && decode_svg_full(data, size, out, svg_w, svg_h)) return true;
#endif
    return decode_svg_nano(data, size, out, svg_w, svg_h);
}

// ---------------------------------------------------------------------------
// SVG animado: driver SMIL leve sobre LunaSVG.
//
// O parser do LunaSVG ignora elementos desconhecidos (<animate> etc.), então
// os marcadores SMIL são lidos do XML cru e os atributos são aplicados no
// elemento-alvo via Element::setAttribute(). O documento é renderizado de novo
// somente quando algum atributo muda.
// ---------------------------------------------------------------------------

namespace image_detail {
namespace smil {

inline bool space(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

inline std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

inline std::string trim(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && space(value[begin])) ++begin;
    while (end > begin && space(value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

struct Attr {
    std::string name;
    std::string value;
};

inline bool attr(const std::vector<Attr>& attrs, const char* key, std::string& out) {
    const std::string wanted = lower(key);
    for (const Attr& item : attrs) {
        if (lower(item.name) == wanted) {
            out = item.value;
            return true;
        }
    }
    return false;
}

inline std::vector<Attr> parse_attrs(const std::string& body) {
    std::vector<Attr> out;
    size_t index = 0;
    while (index < body.size()) {
        while (index < body.size() && (space(body[index]) || body[index] == '/')) ++index;
        if (index >= body.size()) break;
        const size_t start = index;
        while (index < body.size() && !space(body[index]) &&
               body[index] != '=' && body[index] != '/') {
            ++index;
        }
        if (index == start) {
            ++index;
            continue;
        }
        Attr item;
        item.name = body.substr(start, index - start);
        while (index < body.size() && space(body[index])) ++index;
        if (index < body.size() && body[index] == '=') {
            ++index;
            while (index < body.size() && space(body[index])) ++index;
            if (index < body.size() && (body[index] == '"' || body[index] == '\'')) {
                const char quote = body[index++];
                const size_t value_start = index;
                while (index < body.size() && body[index] != quote) ++index;
                item.value = body.substr(value_start, index - value_start);
                if (index < body.size()) ++index;
            } else {
                const size_t value_start = index;
                while (index < body.size() && !space(body[index])) ++index;
                item.value = body.substr(value_start, index - value_start);
            }
        }
        out.push_back(std::move(item));
    }
    return out;
}

struct Value {
    std::vector<float> numbers;
    std::string suffix;
    std::string literal;  // somente <set>
};

inline bool parse_values(const std::string& raw, std::vector<Value>& out) {
    out.clear();
    size_t start = 0;
    while (start <= raw.size()) {
        const size_t end = raw.find(';', start);
        const std::string part = trim(raw.substr(start, end == std::string::npos
                                                      ? std::string::npos
                                                      : end - start));
        if (!part.empty()) {
            Value value;
            size_t cursor = 0;
            while (cursor <= part.size()) {
                while (cursor < part.size() && (space(part[cursor]) || part[cursor] == ',')) ++cursor;
                if (cursor >= part.size()) break;
                size_t stop = cursor;
                while (stop < part.size() && !space(part[stop]) && part[stop] != ',') ++stop;
                const std::string token = part.substr(cursor, stop - cursor);
                char* token_end = nullptr;
                const float number = std::strtof(token.c_str(), &token_end);
                if (token_end == token.c_str() || !std::isfinite(number)) return false;
                const std::string suffix = lower(trim(token.substr(
                    static_cast<size_t>(token_end - token.c_str()))));
                if (suffix.size() > 4) return false;
                for (char c : suffix) {
                    if (!std::isalpha(static_cast<unsigned char>(c)) && c != '%') return false;
                }
                if (!value.numbers.empty() && suffix != value.suffix) return false;
                value.numbers.push_back(number);
                value.suffix = suffix;
                cursor = stop;
            }
            if (value.numbers.empty()) return false;
            if (!out.empty() && value.numbers.size() != out.front().numbers.size()) return false;
            out.push_back(std::move(value));
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return !out.empty();
}

inline bool parse_duration(const std::string& raw, float& out) {
    std::string text = trim(raw);
    const std::string low = lower(text);
    if (low == "indefinite") return false;
    float scale = 1000.0f;  // número sem unidade = segundos (SMIL)
    if (low.size() >= 2 && low.compare(low.size() - 2, 2, "ms") == 0) {
        text = text.substr(0, text.size() - 2);
        scale = 1.0f;
    } else if (!low.empty() && low.back() == 's') {
        text = text.substr(0, text.size() - 1);
    }
    text = trim(text);
    if (text.empty()) return false;
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(value) || value < 0.0f) return false;
    out = value * scale;
    return true;
}

inline std::string number_text(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    std::string text(buffer);
    while (text.size() > 1 && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    if (text == "-0") text = "0";
    return text;
}

inline int transform_type(const std::string& raw) {
    const std::string type = lower(trim(raw));
    if (type == "translate") return 1;
    if (type == "scale") return 2;
    if (type == "rotate") return 3;
    if (type == "skewx") return 4;
    if (type == "skewy") return 5;
    return 0;
}

inline std::string format_value(const Value& value, int transform) {
    static const char* names[] = {"", "translate", "scale", "rotate", "skewX", "skewY"};
    if (transform > 0 && transform < 6) {
        std::string text = names[transform];
        text += "(";
        for (size_t i = 0; i < value.numbers.size(); ++i) {
            if (i) text += ' ';
            text += number_text(value.numbers[i]);
        }
        text += ")";
        return text;
    }
    std::string text;
    for (size_t i = 0; i < value.numbers.size(); ++i) {
        if (i) text += ' ';
        text += number_text(value.numbers[i]) + value.suffix;
    }
    return text;
}

inline Value mix(const Value& a, const Value& b, float frac) {
    Value out;
    const size_t count = std::min(a.numbers.size(), b.numbers.size());
    for (size_t i = 0; i < count; ++i) {
        out.numbers.push_back(a.numbers[i] + (b.numbers[i] - a.numbers[i]) * frac);
    }
    out.suffix = b.suffix.empty() ? a.suffix : b.suffix;
    return out;
}

struct XmlFrame {
    std::string tag;
    std::string id;
    size_t index = 0;
};

struct Tag {
    std::string kind;
    std::vector<Attr> attrs;
    std::string target_id;
    std::string target_tag;
    size_t target_index = 0;
};

inline bool smil_kind(const std::string& lower_tag, std::string& kind) {
    if (lower_tag == "animate") {
        kind = "animate";
    } else if (lower_tag == "animatetransform") {
        kind = "animatetransform";
    } else if (lower_tag == "set") {
        kind = "set";
    } else {
        return false;
    }
    return true;
}

/// Varre o XML cru mantendo uma pilha de elementos para descobrir o alvo.
inline std::vector<Tag> parse_tags(const std::string& text) {
    std::vector<Tag> out;
    std::vector<XmlFrame> stack;
    std::vector<std::string> seen;
    size_t cursor = 0;
    while (cursor < text.size()) {
        const size_t open = text.find('<', cursor);
        if (open == std::string::npos) break;
        if (text.compare(open, 4, "<!--") == 0) {
            const size_t end = text.find("-->", open + 4);
            cursor = end == std::string::npos ? text.size() : end + 3;
            continue;
        }
        if (text.compare(open, 9, "<![CDATA[") == 0) {
            const size_t end = text.find("]]>", open + 9);
            cursor = end == std::string::npos ? text.size() : end + 3;
            continue;
        }
        if (text.compare(open, 2, "<?") == 0) {
            const size_t end = text.find("?>", open + 2);
            cursor = end == std::string::npos ? text.size() : end + 2;
            continue;
        }
        if (text.compare(open, 2, "<!") == 0) {
            const size_t end = text.find('>', open + 2);
            cursor = end == std::string::npos ? text.size() : end + 1;
            continue;
        }
        const bool closing = open + 1 < text.size() && text[open + 1] == '/';
        size_t index = open + (closing ? 2 : 1);
        while (index < text.size() && space(text[index])) ++index;
        const size_t name_start = index;
        while (index < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[index])) ||
                text[index] == ':' || text[index] == '-' || text[index] == '_')) {
            ++index;
        }
        const std::string tag_name = text.substr(name_start, index - name_start);
        size_t end = index;
        char quote = 0;
        for (; end < text.size(); ++end) {
            const char c = text[end];
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                break;
            }
        }
        if (end >= text.size()) break;
        const std::string body = trim(text.substr(index, end - index));
        const bool self_closing = !body.empty() && body.back() == '/';
        const std::string folded = lower(tag_name);

        if (closing) {
            for (size_t i = stack.size(); i > 0; --i) {
                if (lower(stack[i - 1].tag) == folded) {
                    stack.erase(stack.begin() + static_cast<long>(i - 1));
                    break;
                }
            }
        } else if (!tag_name.empty()) {
            const auto attributes = parse_attrs(body);
            std::string kind;
            if (smil_kind(folded, kind)) {
                Tag tag;
                tag.kind = kind;
                tag.attrs = attributes;
                std::string href;
                if (attr(attributes, "href", href) || attr(attributes, "xlink:href", href)) {
                    href = trim(href);
                    if (href.size() > 1 && href[0] == '#') tag.target_id = href.substr(1);
                    // href externo/não-fragmento é ignorado (documentado).
                } else {
                    for (size_t i = stack.size(); i > 0; --i) {
                        if (!stack[i - 1].id.empty()) {
                            tag.target_id = stack[i - 1].id;
                            break;
                        }
                    }
                    if (tag.target_id.empty() && !stack.empty()) {
                        tag.target_tag = stack.back().tag;
                        tag.target_index = stack.back().index;
                    }
                }
                if (!tag.target_id.empty() || !tag.target_tag.empty()) out.push_back(std::move(tag));
            } else if (!self_closing) {
                XmlFrame frame;
                frame.tag = tag_name;
                frame.index = static_cast<size_t>(
                    std::count(seen.begin(), seen.end(), folded));
                attr(attributes, "id", frame.id);
                seen.push_back(folded);
                stack.push_back(std::move(frame));
            }
        }
        cursor = end + 1;
    }
    return out;
}

}  // namespace smil
}  // namespace image_detail

struct AnimatedSvg::Impl {
#ifdef APKFAST_SVG_FULL
    struct Animation {
        lunasvg::Element target;
        std::string attribute;
        std::string original;
        float begin_ms = 0.0f;
        float dur_ms = 0.0f;
        float repeat = 1.0f;
        bool infinite = false;
        bool freeze = false;
        bool is_set = false;
        int transform = 0;
        std::vector<image_detail::smil::Value> values;
        std::string set_to;
        std::string applied;
    };
    std::vector<Animation> animations;
    std::unique_ptr<lunasvg::Document> document;
#endif
    std::vector<uint8_t> bytes;
    DecodedImage image;
    int svg_w = 0;
    int svg_h = 0;
    int width = 0;
    int height = 0;
    float scale = 1.0f;
    float tx = 0.0f;
    float ty = 0.0f;
    float elapsed_ms = 0.0f;
    float duration_ms = 0.0f;
    int loop_count = 1;
    bool static_only = false;

#ifdef APKFAST_SVG_FULL
    bool build(const std::vector<image_detail::smil::Tag>& tags);
#endif
    bool configure_render();
    void render_document();
    bool apply_at(float time_ms, bool force = false);
};

#ifdef APKFAST_SVG_FULL
bool AnimatedSvg::Impl::build(const std::vector<image_detail::smil::Tag>& tags) {
    using namespace image_detail::smil;
    animations.clear();
    duration_ms = 0.0f;
    loop_count = 1;
    bool infinite = false;
    float last_end = 0.0f;
    int max_repeat = 1;

    for (const Tag& tag : tags) {
        Animation anim;
        if (tag.kind == "set") {
            if (!attr(tag.attrs, "attributeName", anim.attribute) || anim.attribute.empty()) continue;
            if (!attr(tag.attrs, "to", anim.set_to) || trim(anim.set_to).empty()) continue;
            anim.set_to = trim(anim.set_to);
            anim.is_set = true;
        } else {
            if (tag.kind == "animatetransform") {
                anim.attribute = "transform";
                std::string type;
                if (!attr(tag.attrs, "type", type)) continue;
                anim.transform = transform_type(type);
                if (anim.transform == 0) continue;
            } else if (!attr(tag.attrs, "attributeName", anim.attribute) || anim.attribute.empty()) {
                continue;
            }
            std::string values_text;
            if (attr(tag.attrs, "values", values_text) && !trim(values_text).empty()) {
                if (!parse_values(values_text, anim.values) || anim.values.size() < 2) continue;
            } else {
                std::string from;
                std::string to;
                if (!attr(tag.attrs, "from", from) || !attr(tag.attrs, "to", to)) continue;
                std::vector<Value> parsed;
                if (!parse_values(from, parsed) || parsed.empty()) continue;
                const Value first = parsed.front();
                parsed.clear();
                if (!parse_values(to, parsed) || parsed.empty()) continue;
                const Value last = parsed.front();
                if (first.numbers.size() != last.numbers.size()) continue;
                anim.values = {first, last};
            }
        }

        std::string begin;
        if (attr(tag.attrs, "begin", begin)) {
            const std::string folded = lower(trim(begin));
            if (folded.find_first_not_of("0123456789.ms ") != std::string::npos) continue;
            if (!parse_duration(begin, anim.begin_ms)) continue;
        }
        std::string dur;
        if (!attr(tag.attrs, "dur", dur) || !parse_duration(dur, anim.dur_ms) ||
            anim.dur_ms <= 0.0f) {
            continue;
        }
        std::string repeat;
        if (attr(tag.attrs, "repeatCount", repeat)) {
            const std::string folded = lower(trim(repeat));
            if (folded == "indefinite" || folded == "infinite") {
                anim.infinite = true;
            } else {
                char* end = nullptr;
                const float count = std::strtof(folded.c_str(), &end);
                if (end != folded.c_str() + folded.size() || !std::isfinite(count) || count <= 0.0f) {
                    continue;
                }
                anim.repeat = count;
            }
        }
        std::string fill;
        if (attr(tag.attrs, "fill", fill) && lower(trim(fill)) == "freeze") anim.freeze = true;

        if (!document) continue;
        lunasvg::Element target;
        if (!tag.target_id.empty()) {
            target = document->getElementById(tag.target_id);
        } else if (!tag.target_tag.empty()) {
            const auto list = document->querySelectorAll(tag.target_tag);
            if (tag.target_index < list.size()) target = list[tag.target_index];
        }
        if (target.isNull()) continue;
        anim.target = target;
        anim.original = target.getAttribute(anim.attribute);
        anim.applied = anim.original;
        animations.push_back(std::move(anim));

        const Animation& added = animations.back();
        if (added.infinite) {
            infinite = true;
        } else {
            last_end = std::max(last_end, added.begin_ms + added.dur_ms * added.repeat);
            max_repeat = std::max(max_repeat, static_cast<int>(std::ceil(added.repeat)));
        }
    }
    duration_ms = infinite ? 0.0f : last_end;
    loop_count = infinite ? 0 : max_repeat;
    return !animations.empty();
}
#endif

bool AnimatedSvg::Impl::configure_render() {
#ifdef APKFAST_SVG_FULL
    if (document) {
        const float natural_w = document->width();
        const float natural_h = document->height();
        if (natural_w <= 0.0f || natural_h <= 0.0f) return false;
        width = svg_w > 0 ? svg_w
                          : (svg_h > 0 ? static_cast<int>(std::ceil(svg_h * natural_w / natural_h))
                                       : static_cast<int>(std::ceil(natural_w)));
        height = svg_h > 0 ? svg_h : static_cast<int>(std::ceil(natural_h * width / natural_w));
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
        scale = std::min(static_cast<float>(width) / natural_w,
                         static_cast<float>(height) / natural_h);
        tx = (static_cast<float>(width) - natural_w * scale) * 0.5f;
        ty = (static_cast<float>(height) - natural_h * scale) * 0.5f;
        return true;
    }
#endif
    if (image.width <= 0 || image.height <= 0) return false;
    width = image.width;
    height = image.height;
    scale = 1.0f;
    tx = ty = 0.0f;
    return true;
}

void AnimatedSvg::Impl::render_document() {
#ifdef APKFAST_SVG_FULL
    if (document) {
        lunasvg::Bitmap bitmap(width, height);
        if (bitmap.isNull()) {
            image_detail::warn("AnimatedSvg: falha ao alocar bitmap");
            return;
        }
        document->render(bitmap, lunasvg::Matrix(scale, 0, 0, scale, tx, ty));
        bitmap.convertToRGBA();
        image.width = width;
        image.height = height;
        image.rgba.resize(static_cast<size_t>(width) * height * 4);
        for (int y = 0; y < height; ++y) {
            std::memcpy(image.rgba.data() + static_cast<size_t>(y) * width * 4,
                        bitmap.data() + static_cast<size_t>(y) * bitmap.stride(),
                        static_cast<size_t>(width) * 4);
        }
        return;
    }
#endif
    if (!bytes.empty()) decode_svg_auto(bytes.data(), bytes.size(), image, svg_w, svg_h);
}

bool AnimatedSvg::Impl::apply_at(float time_ms, bool force) {
#ifdef APKFAST_SVG_FULL
    using namespace image_detail::smil;
    bool changed = false;
    for (Animation& anim : animations) {
        std::string value = anim.original;
        if (time_ms >= anim.begin_ms) {
            const float local = time_ms - anim.begin_ms;
            const bool active = anim.infinite || local < anim.dur_ms * anim.repeat;
            if (active) {
                if (anim.is_set) {
                    value = anim.set_to;
                } else if (anim.values.size() == 1) {
                    value = format_value(anim.values.front(), anim.transform);
                } else {
                    const float phase = std::fmod(local, anim.dur_ms);
                    const float position = phase / anim.dur_ms *
                                           static_cast<float>(anim.values.size() - 1);
                    const size_t index = std::min(
                        anim.values.size() - 2, static_cast<size_t>(position));
                    const float frac = std::max(0.0f, std::min(1.0f, position - static_cast<float>(index)));
                    value = format_value(mix(anim.values[index], anim.values[index + 1], frac),
                                         anim.transform);
                }
            } else if (anim.freeze) {
                value = anim.is_set ? anim.set_to
                                    : format_value(anim.values.back(), anim.transform);
            }
        }
        if (force || value != anim.applied) {
            anim.target.setAttribute(anim.attribute, value);
            anim.applied = value;
            changed = true;
        }
    }
    return changed;
#else
    (void)time_ms;
    (void)force;
    return false;
#endif
}

AnimatedSvg::AnimatedSvg() = default;

AnimatedSvg::~AnimatedSvg() { close(); }

void AnimatedSvg::close() {
    delete impl_;
    impl_ = nullptr;
    empty_.rgba.clear();
    empty_.width = empty_.height = 0;
}

bool AnimatedSvg::open(const uint8_t* data, size_t size) {
    close();
    if (!data || size == 0) return false;
    const std::string text(reinterpret_cast<const char*>(data), size);
    const auto tags = image_detail::smil::parse_tags(text);

#ifdef APKFAST_SVG_FULL
    auto document = lunasvg::Document::loadFromData(
        reinterpret_cast<const char*>(data), size);
#else
    if (!tags.empty()) {
        image_detail::warn("AnimatedSvg: SMIL requer LunaSVG (lunasvg-install)");
        return false;
    }
#endif

    impl_ = new Impl();
    impl_->bytes.assign(data, data + size);
    impl_->svg_w = svg_w_;
    impl_->svg_h = svg_h_;
#ifdef APKFAST_SVG_FULL
    impl_->document = std::move(document);
#endif

    if (tags.empty()) {
        // Documento sem SMIL: exatamente o caminho estático da v2.3.0.
        if (!decode_svg_auto(data, size, impl_->image, svg_w_, svg_h_) ||
            !impl_->configure_render()) {
            close();
            return false;
        }
        impl_->static_only = true;
        return true;
    }

#ifdef APKFAST_SVG_FULL
    if (!impl_->document || !impl_->configure_render()) {
        close();
        return false;
    }
    if (!impl_->build(tags)) {
        // Marcadores presentes, mas nenhum é utilizável neste subconjunto:
        // mantém o SVG estático em vez de quebrar a app.
        image_detail::warn("AnimatedSvg: nenhum SMIL suportado; usando SVG estático");
        DecodedImage still;
        if (!decode_svg_auto(data, size, still, svg_w_, svg_h_)) {
            close();
            return false;
        }
        impl_->document.reset();
        impl_->image = std::move(still);
        impl_->static_only = true;
        if (!impl_->configure_render()) {
            close();
            return false;
        }
        return true;
    }
    impl_->apply_at(0.0f, true);
    impl_->render_document();
    if (impl_->image.rgba.empty()) {
        close();
        return false;
    }
    return true;
#else
    return false;  // inalcançável: SMIL sem LunaSVG já retornou acima
#endif
}

bool AnimatedSvg::open_asset(const std::string& path) {
    auto& assets = Assets::instance(nullptr);
    if (!assets.manager()) return false;
    const auto bytes = assets.read(path);
    if (bytes.empty()) return false;
    return open(bytes.data(), bytes.size());
}

int AnimatedSvg::width() const { return impl_ ? impl_->width : 0; }
int AnimatedSvg::height() const { return impl_ ? impl_->height : 0; }
float AnimatedSvg::duration_ms() const { return impl_ ? impl_->duration_ms : 0.0f; }
int AnimatedSvg::loop_count() const { return impl_ ? impl_->loop_count : 1; }
size_t AnimatedSvg::animation_count() const {
#ifdef APKFAST_SVG_FULL
    return impl_ ? impl_->animations.size() : 0;
#else
    return 0;
#endif
}

bool AnimatedSvg::update(float dt_ms) {
    if (!impl_ || impl_->static_only || dt_ms <= 0.0f) return false;
    impl_->elapsed_ms += dt_ms;
    if (!impl_->apply_at(impl_->elapsed_ms)) return false;
    impl_->render_document();
    return true;
}

void AnimatedSvg::reset() {
    if (!impl_ || impl_->static_only) return;
    impl_->elapsed_ms = 0.0f;
    impl_->apply_at(0.0f, true);
    impl_->render_document();
}

const DecodedImage& AnimatedSvg::current() const {
    return impl_ ? impl_->image : empty_;
}

bool AnimatedSvg::set_size(int width, int height) {
    svg_w_ = width;
    svg_h_ = height;
    if (!impl_) return true;
    impl_->svg_w = width;
    impl_->svg_h = height;
    if (impl_->static_only) {
        if (!decode_svg_auto(impl_->bytes.data(), impl_->bytes.size(),
                             impl_->image, width, height)) {
            return false;
        }
        impl_->width = impl_->image.width;
        impl_->height = impl_->image.height;
        impl_->scale = 1.0f;
        impl_->tx = impl_->ty = 0.0f;
        return true;
    }
    if (!impl_->configure_render()) return false;
    impl_->apply_at(impl_->elapsed_ms, true);
    impl_->render_document();
    return !impl_->image.rgba.empty();
}

// ---------------------------------------------------------------------------
// WebP animado (libwebp 1.6+, decoder + demux). Sem a lib, JNI puro continua
// cobrindo apenas o primeiro frame.
// ---------------------------------------------------------------------------

bool decode_webp_full(const uint8_t* data, size_t size, DecodedImage& out) {
#ifdef APKFAST_WEBP_FULL
    AnimatedImage animation;
    if (!animation.open(data, size) || animation.frame_count() == 0) return false;
    const auto& first = animation.frame(0);
    out.width = first.width;
    out.height = first.height;
    out.rgba = first.rgba;
    return true;
#else
    (void)data; (void)size; (void)out;
    return false;
#endif
}

AnimatedImage::AnimatedImage() = default;

AnimatedImage::~AnimatedImage() { close(); }

void AnimatedImage::close() {
#ifdef APKFAST_WEBP_FULL
    if (decoder_) {
        WebPAnimDecoderDelete(decoder_);
        decoder_ = nullptr;
    }
    delete data_;
    data_ = nullptr;
#endif
    bytes_.clear();
    frames_.clear();
    index_ = 0;
    elapsed_ms_ = 0.0f;
    plays_ = 0;
    width_ = height_ = 0;
    loop_count_ = 1;
}

bool AnimatedImage::open(const uint8_t* data, size_t size) {
#ifdef APKFAST_WEBP_FULL
    close();
    if (!data || size < 12) return false;

    bytes_.assign(data, data + size);
    data_ = new WebPData{bytes_.data(), bytes_.size()};

    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options)) {
        close();
        return false;
    }
    options.color_mode = MODE_RGBA;
    options.use_threads = 0;

    decoder_ = WebPAnimDecoderNew(data_, &options);
    if (!decoder_) {
        close();
        return false;
    }
    WebPAnimInfo info{};
    if (!WebPAnimDecoderGetInfo(decoder_, &info)) {
        close();
        return false;
    }
    width_ = static_cast<int>(info.canvas_width);
    height_ = static_cast<int>(info.canvas_height);
    loop_count_ = static_cast<int>(info.loop_count);
    if (width_ <= 0 || height_ <= 0 || info.frame_count == 0 || info.frame_count > 4096) {
        close();
        return false;
    }
    const uint64_t total = static_cast<uint64_t>(width_) * height_ * 4 * info.frame_count;
    if (total > 128ULL * 1024 * 1024) {
        image_detail::warn("WebP animado acima do limite de 128 MiB");
        close();
        return false;
    }

    frames_.reserve(info.frame_count);
    int previous_timestamp = 0;
    while (WebPAnimDecoderHasMoreFrames(decoder_)) {
        uint8_t* buffer = nullptr;
        int timestamp = 0;
        if (!WebPAnimDecoderGetNext(decoder_, &buffer, &timestamp)) {
            close();
            return false;
        }
        Frame frame;
        frame.width = width_;
        frame.height = height_;
        frame.duration_ms = std::max(1, timestamp - previous_timestamp);
        previous_timestamp = timestamp;
        frame.rgba.assign(buffer, buffer + static_cast<size_t>(width_) * height_ * 4);
        frames_.push_back(std::move(frame));
    }

    // Os frames já copiaram os pixels; o decoder não é mais necessário.
    WebPAnimDecoderDelete(decoder_);
    decoder_ = nullptr;
    delete data_;
    data_ = nullptr;
    bytes_.clear();
    reset();
    return true;
#else
    (void)data; (void)size;
    return false;
#endif
}

bool AnimatedImage::open_asset(const std::string& path) {
    auto& assets = Assets::instance(nullptr);
    if (!assets.manager()) return false;
    const auto bytes = assets.read(path);
    if (bytes.empty()) return false;
    return open(bytes.data(), bytes.size());
}

const AnimatedImage::Frame& AnimatedImage::frame(size_t index) const {
    static const Frame empty;
    if (frames_.empty()) return empty;
    return frames_[index % frames_.size()];
}

const AnimatedImage::Frame& AnimatedImage::current_frame() const {
    return frame(index_);
}

void AnimatedImage::reset() {
    index_ = 0;
    elapsed_ms_ = 0.0f;
    plays_ = 0;
}

bool AnimatedImage::update(float dt_ms) {
    if (frames_.empty() || dt_ms <= 0.0f) return false;
    const size_t before = index_;
    elapsed_ms_ += dt_ms;

    // Limite de iterações evita duration_ms=0 ou dt enorme travando o loop.
    for (int guard = 0; guard < 1024 && elapsed_ms_ >= frames_[index_].duration_ms; ++guard) {
        elapsed_ms_ -= static_cast<float>(frames_[index_].duration_ms);
        if (index_ + 1 < frames_.size()) {
            ++index_;
            continue;
        }
        ++plays_;
        const bool infinite = force_loop_ || loop_count_ == 0;
        if (infinite || plays_ < static_cast<uint32_t>(loop_count_)) {
            index_ = 0;
        } else {
            index_ = frames_.size() - 1;
            elapsed_ms_ = 0.0f;
            break;
        }
    }
    return index_ != before;
}

}  // namespace nativeapp
