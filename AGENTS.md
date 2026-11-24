# AGENTS.md

Instruções para agentes/contribuidores que trabalham neste repositório.

## O que é apkfast

apkfast é a **base** para apps Android nativos em C/C++ sem Gradle:

- CLI de build, empacotamento, assinatura, AAB, cache e distribuição;
- API C++ header-only `nativeapp/` como camada de plataforma;
- glue com raylib, SDL2 e SDL3.

Versão estável atual: **2.7.3** (`apkfast/__init__.py`).
Não altere a versão sem instrução explícita.

## Limite de escopo — leia antes de propor features

apkfast é uma base, **não é uma engine nem um framework de apps**.

### Fica na base

- toolchain, ABIs, NDK/SDK, lockfile, build, APK/AAB, assinatura, cache;
- lifecycle/looper, EGL/GLES/Vulkan, input bruto, permissões, teclado virtual;
- assets, codecs de imagem/áudio, fonte e texto básico;
- sensores, device, glue com engines;
- logs, report, doctor, transporte de cache.

Regra: se é difícil, universal e específico do Android, fica aqui.

### Não entra na base

- ECS, scene graph, física, colisão, câmera;
- UI widgets, layout, temas;
- animação de jogo, state machines, skeletal animation;
- save system, serialização de domínio, rede/multiplayer;
- mixer 3D/posicional, scripting, editor visual, pipeline de assets com baking.

Esses itens pertencem a repositórios separados que dependem do apkfast:

```text
framework → apkfast
apkfast  ↛ framework
```

A base nunca deve saber que existe um framework em cima dela.

## Mapa rápido

| Caminho | Papel |
|---|---|
| `apkfast.py`, `apkfast/cli.py` | entry point e comandos |
| `apkfast/builder.py` | pipeline de build |
| `apkfast/cache.py`, `resource_cache.py`, `cache_transport.py` | cache local/remoto |
| `apkfast/bundle.py` | AAB via bundletool |
| `apkfast/signing.py` | assinatura e versionCode |
| `apkfast/raylib.py`, `sdl.py`, `webp.py`, `lunasvg.py` | setup de libs pinned |
| `include/nativeapp/` | API C++ header-only |
| `src/nativeapp_impl*.cpp` | TUs injetadas com implementações third-party |
| `examples/` | 14 exemplos funcionais |
| `tests/` | testes unitários |
| `docs/` | guias por helper |
| `scripts/device-check.sh` | validação adb local |
| `vendor/` | **não versionado** — baixado por `setup-libs` |

## Regras de ouro

1. **Não commite binários.** Nada de `.a`, `.so`, `.apk`, `.aab`, keystore ou `vendor/`.
2. **Fontes third-party pinned.** raylib/SDL/libwebp/LunaSVG com versão e SHA256 fixados.
3. **API é source-compatible, não ABI-compatible.** Headers são compilados dentro do app.
4. **Mude `version.hpp` e `__init__.py` juntos**, somente em release autorizada.
5. **Cache precisa ser portável.** Nunca coloque paths absolutos nas chaves; use os tokens lógicos de `cache.py`.
6. **Quiet mode é contrato.** Erros sempre em `stderr` com `[✗]`; `--quiet` não vaza output.
7. **Segredos nunca em log.** Use `signing.redact_args()` / variáveis de ambiente.
8. **Docs acompanham código.** Versões de libs, comandos e pipeline descritos no README devem bater com o código.
9. **Não publique nada sem autorização.** PyPI e releases são ações externas e irreversíveis.

## Validação antes de dizer que terminou

```sh
python3 -m compileall -q apkfast apkfast.py tests
python3 -m unittest discover -s tests -v
python3 apkfast.py build examples/native-hello -q
python3 apkfast.py build examples/image-demo --aab -q
sh scripts/device-check.sh dist/app.apk com.example.nativehello
```

Também vale conferir links locais dos Markdown e o `--help` de comandos novos.

## CI e releases

- `ci.yml`: sintaxe, help, testes, config/lock.
- `device.yml`: **build matrix**, não runtime em dispositivo.
- `device-real.yml`: dispositivo real, mas depende de runner self-hosted ou Firebase; fica em skip se nada estiver configurado.
- `publish.yml`: publica no PyPI quando há token/OIDC configurado.

Estado atual das releases do GitHub:

- **v2.7.3 é a única pública**
- releases antigas estão em **draft**, com tags preservadas
- não mude isso sem pedido explícito

## Tom das mudanças

Prefira mudanças pequenas, reversíveis e auditáveis.
Se uma alteração começar a responder “como o app deve ser organizado”, ela provavelmente saiu do escopo do apkfast.
