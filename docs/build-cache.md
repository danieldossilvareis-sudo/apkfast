# Cache de recursos e APK (v2.6.0)

O apkfast já cacheava objetos C/C++ e a link da `.so` (schema 2,
[build incremental](incremental.md)). A v2.6.0 estende o mesmo mecanismo para
**recursos compilados** e para o **APK**, reaproveitando blobs por SHA256, locks
por `(projectId, ABI)` e os modos `trusted`/`verify`.

Nada muda na localização do cache:

```text
~/.cache/apkfast/                     # ou $XDG_CACHE_HOME/apkfast
├── <projectId>/<abi>/cache.json      # agora com: resource, apkLink, apk
└── blobs/<aa>/<sha256>               # flats (ZIP), APK unsigned e APK assinado
```

## O que é cacheado

| Estágio | Entrada em `cache.json` | Conteúdo do blob | O que ainda roda no hit |
|---------|-------------------------|------------------|-------------------------|
| `resource` | `resource` | ZIP com todos os `.flat` do `aapt2 compile` | nada (sem `aapt2 compile`) |
| `link` | `apkLink` | APK após `aapt2 link` + arquivos extras (`.so`/`.dex`) | nada (sem `aapt2 link`/zipfile) |
| `apk` | `apk` | APK já alinhado e assinado | nada (sem `zipalign`/`apksigner`) |
| `native` (v2.3) | `objects` / `link` | `.o` e `lib*.so` | nada (sem clang/llvm-strip) |
| D8/R8 | — | — | **sempre roda** (não é cacheado) |
| AAB | — | — | **sempre roda** (`--aab`/`--aab-only`) |

O fluxo de decisão do build é:

1. `resource_key` bate → flats são copiados do blob para `build/apkfast/compiled_res`;
   se não bater, o `aapt2 compile` roda e o ZIP resultante é gravado.
2. `apk_key(signed=False)` bate → o APK unsigned é copiado do blob para
   `work/apk/unsigned_unaligned.apk`; se não bater, `aapt2 link` +
   `_add_extra_files_with_zip()` rodam e o resultado é gravado.
3. `apk_key(signed=True)` bate → o APK assinado é copiado para
   `work/apk/signed.apk` e **os passos 2, `zipalign` e `apksigner` são pulados**.
   Caso contrário, alinha/assina e grava o novo blob.

Mesmo num hit completo do APK, o build ainda calcula as chaves (lê `res/`,
`assets/`, `.so`, `.dex` e a keystore) e executa as etapas nativas: elas são
necessárias para provar que o artefato em cache continua válido e para o AAB.

## Chaves e invalidadores

`resource_key` muda quando qualquer um destes muda:

- conteúdo/caminho relativo de `res/**` e `assets/**` (inclui `--assets`);
- `AndroidManifest.xml` bruto, `package`, `versionCode`/`versionName`;
- `minSdk`/`targetSdk` (`--min-sdk`, `--target-sdk`);
- versão do aapt2: caminho lógico + saída de `aapt2 --version`;
- namespace/projectId do cache (`--cache-namespace`).

`apk_key` (padrão, assinado) contém tudo isso mais:

- hashes das `.so` nativas e `classes.dex` presentes em `work/apk`;
- lista de ABIs e nome da biblioteca nativa;
- flags de zipalign (`-P 16`) e de assinatura (v1/v2, alias);
- hashes de `zipalign`/`apksigner`, para detectar troca de build-tools;
- **fingerprint da keystore**.

A chave do APK unsigned usa os mesmos dados, exceto assinatura/keystore. Por
isso, trocar apenas a keystore ainda reaproveita o `aapt2 link` e refaz somente
`zipalign`/`apksigner`.

Os caminhos nas chaves são lógicos (`<project>`, `<apkfast>`, `<ndk>`,
`<cache>`, …), então os blobs são portáveis entre checkouts/máquinas com a
mesma toolchain: as chaves nunca contêm o caminho absoluto do projeto.

## Segurança

- A identidade da keystore é calculada por
  `apkfast.signing.signature_fingerprint(keystore)` quando o módulo está
  disponível (na v2.6.0 ele devolve o SHA256 do arquivo da keystore). Se o
  módulo não existir, o builder usa o SHA256 do arquivo diretamente. Nunca é
  servido um APK assinado com fingerprint diferente; o fingerprint também
  aparece (com a origem do cálculo) no `--report`.
- Senhas nunca entram na chave, no blob, no log nem no relatório.
- Todo blob tem seu SHA256 e sua estrutura ZIP validados antes do uso. Em
  `--cache-mode verify` (padrão quando se passa `--cache-dir`),
  `cache.ensure_blob()` ainda impõe o limite de tamanho e reforça a checagem;
  o modo `trusted` confia no conteúdo local, mas não pula o SHA256.
- Locks por `(projectId, ABI)` evitam corrida entre builds compartilhando o
  mesmo cache (inclusive em NFS/sshfs).

## Flags

```sh
# build normal: lê e grava todos os níveis de cache
python3 apkfast.py build . --report

# desliga leitura e gravação (todos os estágios viram miss no report)
python3 apkfast.py build . --no-incremental --report

# apaga somente as entradas de resource/APK deste projeto, além do cache C/C++
python3 apkfast.py build . --clean-cache

# cache compartilhado, com verificação reforçada
python3 apkfast.py build . --cache-dir /mnt/nfs/apkfast --cache-mode verify
```

`--clean-cache` remove apenas os `cache.json` do `projectId` atual e os blobs
que ficaram órfãos; projetos com outro `projectId`/namespace não são afetados.
`--no-incremental` não apaga nada: apenas ignora o cache neste build.

## Lendo o relatório (`--report`)

As chaves antigas (`apkfastVersion`, `toolchain`, `lock`, `config`, ...) seguem
presentes. Foram acrescentadas duas:

```json
{
  "cache": {
    "enabled": true,
    "mode": "trusted",
    "root": "/home/user/.cache/apkfast",
    "namespace": "",
    "hits": 3,
    "misses": 1,
    "keystoreFingerprint": {
      "source": "apkfast.signing.signature_fingerprint",
      "available": true,
      "fingerprint": "3f9a…"
    },
    "events": [
      {"stage": "resource", "result": "hit", "reason": "5 flat(s) reaproveitado(s) do cache", "key": "a1b2c3d4e5f6"},
      {"stage": "native",  "result": "miss", "reason": "objetos: 2 reaproveitado(s), 1 compilado(s) (arm64-v8a)", "key": "…"},
      {"stage": "link",    "result": "hit",  "reason": "APK unsigned reaproveitado do cache", "key": "…"},
      {"stage": "apk",     "result": "miss", "reason": "sem entrada de APK para esta chave", "key": "…"}
    ]
  },
  "timings": {
    "toolchain_ms": 120,
    "resources_ms": 15,
    "native_ms": 3400,
    "package_ms": 700,
    "sign_ms": 950,
    "total_ms": 5200
  }
}
```

`stage` é `native`, `resource`, `link` ou `apk`; `result` é `hit` ou `miss`;
`reason` explica o motivo (por exemplo `incremental desligado (--no-incremental)`,
`blob inválido`, `chave mudou`, `keystore diferente`). `key` é um prefixo curto
da chave, suficiente para correlacionar eventos sem poluir o JSON. Com `-v`, os
misses também aparecem no console com o mesmo motivo.

Os tempos são acumulados por etapa em milissegundos. `package_ms` inclui D8/R8,
`aapt2 link` e a inserção de arquivos extras; `sign_ms` inclui `zipalign`,
`apksigner` e a verificação da assinatura (ou a simples cópia do blob no hit).
`total_ms` mede o build inteiro, incluindo a detecção da toolchain.

## Limitações

- D8/R8 e a geração do AAB não são cacheados (`--aab` continua rodando o
  bundletool e o `aapt2 --proto-format`).
- O cache de APK é por combinação exata de ABIs/entradas; alternar entre
  `--abi arm64-v8a` e `--abi all` produz chaves diferentes.
- Se o `zipalign` instalado não aceitar `-P 16`, o builder cai para alinhamento
  4; o cache ainda é válido, mas o motivo da queda aparece apenas no log.
