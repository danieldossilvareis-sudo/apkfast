# Cache de build (v2.7.0)

O apkfast reaproveita o mesmo mecanismo para todas as etapas: raiz de cache
por `projectId`, blobs imutáveis por SHA256, locks por `(projectId, ABI)` e os
modos `trusted`/`verify`. A v2.6.0 cobria C/C++ (`objects`/`link` da `.so`),
recursos e APK; a v2.7.0 acrescenta **DEX (D8/R8)** e **AAB assinado**.

```text
~/.cache/apkfast/                     # ou $XDG_CACHE_HOME/apkfast
├── <projectId>/<abi>/cache.json      # resource, dex, apkLink, apk, aab
└── blobs/<aa>/<sha256>               # flats (ZIP), DEX, APK e AAB
```

## Níveis cacheados

| Estágio | Entrada em `cache.json` | Conteúdo do blob | O que ainda roda no hit |
|---------|-------------------------|------------------|-------------------------|
| `native` | `objects` / `link` | `.o` e `lib*.so` | nada (sem clang/llvm-strip) |
| `resource` | `resource` | ZIP com os `.flat` do `aapt2 compile` | nada (sem `aapt2 compile`) |
| `dex` | `dex` | `classes.dex` de `javac` + D8/R8 | nada (sem `javac`, D8 e R8) |
| `link` | `apkLink` | APK após `aapt2 link` + `.so`/`.dex` extras | nada (sem `aapt2 link`/zipfile) |
| `apk` | `apk` | APK já alinhado e assinado | nada (sem `zipalign`/`apksigner`) |
| `aab` | `aab` | AAB já gerado pelo bundletool e assinado com jarsigner | nada (sem `aapt2 --proto-format`, bundletool, jarsigner) |

`native`, `resource`, `dex`, `link`, `apk` e `aab` são independentes: um miss
em um nível não desfaz os hits dos anteriores. Um hit de `apk` ou de `aab`
também reporta os níveis posteriores como "pulados" (`link` no caso do APK).

### Fluxo de decisão

1. `resource_key` bate → os flats são copiados do blob para
   `build/apkfast/compiled_res`; se não bater, o `aapt2 compile` roda e o ZIP é
   gravado.
2. `dex_key` bate → `work/apk/classes.dex` é copiado do blob; `javac` e D8/R8
   são pulados. Se não bater, `javac` + D8 (ou R8 com `--obfuscate`) rodam e o
   DEX é gravado.
3. As `.so` são produzidas/ reaproveitadas por ABI pelo cache `native`.
4. `apk_key(signed=False)` bate → o APK unsigned é copiado para
   `work/apk/unsigned_unaligned.apk`; se não bater, `aapt2 link` +
   `_add_extra_files_with_zip()` rodam e o resultado é gravado.
5. `apk_key(signed=True)` bate → o APK assinado é copiado para
   `work/apk/signed.apk`; `zipalign`, `apksigner` e a verificação da assinatura
   são pulados. Caso contrário, alinha/assina e grava o blob.
6. `aab_key` bate → o AAB assinado é copiado diretamente para o caminho final
   (`dist/<app>-<suffix>.aab`); `aapt2 link --proto-format`, `bundletool
   build-bundle`, `jarsigner` e `bundletool validate/dump/build-apks` são
   pulados. Se não bater, o fluxo normal de AAB roda e o resultado é gravado.

Mesmo no hit completo, o build continua calculando todas as chaves (lendo
`res/`, `assets/`, fontes Java, `.so`, `.dex` e a keystore) e executando a
etapa nativa: são elas que provam que o artefato em cache ainda é válido.

## Chaves e invalidadores

### Por conteúdo de ABI, não por combinação de ABIs

As bibliotecas nativas entram nas chaves de APK/AAB como um mapa
`<abi> -> [[arquivo, sha256], ...]`, lido de `work/apk/lib/`. A lista de ABIs
da CLI (`--abi X`, `--abis ...`, `--abi all`) **não é concatenada na chave**:
ela apenas seleciona quais entradas do mapa são consideradas. Portanto, uma
ABI isolada tem a mesma chave daquela ABI dentro de um build `--abi all`
quando o conteúdo relevante é idêntico — os blobs são reaproveitados entre os
dois modos. Se o build `all` inclui mais `.so`, o mapa (e a chave) muda, como
esperado para um APK/AAB mais gordo.

### `resource_key`

Muda quando mudam:

- conteúdo/caminho relativo de `res/**` e `assets/**` (inclui `--assets`);
- `AndroidManifest.xml` bruto, `package`, `versionCode`/`versionName`;
- `minSdk`/`targetSdk` (`--min-sdk`, `--target-sdk`);
- versão do aapt2: caminho lógico + saída de `aapt2 --version`;
- namespace/projectId do cache (`--cache-namespace`).

### `dex_key`

Muda quando mudam:

- fontes `src/**/*.java` e `.jar` do projeto (caminho relativo + SHA256);
- `--optimize` (`none`/`release`/`size`), `--obfuscate` e `--min-sdk`;
- versão e identidade de `javac`, `d8`/`r8` (`--version` + SHA256 do binário);
- regras do R8 (`proguard-rules.pro`, quando presentes);
- configuração **Java** do manifesto: classe do application, atividades e
  flags como `debuggable`/`exported`/`process`. Um rótulo de recursos não
  invalida o DEX, pois o manifesto cru já é invalidador do `resource_key`;
- namespace/projectId do cache.

O DEX não depende da ABI: um único blob serve `--abi arm64-v8a` e `--abi all`.

### `apk_key` e `aab_key`

O APK (assinado ou não) contém `resource_key`, o mapa de natives, os hashes
das `.dex` em `work/apk`, o nome da lib, os flags de zipalign/assinatura e — no
caso assinado — o fingerprint da keystore. Trocar apenas a keystore
reaproveita o `aapt2 link` e refaz `zipalign`/`apksigner`.

O AAB assinado contém `resource_key` + `dex_key` + mapa de natives + versão do
bundletool + fingerprint da keystore + flags de assinatura AAB (alias, algoritmo
SHA-256 e identidade do `jarsigner`). **Senhas nunca entram em nenhuma chave.**
Um AAB com keystore diferente simplesmente não bate a chave e nunca é servido.

Os caminhos nas chaves são lógicos (`<project>`, `<apkfast>`, `<ndk>`,
`<cache>`, …), então os blobs são portáveis entre checkouts/máquinas com a
mesma toolchain: as chaves nunca contêm o caminho absoluto do projeto.

## Segurança

- A identidade da keystore é calculada por
  `apkfast.signing.signature_fingerprint(keystore)` quando disponível (na
  prática, o SHA256 do arquivo da keystore); se o módulo não existir, o builder
  usa o SHA256 do arquivo diretamente. O fingerprint (e sua origem) aparece no
  `--report`, nunca a senha.
- Todo blob tem SHA256, tamanho e formato validados antes do uso: ZIP para
  flats/APK/AAB e cabeçalho `dex\n` para o DEX. Em `--cache-mode verify`
  (padrão quando se passa `--cache-dir`), `cache.ensure_blob()` ainda impõe o
  limite de tamanho; o modo `trusted` confia no conteúdo local, mas não pula o
  SHA256.
- Locks por `(projectId, ABI)` evitam corrida entre builds compartilhando o
  mesmo cache (inclusive em NFS/sshfs).
- `cache-push`/`cache-pull` (`cache.export_bundle()`/`import_bundle()`)
  transportam os blobs de todas as entradas, inclusive DEX e AAB. Bundles da
  v2.6.0 (sem essas entradas) continuam sendo importados.

## Flags

```sh
# build normal: lê e grava todos os níveis de cache
python3 apkfast.py build . --report

# AAB assinado também entra no cache
python3 apkfast.py build . --aab --report

# desliga leitura e gravação (todos os estágios viram miss no report)
python3 apkfast.py build . --no-incremental --report

# apaga resource/dex/APK/AAB e o cache C/C++ deste projeto
python3 apkfast.py build . --clean-cache

# cache compartilhado, com verificação reforçada
python3 apkfast.py build . --cache-dir /mnt/nfs/apkfast --cache-mode verify
```

`--clean-cache` remove apenas as entradas do `projectId`/namespace atual (e as
ABIs selecionadas) e os blobs que ficaram órfãos; outros projetos não são
afetados. `--no-incremental` não apaga nada: apenas ignora o cache neste build.

## Lendo o relatório (`--report`)

As chaves antigas (`apkfastVersion`, `toolchain`, `lock`, `config`, ...) seguem
presentes. `cache.events` agora usa `stage` ∈ `native`, `resource`, `dex`,
`link`, `apk`, `aab`:

```json
{
  "cache": {
    "enabled": true,
    "mode": "trusted",
    "root": "/home/user/.cache/apkfast",
    "namespace": "",
    "hits": 5,
    "misses": 1,
    "keystoreFingerprint": {
      "source": "apkfast.signing.signature_fingerprint",
      "available": true,
      "fingerprint": "3f9a…"
    },
    "events": [
      {"stage": "resource", "result": "hit", "reason": "5 flat(s) reaproveitado(s) do cache", "key": "a1b2c3d4e5f6"},
      {"stage": "dex",     "result": "hit", "reason": "DEX reaproveitado do cache (javac/D8/R8 pulados)", "key": "…"},
      {"stage": "native",  "result": "miss", "reason": "objetos: 2 reaproveitado(s), 1 compilado(s) (arm64-v8a)", "key": "…"},
      {"stage": "link",    "result": "hit",  "reason": "APK unsigned reaproveitado do cache", "key": "…"},
      {"stage": "apk",     "result": "hit",  "reason": "APK assinado reaproveitado (keystore igual)", "key": "…"},
      {"stage": "aab",     "result": "miss", "reason": "sem entrada para esta chave (aab)", "key": "…"}
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

`result` é `hit` ou `miss`; `reason` explica o motivo (por exemplo
`incremental desligado (--no-incremental)`, `keystore diferente`, `blob
inválido`). `key` é um prefixo curto da chave, suficiente para correlacionar
eventos sem poluir o JSON. Com `-v`, os misses também aparecem no console.

`package_ms` inclui D8/R8, `aapt2 link`, a inserção de arquivos extras e o
carregamento do AAB em cache; `sign_ms` inclui `zipalign`, `apksigner` e a
verificação da assinatura (ou a cópia do blob no hit).

## O que ainda roda sempre

- A detecção da toolchain, a leitura do `apkfast.json`/lockfile e o cálculo de
  todas as chaves (inclui `--version` de aapt2/javac/d8/r8 e hashing dos
  arquivos de entrada).
- A compilação/verificação do código nativo por ABI (os caches `objects`/`link`
  cobrem os produtos, mas a decisão de cache é por ABI).
- A resolução da keystore (inclusive criação da keystore debug) e o cálculo do
  fingerprint.
- No hit de `apk`, os eventos `resource`/`dex`/`link`/`apk` são emitidos, mas
  nenhuma ferramenta de packaging/assinatura de APK roda.
- No hit de `aab`, a validação local (`bundletool validate`/`dump`/
  `build-apks`) não é repetida; ela roda apenas quando o AAB é gerado.

## Limitações

- O fingerprint da keystore é o SHA256 do **arquivo**, não o fingerprint do
  certificado: qualquer alteração no arquivo invalida APK e AAB.
- Se o `zipalign` instalado não aceitar `-P 16`, o builder cai para alinhamento
  4; a chave não muda e o motivo da queda aparece apenas no log.
- Um hit de DEX pressupõe que as entradas de `javac`/D8/R8 sejam as mesmas do
  blob (versão + SHA256). Ferramentas com o mesmo conteúdo, mas caminhos
  diferentes, usam caminhos lógicos e são consideradas equivalentes.
- O AAB segue cobrindo apenas o módulo `base` (sem dynamic features/asset
  packs) e a verificação local não substitui o upload de teste na Play Store.
- A chave é por conteúdo de ABI; um mesmo projeto com `--abi all` e com uma
  ABI isolada só compartilha o blob quando o conjunto de `.so` é realmente o
  mesmo. Isso é intencional: um fat APK/AAB não é servido como se fosse
  universal de uma ABI só.
