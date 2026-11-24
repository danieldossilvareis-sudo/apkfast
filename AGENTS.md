# AGENTS.md

## Contrato da base

apkfast é **CLI + runtime `nativeapp/` para Android nativo em C/C++**, não engine nem framework.

- Fica na base: toolchain/ABIs, build/APK/AAB, assinatura, cache, lockfile, lifecycle/looper, EGL/GLES/Vulkan, input, permissões, teclado, assets, codecs, fonte/texto, sensores/device, glue raylib/SDL, logs/report/doctor.
- Não entra: ECS, física, câmera, UI widgets, animação de jogo, save, rede, mixer 3D, scripting, editor, pipeline de assets com baking.
- Dependência é `framework → apkfast`; nunca o contrário. Se a mudança passa a dizer “como o app deve ser organizado”, saiu do escopo.

## Entrypoints e layout não óbvios

- `apkfast.py` é o script raiz; entry point instalado é `apkfast = apkfast.cli:main`. A lógica vive em `apkfast/cli.py` e `apkfast/builder.py`.
- Versão: `apkfast/__init__.py`. `include/nativeapp/version.hpp` deve acompanhar. Não bump sem release autorizada.
- O root `apkfast.py` sombreia o pacote no setuptools; por isso `pyproject.toml` usa `package-dir apkfast = "apkfast/."`. Não “corrigir”.
- O builder resolve `<pacote>/../include`, `src/`, `patches/` e `docs/`; a wheel os instala como namespace packages. Não mover esses diretórios.
- Implementações third-party ficam em `src/nativeapp_impl*.cpp` + `include/nativeapp/image_impl.hpp`; raylib usa `nativeapp_impl_no_image.cpp`.

## Setup e fluxo

- Python ≥ 3.11. SDK/NDK em `~/Android` ou `APKFAST_ANDROID_ROOT`; verifique com `apkfast doctor`.
- `setup-libs --abis <abi>` baixa/compila raylib/SDL/libwebp/LunaSVG. `vendor/` não é versionado.
- `build --generate-lock` **só gera o lockfile e sai**. Lock divergente falha; `--ignore-lock` transforma em aviso.
- `--no-incremental` ignora leitura/escrita do cache; `--clean-cache` apaga só o escopo do projeto/ABI.

## Comandos de verificação

```sh
python3 -m compileall -q apkfast apkfast.py tests
python3 -m unittest discover -s tests -v
python3 -m unittest tests.test_bundle -v          # um arquivo específico
python3 apkfast.py config show examples/native-hello
python3 apkfast.py build examples/native-hello -q
python3 apkfast.py build examples/image-demo --aab -q
sh scripts/device-check.sh app.apk com.example.nativehello
```

- Testes unitários são puros: não exigem SDK/NDK. CI valida lockfile com toolchain fake.
- `ci.yml`: sintaxe/help/config/lock/testes. `device.yml`: **build matrix**, não runtime em dispositivo. `device-real.yml`: depende de runner self-hosted ou Firebase e fica em skip sem config.

## Armadilhas

- Chaves de cache devem usar tokens lógicos (`<project>`, `<ndk>`, `<cache>`, ...), nunca paths absolutos. Os `-ffile-prefix-map` do builder são necessários.
- Keystore/senhas vêm de `APKFAST_KEYSTORE`, `APKFAST_STORE_PASS`, `APKFAST_KEY_PASS`, `APKFAST_KEY_ALIAS`. Nunca logar; use `signing.redact_args()`.
- bundletool 1.18.3 é pinned: `APKFAST_BUNDLETOOL_SHA256` ou `--bundletool-version`; cache em `~/.apkfast/tools` (`APKFAST_TOOLS_DIR`).
- raylib/SDL2/SDL3 são pinned por versão + SHA256. Trocar a versão sem SHA falha de propósito.
- AAB = `bundletool build-bundle` + `jarsigner`. APK = `apksigner` v1/v2/v3/v4 + `.idsig` quando v4.
- `--quiet` é contrato: erros em `stderr` com `[✗]`; nenhum output de subprocess deve vazar.
- Se docs e código divergirem, `--help`, CI e testes são a fonte de verdade.

## Publicação e estado das releases

- `dist` gera/valida sdist+wheel; `publish-pypi` é **dry-run por padrão**, exige `--yes` para upload real.
- Token local: `TWINE_USERNAME`/`TWINE_PASSWORD` ou `TWINE_API_TOKEN`/`PYPI_TOKEN`. No GitHub: secret `PYPI_API_TOKEN`.
- Não publique nada sem autorização explícita.
- Snapshot atual: **v2.7.3 é a única release pública** no GitHub; as antigas estão em draft com tags preservadas. Não alterar sem pedido.
