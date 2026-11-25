# Verificação visual GL × Vulkan (`scripts/visual-parity.sh`)

Esta é uma **prova visual comparável** entre o backend GLES2 e o Vulkan do
`nativeapp`. Ela não substitui os testes estáticos nem o `device-check.sh`:
complementa ambos com uma captura de tela de cada backend para inspeção lado a
lado.

> Escopo: apenas o mecanismo de verificação. Nenhuma API de `vk.hpp`,
> `gl.hpp` ou `font.hpp` é alterada aqui.

---

## 1. Como rodar

Pré-requisitos: SDK/NDK detectáveis pelo apkfast, `adb` no PATH (ou em
`$HOME/Android/android-sdk/platform-tools/adb`) e um dispositivo/emulador
online com a mesma ABI dos APKs.

```sh
# caminho mínimo: usa arm64-v8a e o primeiro dispositivo adb online
sh scripts/visual-parity.sh --out /tmp/visual-parity

# exemplo explícito, igual ao usado na validação local
sh scripts/visual-parity.sh \
    --out /tmp/visual-parity \
    --build-dir /tmp/visual-parity-build \
    --abi arm64-v8a \
    --serial emulator-5554

# inclui image-demo e media-demo como casos adicionais de textura/texto
sh scripts/visual-parity.sh --out /tmp/visual-parity --extras
```

Opções principais:

| Opção | Finalidade |
|---|---|
| `--out CAMINHO` | diretório dos APKs, PNGs, fontes geradas e logs (default: `dist/visual-parity`) |
| `--build-dir CAMINHO` | raiz dos build dirs (default: `<out>/build`) |
| `--abi ABI` | ABI dos dois APKs (default: `arm64-v8a`; `all` é rejeitado) |
| `--adb CAMINHO` | força o binário do adb |
| `--serial SERIAL` | escolhe o dispositivo (default: `ADB_SERIAL` ou primeiro online) |
| `--wait SEGUNDOS` | espera após launch/captura (default: 4) |
| `--lines N` | linhas de logcat por checagem (default: 200) |
| `--extras` | também builda e valida `image-demo` e `media-demo` |

Variáveis aceitas: `ADB`, `ADB_SERIAL`, `APKFAST_PARITY_ABI`,
`VISUAL_PARITY_WAIT`, `VISUAL_PARITY_LOG_LINES`, `ANDROID_HOME`,
`ANDROID_SDK_ROOT`.

O script falha (exit != 0) se: não houver dispositivo online, a ABI do
dispositivo divergir da ABI pedida, algum build falhar, algum
`device-check.sh` acusar `FATAL|dlopen failed|AndroidRuntime.*Exception`, o app
não estiver em foco na captura, a captura não for um PNG válido ou o logcat
logo após a captura contiver os mesmos padrões de crash.

---

## 2. O que o script faz

1. Gera `<out>/src/gl_parity_main.cpp`: uma cena `GLES2Scene` com **os mesmos
   parâmetros visuais** do exemplo Vulkan (clear color, triângulo, sprite e
   texto). O arquivo é gerado no `--out`, não é versionado.
2. Builda `examples/nativeapp-gl` com esse arquivo via `--src` e com os assets
   de `examples/nativeapp-vulkan/assets` via `--assets`. Assim o APK GL usa
   exatamente o mesmo `sprite.png` e `font.ttf` do APK Vulkan. O
   `jni/main.cpp` versionado de `nativeapp-gl` permanece intacto.
3. Builda `examples/nativeapp-vulkan` normalmente (`jni/main.cpp` versionado).
4. Instala cada APK e chama `scripts/device-check.sh` com `--adb`, `--serial`,
   `--wait`, `--lines` e `--log-file`.
5. Força o foco de cada package, captura com `adb exec-out screencap -p` e
   valida a assinatura PNG (`89504e470d0a1a0a`) e o logcat pós-captura.
6. Imprime packages, ABIs, device, resolução, caminhos dos APKs/PNGs e dos
   logs.

Saídas em `<out>`:

```text
gl.apk                    vk.apk
gl.png                    vk.png
gl-device-check.log       vk-device-check.log
gl-screenshot.logcat      vk-screenshot.logcat
launch-gl.log             launch-vk.log
install-gl.log            install-vk.log
src/gl_parity_main.cpp
build/gl/                 build/vk/
```

Packages usados:

| Rótulo | Package |
|---|---|
| `gl` | `com.apkfast.nativeappgl` |
| `vk` | `com.apkfast.nativeappvk` |
| `image-demo` (opcional) | `com.apkfast.imagedemo` |
| `media-demo` (opcional) | `com.apkfast.mediademo` |

---

## 3. Cena de referência (o que observar)

Os dois APKs desenham, na mesma ordem:

1. clear color `(0.10, 0.20, 0.35, 1.0)` — esperado `(26, 51, 89)` em RGB;
2. `MeshData::triangle()` com `Material` branco, transladado para
   `(0.50*w, 0.35*h)` e escalado por `0.45*min(w,h)`;
3. `MeshData::quad(180, 180)` com `sprite.png`, transladado para
   `(0.72*w, 0.55*h)`;
4. `draw_text("VULKAN OK", 24, 48, 1.4, 1.0, 0.95, 0.65, 1.0)`.

A ordem de leitura recomendada é: fundo → texto → triângulo → sprite. Abra
`gl.png` e `vk.png` lado a lado (qualquer visualizador de imagem) e compare
posição, tamanho, orientação e cor.

### Diferenças observadas na execução local (emulator-5554, arm64-v8a, 1080×2340)

**Estado final:** sem diferenças visuais entre `gl.png` e `vk.png` para a cena
de referência. Caixas medidas em pixels nos dois PNGs:

- **fundo**: `(26, 51, 89, 255)` nos dois;
- **texto** `VULKAN OK`: `x=34..228, y=42..67` nos dois;
- **triângulo**: `x=297..782, y=576..1060` nos dois;
- **sprite**: `x=416..867, y=624..1376` (180×180) nos dois.

Comparação pixel a pixel: diferença máxima de **1/255 por canal** (arredondamento
de composição), nenhuma divergência visual. `device-check` nos dois APKs não
acusa `FATAL|dlopen failed|AndroidRuntime.*Exception`.

**Histórico (já corrigido no backend Vulkan, fora deste script):** o sprite
aparecia como faixa de ~180×47 px por uso de `rowPitch` de imagem
`TILING_OPTIMAL` no staging de `vkCmdCopyBufferToImage` com
`bufferRowLength = 0`; triângulo e texto ficavam espelhados porque
`vk_detail::ortho2d_screen()` reutilizava a matriz do GL no NDC do Vulkan. A
correção está em `include/nativeapp/vk.hpp` (linhas contíguas e espelhamento no
upload, `mat4_ortho(0, w, 0, h, -1, 1)`, baseline sem `height - y`). Re rode o
script se `vk.hpp`, `gl.hpp`, `font.hpp` ou os assets mudarem; este documento
registra o mecanismo e a última verificação, não substitui a captura atual.

---

## 4. Limitações do método

- **Screenshot ≠ olho humano**: o `screencap` congela um frame. Ele não mostra
  flicker, latência, tearing, taxa de quadros, nem suavidade de animação.
- **Não é pixel-diff**: o script valida assinatura PNG e ausência de FATAL, mas
  não calcula diferença por pixel. Duas imagens podem ser "parecidas" para uma
  pessoa e diferentes em dezenas de pixels por causa de filtragem,
  rasterização de fonte, compositor e compressão.
- **Estado do dispositivo importa**: brilho, modo escuro, rotação, barra de
  navegação, notificações e escala de densidade afetam ambas as capturas. Use
  o mesmo dispositivo e a mesma resolução para as duas capturas (o script já
  garante isso ao falhar se a ABI divergir e ao imprimir a resolução).
- **Timing**: `draw_text` e texturas são capturados após `--wait` segundos;
  animações em `image-demo`/`media-demo` podem estar em frames diferentes.
  Por isso as PNGs oficiais da paridade vêm de cenas estáticas.
- **Assets por APK**: o script resolve isso passando `--assets` do Vulkan para
  o build GL; se você mudar `sprite.png`/`font.ttf`, rode o script de novo.
- **Cenas versionadas diferentes**: o `jni/main.cpp` original de
  `nativeapp-gl` é o exemplo mínimo legado; a cena comparável é a gerada no
  `--out`. Não compare `gl.png` com uma captura feita à mão do exemplo
  versionado sem usar o mesmo `--src`.
- **Falsos positivos de logcat**: o filtro de crash é o mesmo do
  `device-check.sh`. Mensagens de sistema com a palavra `FATAL` podem fazer o
  script falhar; nesse caso inspecione o log em `<out>/*-screenshot.logcat`.

---

## 5. Validação rápida

```sh
sh -n scripts/visual-parity.sh
sh scripts/visual-parity.sh --out /tmp/visual-parity
python3 -m unittest discover -s tests -q
```

A suíte `tests/test_visual_parity.py` é estática/estrutural: verifica presença
do script, dos packages, dos nomes `gl.png`/`vk.png`, do conteúdo visual
comum e da documentação. Ela **não exige GPU, SDK nem adb**.
