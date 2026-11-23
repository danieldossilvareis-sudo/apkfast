# SVG animado declarativo (`AnimatedSvg`) — v2.4.0 (Equipe A)

`AnimatedSvg` adiciona um driver SMIL **leve** sobre o LunaSVG 3.5 vendored.
Ele lê `<animate>`, `<animateTransform>` e `<set>` do XML cru, aplica os
atributos animados com `Element::setAttribute()`, chama `Document::render()`
de novo e devolve RGBA não-premultiplicado em `current()`.

Requer `python3 apkfast.py lunasvg-install`. Se o SVG **não** tiver SMIL, o
caminho é o mesmo da v2.3.0: `decode_svg_auto()` (LunaSVG para recursos
avançados, NanoSVG para ícones simples) e `update()` não faz nada.

## Exemplo (15 linhas)

```cpp
#include <nativeapp/nativeapp.hpp>

nativeapp::AnimatedSvg svg;
if (!svg.open_asset("pulse.svg")) return;        // SMIL exige LunaSVG
nativeapp::Texture tex;
const auto& first = svg.current();
tex.load_rgba(first.rgba.data(), first.width, first.height);

// dentro de App::on_update(dt_ms):
if (svg.update(dt_ms)) {                        // ms, não segundos
    const auto& frame = svg.current();
    tex.load_rgba(frame.rgba.data(), frame.width, frame.height);
}
// svg.reset();                                 // volta para t=0
```

## API

| Método | Significado |
|---|---|
| `open(bytes,size)` / `open_asset(path)` | carrega; `false` se SMIL sem LunaSVG |
| `is_open()` / `close()` | estado e liberação |
| `width()` / `height()` | dimensões da rasterização |
| `duration_ms()` | fim do último intervalo finito; `0` = estático/infinito |
| `loop_count()` | `0` = infinito, `1` = uma vez (padrão) |
| `animation_count()` | quantos marcadores SMIL foram aplicados |
| `update(dt_ms)` | avança o tempo; `true` se algum atributo mudou e houve re-render |
| `reset()` | volta a `t=0`, reaplica e renderiza |
| `current()` | `const DecodedImage&` com `rgba`, `width`, `height` |
| `set_size(w,h)` | rasterização explícita (`0,0` = natural); pode ser chamado antes de `open()` |

Use `svg_register_font_asset("sans-serif", "fonte.ttf")` antes de `open()` se o
SVG tiver `<text>`; o registro de fontes é o mesmo do decoding estático.

Se houver marcadores SMIL mas **nenhum** for utilizável neste subconjunto,
`open()` avisa no logcat e faz fallback para o SVG estático
(`decode_svg_auto()`); `animation_count()` será `0` e `update()` retornará
`false`. Isso evita quebrar a app por causa de uma animação não suportada.

## SMIL suportado

Atributos lidos no elemento de animação:

| Atributo | Comportamento |
|---|---|
| `attributeName` | atributo do elemento-alvo (ex.: `x`, `opacity`, `fill`, `transform`) |
| `from`, `to` | número(s) com sufixo opcional (`px`, `%`, `deg`); interpolation linear |
| `values` | lista separada por `;`; segmentos de duração igual, linear |
| `dur` | `1.5s`, `500ms` ou número (segundos); obrigatório e finito |
| `begin` | apenas offset simples (`0`, `0.5s`, `250ms`); padrão `0` |
| `repeatCount` | número ou `indefinite`/`infinite`; padrão `1` |
| `fill` | `freeze` mantém o último valor; `remove`/ausente restaura o inicial |
| `type` (somente `animateTransform`) | `translate`, `scale`, `rotate`, `skewX`, `skewY` com listas numéricas |
| `set` (`to`) | valor literal qualquer ( ex.: `visibility="hidden"` ), sem interpolação |

Alvo da animação, em ordem de preferência:

1. `href="#id"` ou `xlink:href="#id"` **interno** no próprio marcador;
2. `id` do ancestral XML mais próximo (ex.: `<g id="roda"><animate .../></g>`);
3. nome do ancestral mais próximo + índice de ocorrência no documento.

`duration_ms()` soma `begin + dur * repeatCount` do último intervalo; se
qualquer animação for infinita, retorna `0`. `loop_count()` é o maior
`repeatCount` arredondado para cima, ou `0` se houver loop infinito.

## Explicitamente NÃO suportado

- `<script>`, qualquer JavaScript/eval, `onclick`/`onbegin` e outros eventos;
- CSS animations/transitions/`@keyframes` (apenas atributos SMIL acima);
- `calcMode` diferente de `linear`, `keyTimes`, `keySplines`, `by`,
  `additive`, `accumulate`, `end`, `min`, `max`, `restart`;
- `begin` com eventos ou sincronismo (`click`, `other.end`, `wallclock`) — a
  animação é ignorada;
- `fill` com valores diferentes de `freeze`/`remove` (tratado como `remove`);
- `animateMotion`, `animateColor`, interpolação de cores/caminhos com
  `<animate>` (somente números; para cor use `<set>` com valor literal);
- `href`/`xlink:href` **externo** ou que não comece com `#`;
- referência a elemento por `id` que o LunaSVG não reconheça (a animação é
  ignorada com aviso, sem quebrar o SVG);
- re-render incremental: cada mudança de atributo rasteriza o documento inteiro
  (limite de 4096 px por lado, igual a `decode_svg_full()`).

SVGs com SMIL e sem LunaSVG retornam `false` em `open()` com aviso no logcat —
não há fallback silencioso que finja animar.

## Integração

Inclua `<nativeapp/animated_svg.hpp>` (a umbrella `nativeapp.hpp` já deve
incluí-lo). Nenhum source ou flag nova do builder é necessária: as
implementações vivem em `image_impl.hpp`, no TU injetado, e o LunaSVG já é
compilado quando instalado.

## Validação manual

```sh
python3 apkfast.py lunasvg-install
python3 apkfast.py build examples/image-demo -o image-demo.apk
python3 apkfast.py install image-demo.apk
python3 apkfast.py logs -s image-demo
```

Crie um SVG de teste em `assets/pulse.svg`:

```xml
<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
  <circle id="bola" cx="32" cy="32" r="8" fill="orange">
    <animate attributeName="r" from="8" to="20" dur="1s"
             repeatCount="indefinite" fill="freeze"/>
  </circle>
</svg>
```

Com `AnimatedSvg`, `animation_count()` deve ser `1`, `loop_count() == 0` e
`current().rgba` deve mudar entre chamadas de `update(250.0f)`. Para conferir o
caminho estático, abra um SVG sem marcadores SMIL: `animation_count() == 0` e
`update()` retorna `false`.
