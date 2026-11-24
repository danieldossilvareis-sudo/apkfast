# Validação em dispositivo real (CI)

> **Estado desta entrega:** `device-real.yml` e `scripts/device-check.sh` foram
> criados, mas **não foram executados** aqui (sem runner self-hosted, sem projeto
> Firebase configurado). A `Build matrix` existente (`.github/workflows/device.yml`)
> continua intacta: este workflow é aditivo.

Runners GitHub-hosted não bootam emuladores ARM de forma confiável. A matriz
valida compilação/link/empacotamento; este workflow valida **instalação, launch e
logcat** em um dispositivo real (ou no Firebase Test Lab).

## Script local `scripts/device-check.sh`

```sh
sh scripts/device-check.sh --log-file device-check.log \
    dist/native-hello.apk com.example.nativehello
```

Opções:

| Opção | Finalidade |
|---|---|
| `--adb CAMINHO` | força o binário do adb |
| `--serial SERIAL` | escolhe o dispositivo (senão usa o primeiro online) |
| `--wait SEGUNDOS` | espera após o launch (padrão 8) |
| `--lines N` | linhas de logcat (padrão 300) |
| `--log-file CAMINHO` | preserva o logcat (útil como artifact) |
| `--keep-log` | mantém o logcat temporário |

Variáveis: `ADB`, `ADB_SERIAL`, `DEVICE_WAIT`, `DEVICE_LOGCAT_LINES`,
`ANDROID_HOME`, `ANDROID_SDK_ROOT`. Detecção do adb: `--adb`/`ADB` →
`$ANDROID_HOME/platform-tools/adb` → `$ANDROID_SDK_ROOT/platform-tools/adb` →
`~/Android/android-sdk/platform-tools/adb` → `PATH`.

Fluxo: `adb devices` → `logcat -c` → `install -r` → `monkey -p <pkg>` (fallback
`am start`) → espera → `logcat -d -b main -b crash -b system -t N` (fallback sem
`-b` em adb antigo) → falha se `FATAL|dlopen failed|AndroidRuntime.*Exception`.
O resumo imprime APK, package, serial, modo de launch, contagem de linhas/erros e
caminho do log.

Funciona em Termux e Linux (POSIX `sh`, sem bashisms). Testado com um adb falso
para os caminhos de sucesso e de crash.

## Modo 1 — runner self-hosted (KVM ou aparelho)

1. Registre um runner com o label escolhido, por exemplo `device-kvm`:

   ```sh
   mkdir actions-runner && cd actions-runner
   # baixe o pacote do GitHub -> Settings -> Actions -> Runners -> New runner
   ./config.sh --url https://github.com/<owner>/apkfast --token <TOKEN> \
     --labels device-kvm --work _work
   sudo ./svc.sh install && sudo ./svc.sh start
   ```

2. Configure as variáveis do repositório:

   | Nome | Tipo | Exemplo |
   |---|---|---|
   | `DEVICE_RUNNER_LABEL` | variable | `device-kvm` |
   | `DEVICE_SERIAL` | variable (opcional) | `emulator-5554` |
   | `APKFAST_ANDROID_ROOT` | variable (opcional) | `/opt/android-root` |

3. O job `self-hosted` faz checkout, compila `examples/native-hello` com a ABI
   escolhida (ou baixa `apk_url`) e chama `scripts/device-check.sh`. O log é
   enviado como artifact `device-check-self-hosted`.

Requisitos do runner: Linux, `adb`/platform-tools, SDK+NDK detectáveis pelo
apkfast, KVM e uma imagem com launcher, ou um aparelho com depuração USB
autorizada. Use **um label por job**; o campo aceita um único label.
O input `abi` tem padrão `x86_64` (compatível com KVM e com as imagens virtuais
do Test Lab); troque para `arm64-v8a` em um aparelho ARM real.

## Modo 2 — Firebase Test Lab

1. Crie/habilite um projeto Firebase e as APIs **Cloud Testing API** e
   **Cloud Storage**. O plano **Spark (gratuito)** cobre até 10 testes/dia em
   dispositivos virtuais (60 min/dia) e 5 testes/dia em dispositivos físicos
   (30 min/dia), sem exigir cartão. Acima disso, é necessário o plano Blaze com
   faturamento. Atenção: o Firebase Test Lab foi descontinuado e será encerrado
   em **30/09/2027**; o sucessor (Developer Device Platform) exige billing.
2. Crie uma service account com permissão para Test Lab e Storage; copie o JSON.
3. Configure os secrets/vars:

   | Nome | Tipo | Finalidade |
   |---|---|---|
   | `GCLOUD_PROJECT` | secret | habilita este modo (usado em `--project`) |
   | `GCLOUD_SERVICE_ACCOUNT_JSON` | secret | autenticação do `gcloud` |
   | `FIREBASE_RESULTS_BUCKET` | variable (opcional) | bucket para buscar logs e aplicar o grep de crash |

4. O job `firebase-test-lab`:

   ```sh
   gcloud auth activate-service-account --key-file="$key"
   gcloud firebase test android run --type robo \
     --app dist/app.apk \
     --device model=Pixel2,version=28,locale=en,orientation=portrait \
     --project "$GCLOUD_PROJECT" --results-dir "apkfast-$GITHUB_RUN_ID" \
     --timeout 5m
   ```

   Com `FIREBASE_RESULTS_BUCKET`, ele ainda faz
   `gsutil -m cp -r gs://<bucket>/<dir>/* testlab-results/` e falha se o logcat
   baixado contiver os mesmos padrões de crash do script local. Os resultados
   viram artifact `testlab-results`.

Para reusar um APK de outro workflow, informe a URL pública no input `apk_url`
do `workflow_dispatch` (o workflow não baixa artifacts de execuções anteriores).

## Se nada estiver configurado

O job `config` escreve apenas `true/false` nos outputs (nunca imprime segredos).
Sem `DEVICE_RUNNER_LABEL` e sem `GCLOUD_PROJECT`, o job `skip` imprime:

```text
device-real: SKIPPED (sem modo configurado)
Configure vars.DEVICE_RUNNER_LABEL (runner self-hosted) ou secrets.GCLOUD_PROJECT (Firebase Test Lab).
```

e a execução termina com sucesso. Como `secrets` não é utilizável em
`jobs.<job_id>.if`, a checagem do projeto é feita via `env` no job `config`.

## Limitações

- Test Lab robo test cobre launch e fluxos automáticos, não interação humana
  prolongada (toques, IME, permissões em runtime complexas).
- `model=Pixel2,version=28` é um padrão conservador; ajuste no workflow para
  cobrir APIs/ABIs específicas. Cotas gratuitas do Spark: 10 testes/dia
  virtuais (60 min) e 5 testes/dia físicos (30 min). O sucessor do Test Lab
  (Developer Device Platform, a partir de 2027) exige projeto com billing.
- O grep de crash depende de `FIREBASE_RESULTS_BUCKET`; sem ele, o Test Lab roda,
  mas o logcat não é verificado automaticamente.
- O schedule é semanal às 06:00 UTC (`0 6 * * 1`).
- `vars.DEVICE_RUNNER_LABEL` deve ser um único label de runner Linux.
- Nenhum segredo é impresso; tokens do PyPI e do gcloud ficam apenas em `env`.
