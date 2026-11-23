# AAB / Android App Bundle

O apkfast v2.5.0 gera, assina e valida um **Android App Bundle** além do APK,
sem Gradle. O AAB é o formato exigido para publicar apps novos na Google Play.

## Uso

```sh
# APK + AAB (mesma keystore, mesmo sufixo de ABI)
python3 apkfast.py build . --aab

# somente AAB
python3 apkfast.py build . --aab-only --abi all

# release com upload key própria e caminho explícito
python3 apkfast.py build . --release --keystore upload.jks \
  --store-pass "senha" --alias upload --key-pass "senha" \
  --aab --aab-output dist/meu-app-upload.aab
```

Flags novas do `build`:

| Flag | Efeito |
|------|--------|
| `--aab` | gera o AAB além do APK (padrão: só APK) |
| `--aab-only` | gera somente o AAB |
| `--aab-output CAMINHO` | caminho do `.aab`; sem ele, usa `--output` trocando a extensão ou `<dist>/<pacote>-<sufixo>.aab` |
| `--bundletool-version X.Y.Z` | versão do bundletool (padrão fixo: `1.18.3`) |

O AAB inclui as mesmas ABIs, dex, assets e bibliotecas nativas do APK. O nome
padrão espelha o APK: `nativehello-arm64-v8a.apk` → `nativehello-arm64-v8a.aab`.

## Keystore: upload key x app signing

O AAB **usa exatamente a mesma keystore do build** (`--keystore`, ou
`~/.apkfast/debug.jks` em builds debug) e é avisado no console. Isso é
importante:

- AAB é assinado com **jarsigner** (v1/JAR). `apksigner` não assina AAB.
- O keystore de debug **não serve para publicar**. Use
  `--release --keystore ... --alias ...`.
- Se você ativou o **Play App Signing**, a chave enviada no AAB deve ser a
  **upload key**, nunca a chave de app signing mantida pelo Google. A Play
  substitui a assinatura do AAB ao gerar os APKs.
- Se você ainda não usa Play App Signing, a chave do AAB é a própria chave de
  app signing: guarde-a com segurança (perdê-la impede atualizações).
- A mesma chave deve ser usada em todas as versões publicadas; trocar a
  upload key exige solicitar redefinição no Play Console.

## O que acontece internamente

1. `aapt2 link --proto-format` cria um APK temporário com manifesto e resource
   table em protobuf (o link normal do APK não é alterado).
2. O módulo `base.zip` é montado com `manifest/AndroidManifest.xml`,
   `resources.pb`, `res/`, `dex/`, `lib/<abi>/` e `assets/`.
3. `bundletool build-bundle --modules=base.zip` gera o AAB.
4. `jarsigner -digestalg SHA-256 -signedjar ...` assina o AAB.
5. `bundletool validate`, `bundletool dump manifest` e
   `bundletool build-apks --mode=universal` validam o resultado. O último
   usa o `aapt2` da toolchain via `--aapt2` (o aapt2 embutido no jar é x86_64).

## bundletool (download e cache)

O jar não é versionado no repositório. Ele fica em
`~/.apkfast/tools/bundletool-all-<versao>.jar` (sobrescreva com
`APKFAST_TOOLS_DIR`). O SHA256 é verificado a cada uso e o download é
atomicamente promovido de `.part` após a checagem.

Versão fixa: **1.18.3**  
Asset: `bundletool-all-1.18.3.jar`  
SHA256: `a099cfa1543f55593bc2ed16a70a7c67fe54b1747bb7301f37fdfd6d91028e29`  
Fonte: https://github.com/google/bundletool/releases/tag/1.18.3

Para outra versão, `--bundletool-version X.Y.Z` busca o digest publicado no
release do GitHub; alternativamente defina `APKFAST_BUNDLETOOL_SHA256` (64
hex) para fixar o checksum. Erros de download/hash abortam o build.

## Verificação manual

```sh
java -jar ~/.apkfast/tools/bundletool-all-1.18.3.jar validate --bundle=dist/app.aab
java -jar ~/.apkfast/tools/bundletool-all-1.18.3.jar dump manifest --bundle=dist/app.aab
java -jar ~/.apkfast/tools/bundletool-all-1.18.3.jar build-apks \
  --bundle=dist/app.aab --output=dist/app.apks --mode=universal \
  --aapt2="$(command -v aapt2)"
jarsigner -verify dist/app.aab
```

O AAB **não pode ser instalado com `adb install`**. Para testar localmente, use
o APK gerado no mesmo build, ou extraia/INSTALE o `universal.apk` do `.apks`:

```sh
java -jar ~/.apkfast/tools/bundletool-all-1.18.3.jar extract-apks \
  --apks=dist/app.apks --output-dir=dist/extracted --device-spec=device-spec.json
adb install-multiple dist/extracted/*.apk
```

## Limitações

- Somente o módulo `base` é gerado: não há dynamic feature modules, Play Asset
  Delivery nem `.obb` (o Play Console rejeita expansion files em AAB).
- AABs não são instaláveis diretamente; o APK continua sendo o artefato de
  teste rápido (e é gerado por padrão junto com o AAB).
- O `aapt2` da toolchain precisa aceitar `--proto-format`. Versões muito antigas
  dos build-tools não suportam; use um aapt2 recente (o erro inclui essa dica).
- `bundletool build-apks` usa o `aapt2` do apkfast por causa da arquitetura
  Termux; em outros hosts o binário embutido no jar normalmente funciona.
- A verificação `--mode=universal` valida a estrutura e a geração de APKs, mas
  não substitui o upload de teste no Play Console (políticas, target API,
  permissões e Play App Signing são verificadas lá).
- `--obfuscate` afeta o APK e o dex incluído no AAB da mesma forma; regras R8
  (`proguard-rules.pro`) são respeitadas nos dois artefatos.
