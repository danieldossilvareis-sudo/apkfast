#!/usr/bin/env sh
# Verificação visual comparável entre o backend GLES2 e o Vulkan.
#
# Gera uma cena GL equivalente à de examples/nativeapp-vulkan/jni/main.cpp,
# builda os dois APKs, instala/valida com scripts/device-check.sh e captura
# <out>/gl.png e <out>/vk.png via "adb exec-out screencap -p".
#
# Uso:
#   sh scripts/visual-parity.sh --out /tmp/visual-parity
#   sh scripts/visual-parity.sh --out /tmp/vp --build-dir /tmp/vp-build \
#       --abi arm64-v8a --serial emulator-5554 --extras
#
# Também aceita ADB, ADB_SERIAL, APKFAST_PARITY_ABI, VISUAL_PARITY_WAIT e
# VISUAL_PARITY_LOG_LINES. Funciona em Termux (mksh/sh) e Linux (dash/bash).

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_ARG=""
BUILD_ARG=""
ABI="${APKFAST_PARITY_ABI:-arm64-v8a}"
SERIAL="${ADB_SERIAL:-}"
ADB_BIN="${ADB:-}"
WAIT_SECONDS="${VISUAL_PARITY_WAIT:-4}"
LOG_LINES="${VISUAL_PARITY_LOG_LINES:-200}"
WITH_EXTRAS=0

usage() {
    cat >&2 <<'EOF'
Uso: sh scripts/visual-parity.sh [opções]

Opções:
  --out CAMINHO        diretório dos APKs, PNGs e logs (default: dist/visual-parity)
  --build-dir CAMINHO  raiz dos build dirs (default: <out>/build)
  --abi ABI            ABI dos APKs (default: arm64-v8a)
  --adb CAMINHO        binário adb (default: $ADB, SDKs ou PATH)
  --serial SERIAL      dispositivo adb (default: $ADB_SERIAL ou primeiro online)
  --wait SEGUNDOS      espera após cada launch/captura (default: 4)
  --lines N            linhas de logcat por verificação (default: 200)
  --extras             também builda/valida image-demo e media-demo
  -h, --help           mostra esta ajuda

Saídas em <out>: gl.apk, vk.apk, gl.png, vk.png, *.log, src/gl_parity_main.cpp.
EOF
}

fail() {
    printf '[✗] visual-parity: %s\n' "$*" >&2
    exit 1
}

note() {
    printf '[✓] visual-parity: %s\n' "$*"
}

info() {
    printf '[…] visual-parity: %s\n' "$*"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --out)
            [ "$#" -ge 2 ] || fail "--out requer um caminho"
            OUT_ARG="$2"
            shift 2
            ;;
        --out=*)
            OUT_ARG="${1#*=}"
            shift
            ;;
        --build-dir)
            [ "$#" -ge 2 ] || fail "--build-dir requer um caminho"
            BUILD_ARG="$2"
            shift 2
            ;;
        --build-dir=*)
            BUILD_ARG="${1#*=}"
            shift
            ;;
        --abi)
            [ "$#" -ge 2 ] || fail "--abi requer um valor"
            ABI="$2"
            shift 2
            ;;
        --abi=*)
            ABI="${1#*=}"
            shift
            ;;
        --adb)
            [ "$#" -ge 2 ] || fail "--adb requer um caminho"
            ADB_BIN="$2"
            shift 2
            ;;
        --adb=*)
            ADB_BIN="${1#*=}"
            shift
            ;;
        --serial)
            [ "$#" -ge 2 ] || fail "--serial requer um valor"
            SERIAL="$2"
            shift 2
            ;;
        --serial=*)
            SERIAL="${1#*=}"
            shift
            ;;
        --wait)
            [ "$#" -ge 2 ] || fail "--wait requer segundos"
            WAIT_SECONDS="$2"
            shift 2
            ;;
        --wait=*)
            WAIT_SECONDS="${1#*=}"
            shift
            ;;
        --lines)
            [ "$#" -ge 2 ] || fail "--lines requer um número"
            LOG_LINES="$2"
            shift 2
            ;;
        --lines=*)
            LOG_LINES="${1#*=}"
            shift
            ;;
        --extras|--with-extras)
            WITH_EXTRAS=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            fail "opção desconhecida: $1"
            ;;
        *)
            fail "argumento posicional inesperado: $1"
            ;;
    esac
done

case "$WAIT_SECONDS" in
    ''|*[!0-9]*) fail "--wait deve ser um inteiro (segundos)" ;;
esac
case "$LOG_LINES" in
    ''|*[!0-9]*) fail "--lines deve ser um inteiro" ;;
esac
[ "$LOG_LINES" -gt 0 ] || fail "--lines deve ser maior que zero"
case "$ABI" in
    all) fail "use uma ABI concreta (ex.: arm64-v8a), não 'all'" ;;
esac

if [ -z "$OUT_ARG" ]; then
    OUT="$ROOT/dist/visual-parity"
else
    case "$OUT_ARG" in
        /*) OUT="$OUT_ARG" ;;
        *) OUT="$PWD/$OUT_ARG" ;;
    esac
fi
if [ -n "$BUILD_ARG" ]; then
    case "$BUILD_ARG" in
        /*) BUILD="$BUILD_ARG" ;;
        *) BUILD="$PWD/$BUILD_ARG" ;;
    esac
else
    BUILD="$OUT/build"
fi

mkdir -p "$OUT" "$OUT/src" "$BUILD" || fail "não foi possível criar os diretórios de saída"

# ---------------------------------------------------------------------------
# adb / dispositivo
# ---------------------------------------------------------------------------

if [ -z "$ADB_BIN" ]; then
    if [ -n "${ANDROID_HOME:-}" ] && [ -x "$ANDROID_HOME/platform-tools/adb" ]; then
        ADB_BIN="$ANDROID_HOME/platform-tools/adb"
    elif [ -n "${ANDROID_SDK_ROOT:-}" ] && [ -x "$ANDROID_SDK_ROOT/platform-tools/adb" ]; then
        ADB_BIN="$ANDROID_SDK_ROOT/platform-tools/adb"
    elif [ -n "${HOME:-}" ] && [ -x "$HOME/Android/android-sdk/platform-tools/adb" ]; then
        ADB_BIN="$HOME/Android/android-sdk/platform-tools/adb"
    else
        ADB_BIN="$(command -v adb 2>/dev/null || true)"
    fi
fi
[ -n "$ADB_BIN" ] || fail "adb não encontrado (use --adb, ADB ou ANDROID_HOME/platform-tools/adb)"
[ -x "$ADB_BIN" ] || fail "adb não é executável: $ADB_BIN"

adb_run() {
    if [ -n "$SERIAL" ]; then
        "$ADB_BIN" -s "$SERIAL" "$@"
    else
        "$ADB_BIN" "$@"
    fi
}

if [ -n "$SERIAL" ]; then
    state="$(adb_run get-state 2>/dev/null || true)"
    [ "$state" = "device" ] || fail "dispositivo $SERIAL não está online (estado: ${state:-desconhecido})"
else
    SERIAL="$(adb_run devices | awk 'NR > 1 && $2 == "device" { print $1; exit }')"
    [ -n "$SERIAL" ] || fail "nenhum dispositivo adb online"
fi

DEVICE_ABI="$(adb_run shell getprop ro.product.cpu.abi | tr -d '\r')"
[ "$DEVICE_ABI" = "$ABI" ] || fail "ABI do dispositivo ($DEVICE_ABI) diverge da ABI pedida ($ABI)"
DEVICE_SIZE="$(adb_run shell wm size | awk '/Physical size/ { print $3; exit }' | tr -d '\r')"
note "dispositivo=$SERIAL abi(device)=$DEVICE_ABI abi(build)=$ABI resolução=${DEVICE_SIZE:-desconhecida}"

# ---------------------------------------------------------------------------
# Cena GL de referência — gerada, não versionada.
# Mesmos clear color, triângulo, sprite e texto do exemplo Vulkan.
# App::set_scene() faz begin_frame()/present() em torno de on_draw().
# ---------------------------------------------------------------------------

GL_SOURCE="$OUT/src/gl_parity_main.cpp"
cat > "$GL_SOURCE" <<'CPP'
// Gerado por scripts/visual-parity.sh. Não editar: o script é a fonte.
// Cena GLES2 com os mesmos parâmetros visuais de
// examples/nativeapp-vulkan/jni/main.cpp.
#include <nativeapp/nativeapp.hpp>

#include <android/log.h>

#include <algorithm>

#define LOG_TAG "visual-parity-gl"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

class ParityApp : public App {
public:
    explicit ParityApp(android_app* app) : App(app) {
        // O loop do App chama begin_frame() antes e present() depois de on_draw().
        set_scene(&scene_);
    }

private:
    void on_ready() override {
        scene_.set_clear_color(0.10f, 0.20f, 0.35f, 1.0f);
    }

    void on_surface_created() override {
        scene_.init(window());
        scene_.resize(width(), height());
        // load_font() exige contexto GL inicializado.
        has_text_ = scene_.load_font("font.ttf", 28);
        if (!has_text_) LOGW("font.ttf ausente: draw_text() será ignorado");
    }

    void on_surface_destroyed() override {
        has_text_ = false;
        scene_.destroy();
    }

    void on_surface_changed(int w, int h) override { scene_.resize(w, h); }

    void on_draw() override {
        const float w = static_cast<float>(width() > 0 ? width() : 1);
        const float h = static_cast<float>(height() > 0 ? height() : 1);

        // 1) Triângulo com cor por vértice (Material branco, sem textura).
        const MeshData triangle = MeshData::triangle();
        Material colored;
        colored.color[0] = 1.0f;
        colored.color[1] = 1.0f;
        colored.color[2] = 1.0f;
        colored.color[3] = 1.0f;
        colored.texture.clear();
        const Mat4 triangle_model = mat4_multiply(
            mat4_translate(w * 0.5f, h * 0.35f, 0.0f),
            mat4_scale(std::min(w, h) * 0.45f, std::min(w, h) * 0.45f, 1.0f));
        scene_.draw(triangle, colored, triangle_model.data());

        // 2) Quad com textura vinda de assets/ (mesmo decode do Vulkan).
        const MeshData quad = MeshData::quad(180.0f, 180.0f);
        Material textured;
        textured.texture = "sprite.png";
        textured.blend = true;
        textured.depth_test = true;
        const Mat4 quad_model = mat4_translate(w * 0.72f, h * 0.55f, 0.0f);
        scene_.draw(quad, textured, quad_model.data());

        // 3) Texto por cima (sem depth), igual ao backend Vulkan.
        if (has_text_ && scene_.has_font()) {
            scene_.draw_text("VULKAN OK", 24.0f, 48.0f, 1.4f,
                             1.0f, 0.95f, 0.65f, 1.0f);
        }
    }

    void on_key(int32_t code, bool down) override {
        if (down && code == AKEYCODE_BACK) request_exit();
    }

    bool has_text_ = false;
    GLES2Scene scene_;
};

APP_MAIN(ParityApp)
CPP

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

build_one() {
    vp_label="$1"
    vp_project="$2"
    vp_package="$3"
    shift 3
    vp_apk="$OUT/$vp_label.apk"
    info "build $vp_label: projeto=$vp_project package=$vp_package abi=$ABI"
    (cd "$ROOT" && python3 "$ROOT/apkfast.py" build "$vp_project" -q \
        --abi "$ABI" --build-dir "$BUILD/$vp_label" -o "$vp_apk" "$@") \
        || fail "build de $vp_label falhou"
    [ -s "$vp_apk" ] || fail "APK não gerado: $vp_apk"
}

GL_ASSETS="$ROOT/examples/nativeapp-vulkan/assets"
[ -f "$GL_ASSETS/sprite.png" ] && [ -f "$GL_ASSETS/font.ttf" ] \
    || fail "assets de referência ausentes em $GL_ASSETS"

build_one gl examples/nativeapp-gl com.apkfast.nativeappgl \
    --src "$GL_SOURCE" --assets "$GL_ASSETS"
build_one vk examples/nativeapp-vulkan com.apkfast.nativeappvk

if [ "$WITH_EXTRAS" -eq 1 ]; then
    build_one image-demo examples/image-demo com.apkfast.imagedemo
    build_one media-demo examples/media-demo com.apkfast.mediademo
fi

# ---------------------------------------------------------------------------
# Instalação + checagem de FATAL
# ---------------------------------------------------------------------------

install_check_one() {
    vp_label="$1"
    vp_package="$2"
    vp_apk="$3"
    adb_run install -r "$vp_apk" >"$OUT/install-$vp_label.log" 2>&1 \
        || { tail -n 20 "$OUT/install-$vp_label.log" >&2; fail "adb install falhou: $vp_label"; }
    adb_run shell pm grant "$vp_package" android.permission.VIBRATE >/dev/null 2>&1 || true
    sh "$ROOT/scripts/device-check.sh" \
        --adb "$ADB_BIN" --serial "$SERIAL" \
        --wait "$WAIT_SECONDS" --lines "$LOG_LINES" \
        --log-file "$OUT/$vp_label-device-check.log" \
        "$vp_apk" "$vp_package" \
        || fail "device-check falhou para $vp_label ($vp_package)"
}

install_check_one gl com.apkfast.nativeappgl "$OUT/gl.apk"
install_check_one vk com.apkfast.nativeappvk "$OUT/vk.apk"
if [ "$WITH_EXTRAS" -eq 1 ]; then
    install_check_one image-demo com.apkfast.imagedemo "$OUT/image-demo.apk"
    install_check_one media-demo com.apkfast.mediademo "$OUT/media-demo.apk"
fi

# ---------------------------------------------------------------------------
# Captura
# ---------------------------------------------------------------------------

check_logcat_fatal() {
    vp_log="$1"
    vp_label="$2"
    if grep -E 'FATAL|dlopen failed|AndroidRuntime.*Exception' "$vp_log" >/dev/null 2>&1; then
        printf '[✗] visual-parity: FATAL/Exception no logcat de %s\n' "$vp_label" >&2
        sed -n '1,20p' "$vp_log" >&2 || true
        exit 1
    fi
}

capture_one() {
    vp_label="$1"
    vp_package="$2"
    vp_png="$3"
    vp_launch="$OUT/launch-$vp_label.log"

    adb_run shell am force-stop "$vp_package" >/dev/null 2>&1 || true
    adb_run logcat -c >/dev/null 2>&1 || true
    adb_run shell monkey -p "$vp_package" -c android.intent.category.LAUNCHER 1 \
        >"$vp_launch" 2>&1 \
        || adb_run shell am start -a android.intent.action.MAIN \
            -c android.intent.category.LAUNCHER -p "$vp_package" \
            >>"$vp_launch" 2>&1 \
        || { tail -n 20 "$vp_launch" >&2; fail "launch falhou: $vp_package"; }
    sleep "$WAIT_SECONDS"

    if ! adb_run shell dumpsys window | grep -F "$vp_package" >/dev/null 2>&1; then
        fail "app sem foco na captura: $vp_package"
    fi

    adb_run exec-out screencap -p >"$vp_png" || fail "screencap falhou: $vp_label"
    [ -s "$vp_png" ] || fail "PNG vazio: $vp_png"
    vp_sig="$(dd if="$vp_png" bs=1 count=8 2>/dev/null | od -An -tx1 | tr -d ' \n')"
    [ "$vp_sig" = "89504e470d0a1a0a" ] || fail "captura não é PNG: $vp_png (assinatura=$vp_sig)"

    vp_logcat="$OUT/$vp_label-screenshot.logcat"
    adb_run logcat -d -t "$LOG_LINES" >"$vp_logcat" 2>/dev/null || true
    check_logcat_fatal "$vp_logcat" "$vp_label"
    note "captura $vp_label: $vp_png"
}

capture_one gl com.apkfast.nativeappgl "$OUT/gl.png"
capture_one vk com.apkfast.nativeappvk "$OUT/vk.png"

# ---------------------------------------------------------------------------
# Resumo
# ---------------------------------------------------------------------------

printf -- '---- resumo visual-parity ----\n'
printf '  dispositivo: %s (abi=%s, resolução=%s)\n' "$SERIAL" "$DEVICE_ABI" "${DEVICE_SIZE:-desconhecida}"
printf '  adb:         %s\n' "$ADB_BIN"
printf '  build-dir:   %s\n' "$BUILD"
printf '  gl:          package=com.apkfast.nativeappgl abi=%s apk=%s png=%s\n' "$ABI" "$OUT/gl.apk" "$OUT/gl.png"
printf '  vk:          package=com.apkfast.nativeappvk abi=%s apk=%s png=%s\n' "$ABI" "$OUT/vk.apk" "$OUT/vk.png"
if [ "$WITH_EXTRAS" -eq 1 ]; then
    printf '  image-demo:  package=com.apkfast.imagedemo abi=%s\n' "$ABI"
    printf '  media-demo:  package=com.apkfast.mediademo abi=%s\n' "$ABI"
fi
printf '  fonte GL:    %s\n' "$GL_SOURCE"
printf '  logs:        %s/*-device-check.log\n' "$OUT"

note "sem FATAL em nenhum app; compare gl.png e vk.png lado a lado"
