# Contribuindo

Obrigado pelo interesse!Este projeto é focado em builds Android sem Gradle,
com NativeActivity/GLES/Vulkan/raylib/SDL.

## Começando

```sh
git clone <repo>
cd apkfast
python3 apkfast.py doctor
python3 apkfast.py setup-libs --abis arm64-v8a
python3 apkfast.py build examples/native-hello
```

Requisitos: Android SDK + NDK em `~/Android`, Python 3.10+, `javac`, `cmake`
(para `setup-libs`).

## Estrutura

| Caminho | Descrição |
|---|---|
| `apkfast/` | Código do CLI (toolchain, builder, device, raylib, sdl) |
| `include/nativeapp/` | API C++ header-only (App, GLES2Scene, VulkanScene, Permissions) |
| `examples/` | Projetos de exemplo funcionais |
| `vendor/` | **Não versionado** — libs baixadas por `setup-libs` |

## Convenções

- Mensagens de commit em português ou inglês, imperativas e curtas
- `print` de erro sempre em `stderr` com prefixo `[✗]`
- Novos comandos devem respeitar `--quiet`
- Ao mexer no builder, valide com `python3 apkfast.py build examples/<ex> -q`

## Adicionando suporte a uma biblioteca

1. Crie `apkfast/<lib>.py` com `install(abi, api, quiet)` via CMake/NDK
2. Detecte o `#include` em `builder.py` e adicione includes/estática/deps
3. Adicione um exemplo em `examples/<lib>-hello/`
4. Documente no README

## Pull requests

- Descreva o que mudou e como testou no dispositivo (logcat/screenshot ajudam)
- Não commite `.a`, `.so`, `.apk`, keystores ou `vendor/`
- Rode `python3 -m py_compile apkfast/*.py` antes de abrir o PR
