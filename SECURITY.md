# Política de segurança

## Versões com suporte

| Versão | Suporte |
|---|---|
| 2.4.x (atual) | Correções de segurança e bugs |
| 2.3.x | Correções críticas de segurança por até 6 meses após 2.4.0 |
| < 2.3 | Sem suporte (o cache schema 1 já foi invalidado em 2.3) |

A política de compatibilidade da API está em [docs/compatibility.md](docs/compatibility.md).

## Como reportar

**Não abra issue pública para vulnerabilidades.** Use um dos canais:

1. GitHub Security Advisories → *Report a vulnerability* no repositório
   (`https://github.com/apkfast/apkfast/security/advisories/new`);
2. ou e-mail privado para os mantenedores (veja o perfil do repositório).

Inclua: versão (`apkfast --version`), `doctor --json` com secrets removidos,
comando executado, impacto estimado e PoC mínima. Resposta esperada em até
7 dias; divulgação coordenada após correção (normalmente 30 dias).

## Cache remoto e HMAC

O bundle de cache (`cache-export`/`cache-import`) contém blobs `.o`/`.so` que
são carregados pelo compilador/linker. Hash SHA256 detecta **corrupção**, não
**autenticidade**: qualquer pessoa que consiga escrever no cache pode gerar
blobs e hashes consistentes.

```sh
export APKFAST_CACHE_HMAC='segredo-compartilhado'
python3 apkfast.py cache-export . -o cache.tar.gz
python3 apkfast.py cache-import . cache.tar.gz --require-cache-signature
```

- Use `--cache-mode verify` e `--require-cache-signature` em CI/cache NFS.
- Nunca publique `APKFAST_CACHE_HMAC` em logs ou no repositório.
- O manifest assinado cobre a lista de blobs; URLs/metadados de transporte
  ficam fora do escopo (o transporte é `scp`/`rsync`/HTTP, escolhido por você).

## Keystore e assinatura

- `apkfast build` usa `~/.apkfast/debug.jks` somente para desenvolvimento.
  O keystore de debug **não é seguro** e não deve ser distribuído.
- Para release, use `--release --keystore /caminho/protegido.jks` e variáveis
  de ambiente/ci secrets para senhas (`--store-pass`, `--key-pass`).
- Não commite `*.jks`, `*.keystore`, `*.p12`, `*.pem` — `.gitignore` já cobre.
- A assinatura é verificada com `apksigner verify --print-certs` ao final de
  todo build.

## Ferramentas baixadas

`setup-sdk`, `setup-libs`, `webp-install` e `lunasvg-install` validam SHA256
das fontes antes de extrair e rejeitam membros de tar com `..`/caminhos absolutos.
Se um checksum mudar, o build falha: atualize o lockfile somente após conferir
a origem da nova versão.
