#!/usr/bin/env sh
# Valida um APK em dispositivo Android real via adb.
#
# Uso:
#   scripts/device-check.sh <apk> <package> [--serial SERIAL] [--adb CAMINHO]
#
# Também aceita variáveis de ambiente: ADB, ADB_SERIAL, DEVICE_WAIT,
# DEVICE_LOGCAT_LINES. Funciona em Termux (mksh/sh) e Linux (dash/bash).
# Falha se o logcat contiver FATAL, "dlopen failed" ou AndroidRuntime.*Exception.

set -eu

APK=""
PKG=""
SERIAL="${ADB_SERIAL:-}"
ADB_BIN="${ADB:-}"
WAIT_SECONDS="${DEVICE_WAIT:-8}"
LOG_LINES="${DEVICE_LOGCAT_LINES:-300}"
LOG_FILE_ARG=""
KEEP_LOG=0

usage() {
    cat >&2 <<'EOF'
Uso: scripts/device-check.sh [opções] <apk> <package>

Opções:
  --adb CAMINHO       usa este adb (senão: $ADB, ANDROID_HOME, PATH)
  --serial SERIAL     usa este dispositivo (senão: $ADB_SERIAL ou primeiro online)
  --wait SEGUNDOS     espera após o launch (padrão: 8)
  --lines N           linhas de logcat (padrão: 300)
  --log-file CAMINHO  salva o logcat neste arquivo
  --keep-log          mantém o logcat temporário mesmo sem --log-file
  -h, --help          mostra esta ajuda

Exemplo:
  sh scripts/device-check.sh --log-file device-check.log \
      dist/native-hello.apk com.example.nativehello
EOF
}

fail() {
    printf '[✗] device-check: %s\n' "$*" >&2
    exit 1
}

note() {
    printf '[✓] device-check: %s\n' "$*"
}

set_positional() {
    if [ -z "$APK" ]; then
        APK="$1"
    elif [ -z "$PKG" ]; then
        PKG="$1"
    else
        fail "argumento extra inesperado: $1"
    fi
}

while [ "$#" -gt 0 ]; do
    case "$1" in
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
        --log-file)
            [ "$#" -ge 2 ] || fail "--log-file requer um caminho"
            LOG_FILE_ARG="$2"
            shift 2
            ;;
        --log-file=*)
            LOG_FILE_ARG="${1#*=}"
            shift
            ;;
        --keep-log)
            KEEP_LOG=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            while [ "$#" -gt 0 ]; do
                set_positional "$1"
                shift
            done
            break
            ;;
        -*)
            fail "opção desconhecida: $1"
            ;;
        *)
            set_positional "$1"
            shift
            ;;
    esac
done

[ -n "$APK" ] || { usage; fail "APK não informado"; }
[ -n "$PKG" ] || { usage; fail "package não informado"; }
[ -f "$APK" ] || fail "APK não encontrado: $APK"

case "$WAIT_SECONDS" in
    ''|*[!0-9]*) fail "--wait deve ser um inteiro (segundos)" ;;
esac
case "$LOG_LINES" in
    ''|*[!0-9]*) fail "--lines deve ser um inteiro positivo" ;;
esac
[ "$LOG_LINES" -gt 0 ] || fail "--lines deve ser maior que zero"

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

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/apkfast-device-check.XXXXXX")" || fail "mktemp falhou"
cleanup() {
    [ -d "$TMP_DIR" ] && rm -rf "$TMP_DIR"
    return 0
}
trap cleanup 0 1 2 15

if [ -n "$LOG_FILE_ARG" ]; then
    LOG_FILE="$LOG_FILE_ARG"
    log_dir="$(dirname "$LOG_FILE")"
    [ -d "$log_dir" ] || mkdir -p "$log_dir" || fail "não foi possível criar: $log_dir"
else
    LOG_FILE="$TMP_DIR/logcat.txt"
fi

if [ -n "$SERIAL" ]; then
    state="$(adb_run get-state 2>/dev/null || true)"
    [ "$state" = "device" ] || fail "dispositivo $SERIAL não está online (estado: ${state:-desconhecido})"
else
    SERIAL="$(adb_run devices | awk 'NR > 1 && $2 == "device" { print $1; exit }')"
    [ -n "$SERIAL" ] || fail "nenhum dispositivo adb online (verifique USB/Xiaomi: autorize a depuração)"
fi

printf '[…] device-check: dispositivo=%s apk=%s package=%s\n' "$SERIAL" "$APK" "$PKG"

adb_run logcat -c >/dev/null 2>&1 || true

INSTALL_LOG="$TMP_DIR/install.log"
if ! adb_run install -r "$APK" >"$INSTALL_LOG" 2>&1; then
    printf '[✗] device-check: adb install -r falhou\n' >&2
    tail -n 20 "$INSTALL_LOG" >&2 || true
    exit 1
fi

START_LOG="$TMP_DIR/start.log"
if adb_run shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >"$START_LOG" 2>&1; then
    START_MODE="monkey"
elif adb_run shell am start -a android.intent.action.MAIN \
        -c android.intent.category.LAUNCHER -p "$PKG" >>"$START_LOG" 2>&1; then
    START_MODE="am"
else
    printf '[✗] device-check: não foi possível iniciar o app (monkey e am falharam)\n' >&2
    tail -n 20 "$START_LOG" >&2 || true
    exit 1
fi

sleep "$WAIT_SECONDS"

LOGCAT_ERR="$TMP_DIR/logcat.err"
if ! adb_run logcat -d -b main -b crash -b system -t "$LOG_LINES" \
        >"$LOG_FILE" 2>"$LOGCAT_ERR"; then
    # adb antigo/Termux pode não aceitar múltiplos -b.
    if ! adb_run logcat -d -t "$LOG_LINES" >"$LOG_FILE" 2>>"$LOGCAT_ERR"; then
        printf '[✗] device-check: falha ao capturar logcat\n' >&2
        tail -n 10 "$LOGCAT_ERR" >&2 || true
        exit 1
    fi
fi

PATTERN='FATAL|dlopen failed|AndroidRuntime.*Exception'
MATCHES="$TMP_DIR/matches.txt"
ERRORS=0
if grep -E "$PATTERN" "$LOG_FILE" >"$MATCHES" 2>/dev/null; then
    ERRORS="$(awk 'END { print NR }' "$MATCHES")"
fi
LOG_TOTAL="$(awk 'END { print NR }' "$LOG_FILE")"

summary() {
    printf -- '---- resumo device-check ----\n'
    printf '  apk:      %s\n' "$APK"
    printf '  package:  %s\n' "$PKG"
    printf '  device:   %s\n' "$SERIAL"
    printf '  adb:      %s\n' "$ADB_BIN"
    printf '  launch:   %s\n' "$START_MODE"
    printf '  logcat:   %s linha(s), %s erro(s)\n' "$LOG_TOTAL" "$ERRORS"
    printf '  log:      %s\n' "$LOG_FILE"
}

summary

if [ "$ERRORS" -gt 0 ]; then
    printf '[✗] device-check: padrão de crash encontrado no logcat\n' >&2
    sed -n '1,20p' "$MATCHES" >&2 || true
    exit 1
fi

if [ "$KEEP_LOG" -eq 0 ] && [ -z "$LOG_FILE_ARG" ]; then
    printf '  (use --log-file para preservar o logcat)\n'
fi
note "app iniciou sem FATAL, dlopen failed ou AndroidRuntime exceptions"
