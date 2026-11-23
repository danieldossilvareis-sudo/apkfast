# Compatibilidade e contrato de API — v2.4.0

A API `nativeapp/` usa **Semantic Versioning** e um número dedicado, disponível
em [`include/nativeapp/version.hpp`](../include/nativeapp/version.hpp):

```cpp
#include <nativeapp/nativeapp.hpp>

#if APKFAST_API_VERSION < 20400
#  error " Este projeto exige apkfast >= 2.4.0"
#endif

const char* versao = APKFAST_API_VERSION_STRING;  // "2.4.0"
```

O número da API acompanha a versão do apkfast (2.4.0 → API 2.4.0). O pacote
Python expõe `apkfast.__version__` e `apkfast --version` com o mesmo valor.

## O que é prometido

| Promessa | Detalhe |
|---|---|
| Compatibilidade de **código-fonte** dentro do mesmo MAJOR | Código que compila com 2.3.x continua compilando com 2.4.x, salvo avisos de depreciação. |
| Nomes e assinaturas públicos | `nativeapp::App`, `GLES2Scene`, `VulkanScene`, `Permissions`, helpers de áudio/imagem/device etc. |
| Comportamento documentado | Ciclo de vida (`on_ready`, `on_surface_created`, `on_draw`, ...), convenção `APP_MAIN`, `lib_name=main`. |
| Backend de build | Mesma toolchain NDK detectada, com `--sdk-dir`/`--ndk-dir`/`--build-tools` como saída de escape. |
| Formato do lockfile | `lockVersion: 1` é estável; mudanças incompatíveis incrementam o número. |
| Formato de `apkfast.json` | Chaves conhecidas e tipos são validados; chaves desconhecidas geram erro claro em vez de silêncio. |

## O que **não** é prometido

- **ABI.** Os headers são header-only e compilados dentro do APK. Não existe
  `libnativeapp.so` para linkar, nem layout estável de classes/structs, nem
  símbolos estáveis. Um objeto, `.so` ou bundle de cache gerado por outra
  versão do apkfast/NDK/STL **não** é compatível — recompile sempre.
- API de funções `internal::`/detalhes de implementação, macros sem prefixo
  `APKFAST_`, nem o conteúdo de `vendor/` (LunaSVG/libwebp baixados por
  `setup-libs`).
- A ordem de campos internos, tamanhos de enum, alinhamento e convenções de
  chamada dependentes de compilador/ABI Android (arm64-v8a vs armeabi-v7a).
- Que uma API deprecada continue disponível para sempre (ver prazo abaixo).
- Reprodução byte a byte do APK entre versões diferentes do NDK/build-tools:
  use o lockfile (`apkfast.lock.json`) para fixar a toolchain.

## Política de depreciação

1. Uma API é marcada com `APKFAST_DEPRECATED("use X")` e documentada na
   [migração](migration.md) numa versão **MINOR**.
2. Permanece disponível e funcional por **pelo menos duas versões MINOR**
   (ou 12 meses, o que for maior), emitindo aviso de compilação.
3. Só é removida numa versão **MAJOR**, com substituto documentado.
4. Correções de segurança podem quebrar esse prazo; nesse caso a exceção é
   explicada no `CHANGELOG.md` e na nota da release.

## Lockfile e CI

`apkfast.lock.json` registra NDK, build-tools, API da platform, versões de
libwebp/LunaSVG e checksums disponíveis. `apkfast build . --generate-lock`
cria o arquivo; builds seguintes avisam (ou falham, use `--ignore-lock` apenas
de forma consciente) quando a toolchain detectada divergir. A matriz de
dispositivos em `.github/workflows/device.yml` cobre API 21/28/36 nas ABIs
arm64-v8a e armeabi-v7a.
