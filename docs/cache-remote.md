# Cache remoto/compartilhado — v2.3.0

O cache do apkfast pode viver num diretório compartilhado (NFS, sshfs, disco
comum de CI) e ser transportado como um bundle `.tar.gz`.

## Modos

| Modo | Quando | Comportamento |
|---|---|---|
| `trusted` | cache local padrão | verifica apenas hashes já registrados; rápido |
| `verify` | `--cache-dir` explícito ou import | recomputa hashes dos blobs, valida ELF da ABI e assinatura HMAC se presente |

```sh
python3 apkfast.py build . --cache-dir /mnt/nfs/apkfast --cache-mode verify
```

Se o diretório não permitir escrita, o build continua em modo `verify`
somente-leitura, com aviso.

## Bundle

```sh
# exporta ABIs selecionadas
python3 apkfast.py cache-export . -o cache.tar.gz --abi all \
  --cache-dir /mnt/nfs/apkfast

# importa em outra máquina/checkout
python3 apkfast.py cache-import . cache.tar.gz \
  --cache-dir /mnt/nfs/apkfast
```

Estrutura:

```text
cache.tar.gz
├── manifest.json
└── blobs/<aa>/<sha256>     # .o (LLVM bitcode) e .so (ELF)
```

Validação no import:

- somente `manifest.json` e `blobs/<2 hex>/<64 hex>`
- sem `..`, paths absolutos, symlinks, hardlinks ou dispositivos
- limite de 512 MiB por blob
- hash SHA256 de cada blob confere com o nome
- `projectId`, `cacheSchema` e ABI conferem
- HMAC opcional (`APKFAST_CACHE_HMAC`)

```sh
export APKFAST_CACHE_HMAC='segredo-compartilhado'
python3 apkfast.py cache-export . -o cache.tar.gz
python3 apkfast.py cache-import . cache.tar.gz --require-cache-signature
```

> Hash detecta corrupção, não autenticidade. Sem HMAC, um produtor malicioso
> pode gerar blobs e hashes consistentes. Em CI, use `--require-cache-signature`.

## `projectId`

O identificador é derivado de:

```text
package do manifesto + nativeLibraryName + cacheNamespace
```

Use `cacheNamespace` para isolar variantes (monorepo, release/debug, API).

## Locks

Cada `(projectId, ABI)` tem um lock por diretório:

```text
<cache-root>/.locks/<projectId>/<abi>.lockdir/token
```

`mkdir` é usado como primitiva (funciona em NFSv3/v4 e sshfs). Locks com mais
de 300 s são considerados obsoletos. ABIs são adquiridas em ordem alfabética
para evitar deadlock com `--abi all`.

## Fora de escopo

Não há adaptador HTTP/S3 embutido. O bundle `tar.gz` é transport-agnóstico:
use `scp`, `rsync`, `curl`, `aws s3 cp` ou o que sua infraestrutura já tiver.
