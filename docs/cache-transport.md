# Transporte de cache: `cache-push` / `cache-pull` — v2.4.0 (Equipe A)

Comando novo para mover o **mesmo** bundle `tar.gz` de `cache-export` entre
máquinas, CI e storages. Não há segundo formato: `cache-push` chama
`apkfast.cache.export_bundle()` e `cache-pull` chama
`apkfast.cache.import_bundle()`. Toda a validação (manifest, `projectId`,
schema, hashes SHA256 dos blobs, ELF da ABI e HMAC) continua em `import_bundle`.

> Os subcomandos são registrados por `apkfast.cache_transport.register()`.
> Enquanto a integração em `cli.py` não for feita, as funções `push()`/`pull()`
> já podem ser usadas diretamente de Python.

## Sintaxe

```sh
python3 apkfast.py cache-push <destino> [projeto] [opções]
python3 apkfast.py cache-pull <origem>  [projeto] [opções]
```

`<origem>`/`<destino>` aceitam:

| Esquema | Comando usado | Observações |
|---|---|---|
| path local | `copyfile` + `os.replace` | cópia atômica no destino |
| `file:///...` | idem | só `localhost`/vazio como host; sem query/fragmento |
| `http://` / `https://` | `curl -fL --retry 3` | `PUT` no push, `GET` no pull, `-o <tmp.part>` + `rename` |
| `s3://bucket/chave` | `aws s3 cp` | exige AWS CLI configurado; **ETag nunca é usado** |

Opções compartilhadas: `--abi`, `--abis`, `--cache-dir`, `--cache-mode`,
`--cache-namespace`, `--require-cache-signature`, `--header` (repitível,
apenas HTTP), `--timeout` (padrão 120 s), `-q/--quiet`. O `pull` também tem
`--force` e `--expect-sha256`.

## Exemplos

```sh
# 1) arquivo local / NFS
python3 apkfast.py cache-push /mnt/nfs/apkfast/cache.tar.gz . --abi all
python3 apkfast.py cache-pull file:///mnt/nfs/apkfast/cache.tar.gz . --abi all

# 2) HTTP(S) com token via header (o header não é impresso)
python3 apkfast.py cache-push https://ci.local/cache/cache.tar.gz . \
  --abi arm64-v8a --header "Authorization: Bearer $TOKEN"
python3 apkfast.py cache-pull https://ci.local/cache/cache.tar.gz . \
  --abi arm64-v8a --expect-sha256 $(sha256sum cache.tar.gz | cut -d' ' -f1)

# 3) S3
AWS_PROFILE=ci python3 apkfast.py cache-push s3://meu-bucket/apkfast/cache.tar.gz .
AWS_PROFILE=ci python3 apkfast.py cache-pull s3://meu-bucket/apkfast/cache.tar.gz .

# 4) HMAC obrigatório
export APKFAST_CACHE_HMAC='segredo-compartilhado'
python3 apkfast.py cache-push s3://meu-bucket/apkfast/cache.tar.gz .
python3 apkfast.py cache-pull s3://meu-bucket/apkfast/cache.tar.gz . --require-cache-signature
```

Saída legível (também disponível como `str(result)`):

```text
[✓] cache-push: s3://meu-bucket/apkfast/cache.tar.gz (18 blobs, ABIs: arm64-v8a, 41943040 bytes, sha256=9a3f1c2e7b40d5a1…)
[✓] cache-pull: s3://meu-bucket/apkfast/cache.tar.gz -> cache:/home/user/.cache/apkfast (12 novo(s), 6 já existente(s), 0 rejeitado(s), sha256=9a3f1c2e7b40d5a1…)
```

## API Python

```python
from pathlib import Path
from apkfast import cache, cache_transport

ctx, abis = ...  # ex.: apkfast.cli._cache_transport_context(args)
sent = cache_transport.push(ctx, "s3://bucket/cache.tar.gz", abis=abis)
got = cache_transport.pull(ctx, "s3://bucket/cache.tar.gz", abis=abis,
                           require_signature=True,
                           expect_sha256=sent.sha256)
print(sent.stats["blobs"], got.stats["imported"])
```

`push()`/`pull()` retornam `TransportResult` com `action`, `source`,
`destination`, `scheme`, `bytes`, `sha256` e `stats`. Erros levantam
`TransportError` (ou `apkfast.cache.CacheError`).

## Segurança

- Nada é descompactado fora de `import_bundle()`. O transporte lida apenas com
  bytes opacos do `.tar.gz`; arquivos temporários ficam em diretórios
  `TemporaryDirectory` e são renomeados após o download/cópia completos.
- `curl` usa `-fL --retry 3` e `--proto/--proto-redir =http,https`, evitando
  redirecionamento para `file:`. Credenciais embutidas na URL são rejeitadas;
  prefira `--header` ou variáveis de ambiente.
- ETags do S3 são **metadados opacos** (podem ser multipart, quotes, MD5, etc.)
  e nunca são usados para integridade. Use `--expect-sha256` quando quiser
  conferir o arquivo transportado; a autenticidade real é o HMAC do manifesto.
- Hash sozinho detecta corrupção, não troca maliciosa. Em CI, use
  `APKFAST_CACHE_HMAC` + `--require-cache-signature` no `cache-pull`.
- Locks por `(projectId, ABI)` são adquiridos em torno de export/import, como
  nos comandos `cache-export`/`cache-import`.

## Teste manual (sem servidor)

```sh
# A) prepara cache e origem
python3 apkfast.py build examples/nativeapp-gl -o /tmp/a.apk --cache-dir /tmp/cache-a
python3 apkfast.py cache-export . -o /tmp/cache.tar.gz --cache-dir /tmp/cache-a --abi all

# B) push/pull local (usa as funções se a CLI ainda não estiver ligada)
python3 - <<'PY'
from pathlib import Path
from apkfast import cache, cache_transport
from apkfast.cli import create_parser
args = create_parser().parse_args(["cache-export", ".", "--cache-dir", "/tmp/cache-a"])
# Em integração futura: use cache-push/cache-pull; aqui, montamos o ctx:
from apkfast.cli import _cache_transport_context
ctx, abis = _cache_transport_context(args)
print(cache_transport.push(ctx, "file:///tmp/cache-pushed.tar.gz", abis=abis))
print(cache_transport.pull(ctx, "/tmp/cache-pushed.tar.gz", abis=abis))
PY

# C) HTTP local (se curl existir): suba um servidor que aceite PUT,
#    por exemplo `python3 -m http.server` não aceita PUT — use nginx/Caddy ou
#    um servidor de teste. Confira o retry com `curl -fL --retry 3` manualmente.

# D) HMAC
export APKFAST_CACHE_HMAC='teste'
python3 - <<'PY'
# repita push/pull acima; depois:
from apkfast import cache
# import_bundle sem HMAC definido e --require-cache-signature deve falhar
PY
unset APKFAST_CACHE_HMAC
```

## Limitações

- Não há servidor embutido; HTTP PUT depende do endpoint aceitar `PUT`.
- `file://` com host remoto (UNC/SMB) é recusado de propósito.
- `aws s3 cp` precisa estar no PATH; não há SDK boto3 embutido.
- `--expect-sha256` é conferido antes do import; não substitui HMAC. O
  manifesto contém `createdAt`, então o SHA256 muda a cada `cache-push`: use o
  hash do bundle efetivamente enviado (devolvido em `result.sha256`).
