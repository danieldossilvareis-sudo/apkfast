# Build incremental — v2.2.0

O apkfast agora recompila apenas o que mudou. O cache é por **ABI**, por
**projeto** e por **build dir**, e fica fora do diretório de build (que é
apagado a cada execução).

## Onde o cache fica

```text
<build-dir>/../.apkfast-cache/<hash-do-projeto>/<abi>/
├── cache.json
├── obj_<hash>.o
├── obj_<hash>.d
└── lib<name>.so
```

Com o build dir padrão:

```text
meu-projeto/build/.apkfast-cache/<hash>/arm64-v8a/
```

Como `.apkfast-cache` fica ao lado de `build/`, ele sobrevive ao `_clean()`.
`DEFAULT_IGNORED_DIRS` já ignora `build/` e `.apkfast/`, então nenhum artefato
é coletado como fonte.

## O que entra na chave

Cada unidade de compilação é identificada por:

- caminho absoluto da fonte
- comando exato do compilador (flags, includes, std, otimização)
- identidade do compilador (`--version`, NDK, sysroot, glue)
- hash de todos os headers do `nativeapp/`
- hash do conteúdo da fonte
- estado dos headers do depfile (`-MD -MF`) — caminho + hash do conteúdo

Se um header muda, todas as TUs que o incluem são recompiladas. Se a toolchain,
as flags ou a ABI mudam, a chave muda.

A linkagem também é cacheada: a chave inclui o comando do linker, os objetos,
`.a`/`.so` de entrada, libs do projeto, nome da lib e modo de strip. Num build
sem mudanças, **nenhum clang/llvm-strip é chamado**.

## Uso

```sh
# cache ligado (padrão)
python3 apkfast.py build .

# desligar (recompila tudo)
python3 apkfast.py build . --no-incremental

# apagar só o cache e reconstruir
python3 apkfast.py build . --clean-cache
```

`apkfast.json`:

```json
{
  "incremental": true,
  "cleanCache": false
}
```

Flags de CLI têm precedência sobre o JSON.

## O que continua rodando

`aapt2`, empacotamento do APK, `zipalign` e `apksigner` ainda executam a cada
build. O ganho está em evitar o compilador C/C++ e o linker, que dominam o
tempo em projetos grandes.

## Fallback seguro

Cache corrompido, schema diferente, objeto faltando ou header removido causam
recompilação — nunca falham o build. Escrita de `cache.json` é atômica
(`tmp` + `os.replace`).

## Limitações

- O cache é local; não há cache remoto/distribuído.
- Um `.a` próprio alterado invalida a linkagem, mas não as TUs que apenas o
  declaram via `extern` (o compilador não depende dele).
- Trocar de `--build-dir` cria outro cache.
