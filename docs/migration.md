# Guia de migração

## 2.2 → 2.3

- **Cache schema 1 invalidado.** O schema 1 (v2.2.0) foi substituído pelo
  **schema 2**, portável entre checkouts/máquinas. Apague
  `~/.cache/apkfast/` (ou o `--cache-dir` usado) na primeira execução; o
  apkfast 2.3+ não lê blobs do schema 1. Não tente importar bundles antigos:
  `projectId`, `cacheSchema` e os caminhos lógicos mudaram.
- O ciclo de vida da API nativeapp passou a usar `on_surface_created()` /
  `on_surface_destroyed()` para recursos ligados à surface (EGL/swapchain).
  Código que criava esses recursos em `on_ready()` pode apresentar tela preta
  ao voltar de background — mova a inicialização para os novos callbacks.
- Imagens: PNG/JPEG passaram a usar decodificadores embutidos; WebP animado e
  SVG completo exigem `apkfast setup-libs` (libwebp e LunaSVG).
- Comandos novos: `cache-export`, `cache-import`, `webp-install`,
  `lunasvg-install`. O transporte de cache continua sendo `tar.gz` + HMAC
  opcional (`APKFAST_CACHE_HMAC`), sem adaptador HTTP/S3 embutido.

## 2.3 → 2.4

- **Contrato de API versionado.** Inclua `<nativeapp/nativeapp.hpp>` (que agora
  inclui `version.hpp`) e use `APKFAST_API_VERSION` /
  `APKFAST_API_VERSION_STRING` se precisar compilar contra múltiplas versões.
  A API é source-compatible, **não** ABI-compatible: recompile o app.
- **Lockfile reprodutível.** Rode uma vez na máquina/CI de referência:

  ```sh
  python3 apkfast.py build . --generate-lock
  # gera apkfast.lock.json (lockVersion 1)
  ```

  Commite `apkfast.lock.json`. Builds com toolchain diferente avisam; para
  atualizar, gere o lock novamente (ou `--ignore-lock` para ignorar uma vez).
  Veja [compatibility.md](compatibility.md).
- **Config validada.** `apkfast.json` agora passa por validação de chaves e
  tipos. Chaves desconhecidas ou tipos errados falham com mensagem indicando
  a chave e o valor esperado. Use `python3 apkfast.py config show .` para ver
  a configuração efetiva em JSON. Nenhuma chave antiga foi removida.
- **Instalação via PyPI.** `pip install apkfast` instala o CLI `apkfast`; o
  `git clone` + `python3 apkfast.py` continua funcionando. Em instalações via
  wheel, headers/src/patches/docs são instalados junto com o pacote.
- **Comandos novos:** `config show`, `completions {bash,zsh,fish}`,
  `doctor --json`, `build --report`, `build --generate-lock`,
  `build --ignore-lock`.

## Antes de atualizar (checklist rápido)

1. `apkfast --version` → 2.4.0.
2. `apkfast config show .` → sem erros de validação.
3. `apkfast doctor --json` → toolchain coerente com `apkfast.lock.json`.
4. Recompile os 14 exemplos relevantes (a matriz CI faz isso para
   `native-hello`, `image-demo` e `permissions-test`).
5. Se usa cache compartilhado, limpe o cache legado do schema 1 e configure
   HMAC quando o cache for remoto.
