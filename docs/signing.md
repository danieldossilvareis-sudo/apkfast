# Assinatura de release, versionCode e reprodutibilidade

Este documento cobre as primitivas de `apkfast/signing.py` e os pinos de
raylib/SDL usados pelo build. As funções são puras e não gravam segredos em
logs; a integração final com `builder.py`/`cli.py` é feita pelo orquestrador do
build.

## 1. Assinando um release

```sh
# Keystore de upload (NÃO a chave de app signing da Play Store)
export APKFAST_KEYSTORE="$HOME/.apkfast/upload.jks"
export APKFAST_STORE_PASS="$(cat "$HOME/.secrets/apkfast/store.pass")"
export APKFAST_KEY_PASS="$(cat "$HOME/.secrets/apkfast/key.pass")"
export APKFAST_KEY_ALIAS="upload"

apkfast build --release ./meu-jogo
```

Sem `--release`, o apkfast continua gerando/usando a keystore de debug em
`~/.apkfast/debug.jks`. Em modo release, a ausência de keystore falha com erro
que cita apenas `--keystore` / `APKFAST_KEYSTORE` — nunca valores de senha.

### Prioridade das opções

`CLI > variáveis de ambiente > padrões de debug`.

| Opção CLI | Variável de ambiente | Padrão |
| --- | --- | --- |
| `--keystore` | `APKFAST_KEYSTORE` | `<debug>` (modo debug) |
| `--store-pass` | `APKFAST_STORE_PASS` | `android` |
| `--key-pass` | `APKFAST_KEY_PASS` | `android` |
| `--alias` | `APKFAST_KEY_ALIAS` | `androiddebugkey` |

Para que a precedência funcione, as opções de senha/alias no `argparse` devem
usar `default=None`; um default literal (ex.: `"android"`) impede que a
variável de ambiente seja vista. `resolve_keystore_options()` aceita um
`env=` opcional para testes e devolve `KeystoreOptions`; use
`KeystoreOptions.redacted()` em logs (senhas aparecem como `***`).

### Keystore por ambiente

Use a mesma variável com valores diferentes por ambiente (CI/secret manager):

```sh
# staging
APKFAST_KEYSTORE=/secrets/staging/upload.jks apkfast build --release ...
# produção
APKFAST_KEYSTORE=/secrets/prod/upload.jks apkfast build --release ...
```

Nunca reutilize a keystore de debug em produção, nunca commite `.jks`, e
prefira montar a keystore como arquivo somente leitura com permissão `0600`.

## 2. Esquemas de assinatura (v1/v2/v3/v4)

`apksigner_sign_args()` monta a linha de comando com **v1, v2, v3 e v4
habilitados por padrão**:

```text
--v1-signing-enabled true   # JAR signing (compatibilidade com Android < 7)
--v2-signing-enabled true   # APK Signature Scheme v2 (Android 7+)
--v3-signing-enabled true   # v3 com rotação de chave (Android 9+)
--v4-signing-enabled true   # v4 com arquivo .idsig (Android 9+ / instalação rápida)
```

Flags da função para desligar: `v1=False`, `v2=False`, `v3=False`, `v4=False`.
O arquivo `<apk>.idsig` gerado pela v4 deve acompanhar o APK se o fluxo de
instalação o utilizar; a verificação `apksigner verify` continua cobrindo
v1/v2/v3.

Compatibilidade: a função consulta `apksigner --help` quando recebe o caminho
do binário (ou aceita `help_text=`/ `probe=False` para CI hermético). Se um
flag não existir na instalação, ele é omitido, um aviso `SigningWarning` é
emitido e o build segue com os esquemas restantes. Se o `--help` não puder ser
lido, v1/v2 são mantidos e v3/v4 são desativados com aviso.

A linha devolvida contém senhas. Para logged-la use `redact_args(cmd)`; nunca
faça `print(cmd)` nem inclua a linha em exceções.

## 3. Upload key vs. app signing key (Google Play)

- **Upload key**: fica no seu CI/keystore local; assina o AAB enviado à Play.
  É a chave que `APKFAST_KEYSTORE` deve apontar.
- **App signing key**: gerada/guardada pela Google quando você habilita o Play
  App Signing. A Play reassina os APKs finais com ela; você **nunca** deve ter
  essa chave no build.

Se você já habilitou o App Signing, use sempre a upload key. Perder a upload key
não impede publicar (a Play pode redefini-la), mas trocar a chave sem avisar a
Play rejeita o upload seguinte.

### Rotação

1. **Upload key**: no Play Console, *Users and permissions → Release signing* →
   *Request upload key reset*; envie o certificado da nova chave; só então
   substitua `APKFAST_KEYSTORE`/senhas no CI.
2. **App signing key**: é uma migração mais sensível (*Upgrade app signing
   key*), com janela de download do certificado e impacto em todos os apps;
   planeje com antecedência e nunca faça isso junto de uma mudança de
   `versionCode` suspeita.
3. Após qualquer rotação, limpe/recompute a chave de cache do APK assinado:
   `signature_fingerprint()` muda quando o arquivo da keystore muda.

## 4. versionCode automático

`next_version_code(manifest_path, strategy=..., explicit=..., timestamp=...)`
lê `android:versionCode` do `AndroidManifest.xml` e devolve um inteiro:

| Estratégia | Resultado |
| --- | --- |
| `manifest+1` (padrão) | valor do manifesto + 1 |
| `explicit` | usa `explicit=` (inteiro ou texto numérico) |
| `timestamp` | ms desde a epoch truncados para **segundos** |

Limites: Google Play aceita no máximo `2_100_000_000` (`MAX_VERSION_CODE`).
Milissegundos desde 1970 já passam de `1.7e12`, então a estratégia `timestamp`
descarta os 3 dígitos de ms para caber no limite; a resolução é de 1 segundo e
o valor acompanha o relógio do build (use `now_ms=` em testes). Passado o
limite, a função falha em vez de estourar o inteiro — nesse caso migre para
`explicit` ou para um esquema próprio. `timestamp=True` funciona como alias
legado para `strategy="timestamp"`.

## 5. Segurança da keystore

- Guarde a keystore fora do repositório (`~/.apkfast/`, secret manager ou
  volume montado); `chmod 600` no arquivo.
- Prefira senhas vindas de variáveis de ambiente injetadas pelo CI, nunca de
  arquivos versionados nem de argumentos visíveis em listas de processos
  compartilhadas.
- `resolve_keystore_options()` e `apksigner_sign_args()` não imprimem senhas;
  mensagens de erro e avisos citam apenas nomes de opções/variáveis.
- Ao integrar, **também** passe a linha por `redact_args()` antes de
  `log.command()`/mensagens de falha em `builder._execute()` — caso contrário o
  logger genérico vazaria `--ks-pass`/`--key-pass`.
- Faça backup da upload key e das senhas em cofre separado; sem ela você pede
  reset no Play Console e perde a continuidade do pipeline.
- Use uma keystore distinta por ambiente (dev/staging/prod) e nunca assine um
  release com a chave de debug.

## 6. Fontes pinadas (raylib / SDL)

`apkfast/raylib.py` e `apkfast/sdl.py` baixam tarballs com SHA256 verificado e
gravam um marcador `.apkfast-source.json` em `vendor/<lib>/src`. Se o marcador
existir e divergir do pino atual, o build falha com instrução para limpar a
fonte — nunca troca a versão silenciosamente.

| Biblioteca | Versão | Commit da tag | SHA256 do tarball |
| --- | --- | --- | --- |
| raylib | `5.5` | `c1ab645ca298a2801097931d1079b10ff7eb9df8` | `aea98ecf5bc5c5e0b789a76de0083a21a70457050ea4cc2aec7566935f5e258e` |
| SDL2 | `2.32.10` | `5d249570393f7a37e037abf22cd6012a4cc56a71` | `5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165` |
| SDL3 | `3.4.20` | `f00dc919ca35b9756316e86c4777392de442f9b2` | `f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f` |

Overrides: `APKFAST_RAYLIB_VERSION` + `APKFAST_RAYLIB_SHA256`,
`APKFAST_SDL2_VERSION` + `APKFAST_SDL2_SHA256` e `APKFAST_SDL3_VERSION` +
`APKFAST_SDL3_SHA256`. Ao trocar a versão, o SHA256 é obrigatório; divergência
de digest gera erro explícito com valores obtido/esperado. O tarball da raylib
é o archive oficial do GitHub da tag (o release não publica um asset de
código-fonte); os tarballs de SDL são assets oficiais com `digest` publicado.
