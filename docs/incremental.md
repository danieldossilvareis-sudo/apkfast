# Build incremental — v2.3.0

O cache foi reescrito no **schema 2** para ser portável entre checkouts,
máquinas e diretórios de build. O schema 1 (v2.2.0) é invalidado uma vez.

## Onde o cache fica

Padrão:

```text
~/.cache/apkfast/                 # ou $XDG_CACHE_HOME/apkfast
├── .locks/<projectId>/<abi>.lockdir/
├── <projectId>/
│   └── <abi>/
│       ├── cache.json
│       └── dep_<key>.d
└── blobs/<aa>/<sha256>           # .o e .so, sem extensão
```

O diretório é configurável:

```sh
python3 apkfast.py build . --cache-dir /mnt/nfs/apkfast-cache
python3 apkfast.py build . --cache-mode verify
```

```json
{
  "incremental": true,
  "cleanCache": false,
  "cacheDir": "/mnt/nfs/apkfast-cache",
  "cacheMode": "verify",
  "cacheNamespace": "meu-monorepo",
  "requireCacheSignature": false
}
```

Precedência: CLI > `apkfast.json` > padrão.

## Chaves portáveis

As chaves **não** contêm caminhos absolutos. Cada caminho é mapeado para um
token lógico:

```text
<project>/...   <apkfast>/...   <ndk>/...   <sysroot>/...
<glue>/...      <work>/...      <cache>/... <vendor>/...
```

Além disso, o compilador recebe `-ffile-prefix-map` para `__FILE__`/debug info,
então o objeto gerado numa máquina é byte-idêntico ao de outra com o mesmo
código e toolchain.

Entram na chave de cada TU:

- caminho lógico e hash da fonte
- flags canônicas
- fingerprint da toolchain (`--version`, revisão do NDK, ABI/API, glue)
- hash de todos os headers do `nativeapp/`
- estado dos headers do depfile (`-MD -MF`), por caminho lógico + hash

A linkagem também é cacheada por comando canônico, objetos, `.a`/`.so` de
entrada, libs do projeto, nome da lib e modo de strip.

## Comandos

```sh
# build normal (cache ligado)
python3 apkfast.py build .

# ignora leitura/escrita do cache (não apaga nada)
python3 apkfast.py build . --no-incremental

# apaga somente o cache desta ABI/projeto
python3 apkfast.py build . --clean-cache

# transporte entre máquinas
python3 apkfast.py cache-export . -o cache.tar.gz --abi all
python3 apkfast.py cache-import . cache.tar.gz --cache-dir /mnt/nfs/apkfast
```

## Cache compartilhado e segurança

Ver [cache-remote.md](cache-remote.md) para `trusted` vs `verify`, HMAC e
detalhes do formato do bundle.

Resumo:

- `--cache-dir` explícito → modo `verify` por padrão
- cache local padrão → modo `trusted`
- `cache-export`/`cache-import` usam `tar.gz` com `manifest.json` + blobs
- `APKFAST_CACHE_HMAC` assina o manifest; `--require-cache-signature` exige
- locks por `(projectId, ABI)` funcionam em NFS/sshfs (`mkdir` + TTL)

## O que continua rodando

`aapt2`, D8/R8, empacotamento, `zipalign` e `apksigner` executam a cada build.
O ganho é evitar clang e llvm-strip, que dominam o tempo em projetos grandes.
