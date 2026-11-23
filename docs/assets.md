# Assets (`nativeapp::Assets` / `assets.hpp`)

> Guia completo do helper de leitura de arquivos empacotados em `assets/`.
>
> **Fontes consultadas:** `include/nativeapp/assets.hpp` (API e implementação),
> `include/nativeapp/app.hpp` (injeção do `AAssetManager` em `activate_surface()`),
> `include/nativeapp/gl.hpp` (`Shader::load`, `Texture::load`),
> `apkfast/builder.py` e `apkfast/cli.py` (empacotamento dos assets no APK),
> `apkfast/scaffold.py` (diretório criado em projetos novos).
>
> **Regra deste documento:** só está aqui o que existe no código atual. Quando o
> código não faz algo — ou faz algo diferente do que o nome sugere — isso é dito
> explicitamente (veja especialmente §5 e §10).

## Índice

1. [Visão geral](#1-visão-geral)
2. [Singleton e injeção do `AAssetManager`](#2-singleton-e-injeção-do-aassetmanager)
3. [Tabela completa da API](#3-tabela-completa-da-api)
4. [`exists()` — testar presença sem ler](#4-exists--testar-presença-sem-ler)
5. [`read()` — bytes, tamanho e leitura curta](#5-read--bytes-tamanho-e-leitura-curta)
6. [`read_text()` — conteúdo como `std::string`](#6-read_text--conteúdo-como-stdstring)
7. [`manager()` — acesso direto ao gerenciador](#7-manager--acesso-direto-ao-gerenciador)
8. [`write()` — stub de gravação](#8-write--stub-de-gravação)
9. [Exemplos com `Shader::load` e `Texture::load`](#9-exemplos-com-shaderload-e-textureload)
10. [Como colocar arquivos em `assets/` no projeto](#10-como-colocar-arquivos-em-assets-no-projeto)
11. [Armadilhas e diagnóstico](#11-armadilhas-e-diagnóstico)
12. [Resumo rápido](#12-resumo-rápido)

---

## 1. Visão geral

`nativeapp::Assets` é uma classe pequena: ela guarda um ponteiro
`AAssetManager*`, oferece leitura de arquivos via API Android
`AAssetManager_open` / `AAsset_read` no modo `AASSET_MODE_BUFFER`, e mais nada.
Não há cache de arquivos, ZIP próprio, decodificação, lista de diretórios nem
escrita.

```
     App::activate_surface()
                │  Assets::instance(activity).set_activity(activity)
                ▼
     ┌──────────────────────────────────────┐
     │ Assets (singleton)                   │
     │   manager_ : AAssetManager*          │
     ├──────────┬──────────┬────────────────┤
     │ exists() │  read()  │ read_text()    │   write() → false
     └────┬─────┴────┬─────┴───────┬────────┘
          │          │             │
          ▼          ▼             ▼
  AAssetManager_open(..., AASSET_MODE_BUFFER)
          │
          ▼
  AAsset_getLength64 → resize → AAsset_read → AAsset_close
```

Todo caminho passado a `exists()`, `read()` ou `read_text()` é **relativo à raiz
do diretório `assets/`** do projeto (que o build empacota dentro do APK). O
código não adiciona nem remove prefixos: o que você passa é exatamente o que vai
para `AAssetManager_open`.

---

## 2. Singleton e injeção do `AAssetManager`

### 2.1 `instance()`

```cpp
static Assets& instance(ANativeActivity* activity) {
    // Inicialização local static é thread-safe em C++11+ e o objeto é
    // destruído no shutdown (o ponteiro new vazava para sempre).
    // Como antes, o activity só é usado na primeira chamada.
    static Assets singleton(activity);
    return singleton;
}
```

Comportamento literal do código:

| Aspecto | O que acontece |
|---|---|
| Primeira chamada | Constrói `Assets(activity)` uma única vez como **objeto local static** (sem `new`). |
| Thread safety | A inicialização de um local static é **thread-safe em C++11+**; não há corrida na criação. |
| Construtor | `manager_(activity ? activity->assetManager : nullptr)`. |
| Chamadas seguintes | **Ignoram o argumento** e devolvem sempre o mesmo objeto. |
| `activity == nullptr` na primeira chamada | Cria o singleton com `manager_ == nullptr`. |
| Destruição | O objeto é destruído automaticamente no shutdown do processo (não há vazamento). |

Consequência importante: chamar `Assets::instance(nullptr)` é seguro **desde que
o singleton já tenha sido criado antes** com um `activity` válido (ou com
`set_activity()` já feito). É exatamente esse o caso de `Shader::load()` e
`Texture::load()`, que usam `Assets::instance(nullptr)`.

### 2.2 `set_activity()`

```cpp
void set_activity(ANativeActivity* activity) {
    if (activity) manager_ = activity->assetManager;
}
```

* Atualiza `manager_` com `activity->assetManager`.
* **Ignora `nullptr`**: se você chamar `set_activity(nullptr)`, o gerenciador
  anterior permanece intacto (não é limpo).
* Não verifica se o `assetManager` dentro do activity é nulo — ele é copiado
  direto para `manager_`; a proteção contra nulo está nos métodos de leitura.

### 2.3 Onde a injeção acontece

Em `App::activate_surface()`, antes de `on_ready()`:

```cpp
// include/nativeapp/app.hpp
if (!initialized_) {
    initialized_ = true;
    // Injeta o AAssetManager antes de qualquer cena carregar assets.
    Assets::instance(app_->activity).set_activity(app_->activity);
    // ... TextInput ...
    on_ready();
    state_ = Lifecycle::Ready;
}
```

Ou seja: **dentro de `on_ready()` o gerenciador já está disponível**, e o
código da sua aplicação não precisa repetir a injeção. Repetir é inofensivo —
apenas reatribui o mesmo ponteiro.

`VulkanScene::init(ANativeActivity*)` também faz a injeção por conta própria:

```cpp
void init(ANativeActivity* activity) {
    if (initialized_) return;
    activity_ = activity;
    Assets::instance(activity).set_activity(activity);
    create_instance();
    pick_physical_device();
    create_device();
    initialized_ = true;
}
```

> **Armadilha clássica:** chamar `Shader::load()` no construtor da sua cena,
> antes de `on_ready()`/`init()`, pode encontrar `manager_ == nullptr`. Veja
> a proteção recomendada em §9.

---

## 3. Tabela completa da API

| Método | Assinatura | Retorno | Falha / caso especial |
|---|---|---|---|
| `instance` | `static Assets& instance(ANativeActivity*)` | Referência ao singleton | Argumento ignorado após a primeira chamada |
| `set_activity` | `void set_activity(ANativeActivity*)` | — | `nullptr` é ignorado; `manager_` não é alterado |
| `exists` | `bool exists(const std::string&) const` | `true` se o arquivo abre | `false` sem manager ou se `AAssetManager_open` falha |
| `read` | `std::vector<uint8_t> read(const std::string&) const` | Bytes do arquivo | Vetor vazio sem manager, se `open` falha, se tamanho < 0 ou `> SIZE_MAX`, se arquivo é vazio ou se alguma leitura retorna ≤ 0 (laço com `clear()`) |
| `read_text` | `std::string read_text(const std::string&) const` | Conteúdo textual | String vazia se `read()` não trouxe bytes |
| `write` | `bool write(const std::string&, const std::vector<uint8_t>&) const` | **Sempre `false`** | Stub: assets são somente leitura |
| `manager` | `AAssetManager* manager() const` | Ponteiro bruto, pode ser nulo | — |

Todos os métodos de leitura são `const`. Não há sobrecargas com tamanho
máximo, offset, streaming, nem versões que retornem `bool` + saída.

---

## 4. `exists()` — testar presença sem ler

```cpp
bool exists(const std::string& path) const {
    if (!manager_) return false;
    AAsset* asset = AAssetManager_open(manager_, path.c_str(), AASSET_MODE_BUFFER);
    if (!asset) return false;
    AAsset_close(asset);
    return true;
}
```

Pontos que valem registrar:

* É um **teste de abertura**, não uma consulta de metadados: o arquivo é aberto
  em `AASSET_MODE_BUFFER` e imediatamente fechado com `AAsset_close`.
* Com `manager_ == nullptr`, retorna `false` sem tentar abrir nada — não há
  exceção, log ou aviso.
* Um arquivo existente mas vazio continua retornando `true` (a decisão depende
  só do sucesso de `AAssetManager_open`).
* Como o caminho é repassado direto, `exists("assets/foo.png")` testa um
  caminho **dentro** de `assets/` chamado `assets/foo.png`, não o arquivo
  `foo.png` na raiz.

Uso típico: escolher um fallback antes de carregar algo obrigatório.

```cpp
auto& assets = Assets::instance(nullptr);
const std::string caminho =
    assets.exists("shaders/custom.frag") ? "shaders/custom.frag"
                                         : "shaders/basic.frag";
```

> Não confunda `exists()` com garantia de leitura: entre o teste e o `read()`
> nada é travado; o `read()` tem suas próprias verificações e pode devolver
> vetor vazio.

---

## 5. `read()` — bytes, tamanho e leitura curta

Implementação completa:

```cpp
std::vector<uint8_t> read(const std::string& path) const {
    std::vector<uint8_t> result;
    if (!manager_) return result;
    AAsset* asset = AAssetManager_open(manager_, path.c_str(), AASSET_MODE_BUFFER);
    if (!asset) return result;
    const int64_t length = AAsset_getLength64(asset);
    if (length < 0 ||
        static_cast<uint64_t>(length) >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        AAsset_close(asset);
        return result;
    }
    const size_t size = static_cast<size_t>(length);
    if (size == 0) {
        AAsset_close(asset);
        return result;
    }
    result.resize(size);
    // AAsset_read pode devolver menos bytes que o pedido; completa o
    // buffer em vez de devolver um arquivo truncado. Qualquer falha
    // ainda resulta em vetor vazio (comportamento histórico).
    size_t total = 0;
    while (total < size) {
        const int bytes = AAsset_read(asset, result.data() + total,
                                      size - total);
        if (bytes <= 0) {
            result.clear();
            break;
        }
        total += static_cast<size_t>(bytes);
    }
    AAsset_close(asset);
    return result;
}
```

Fluxo passo a passo:

| Etapa | Código | Efeito |
|---|---|---|
| 1 | `if (!manager_) return result;` | Sem gerenciador, devolve vetor vazio imediatamente. |
| 2 | `AAssetManager_open(..., AASSET_MODE_BUFFER)` | Abre o arquivo; falha ⇒ vetor vazio. |
| 3 | `AAsset_getLength64(asset)` + checagem | Tamanho em `int64_t`; negativo **ou maior que `SIZE_MAX`** (comparado como `uint64_t`, para evitar wrap) ⇒ fecha e devolve vazio. |
| 4 | `if (size == 0)` | Fecha e devolve vetor vazio sem chamar `AAsset_read`. |
| 5 | `result.resize(size)` | Aloca exatamente o tamanho informado. |
| 6 | `while (total < size)` | **Laço de leitura**: `AAsset_read(asset, result.data() + total, size - total)` completa o buffer após leituras curtas. |
| 7 | `bytes <= 0` | Descarta tudo (`result.clear()`) e sai do laço — comportamento histórico de falha. |
| 8 | `AAsset_close(asset)` | Sempre fecha quando o arquivo foi aberto. |

Sobre a **leitura curta**: o código não assume que `AAsset_read` devolve todos
os bytes pedidos. Ele usa o retorno `int bytes`, avança `total` e pede o
restante no próximo ciclo do laço, gravando em `result.data() + total`. O
resultado, quando não há falha, tem **exatamente** `size` bytes — não é
redimensionado para a primeira leitura. O modo `AASSET_MODE_BUFFER` é o usado na
abertura; isso não elimina a possibilidade de retornos parciais.

Sobre o **arquivo vazio**: quando `size == 0`, `AAsset_read` nem é chamado e o
vetor já retorna vazio. Na prática, `read()` de um arquivo vazio e `read()` de
um arquivo inexistente produzem o **mesmo valor** (`std::vector<uint8_t>{}`):
a API não tem como distinguir os dois casos. O mesmo vale para `read_text()`.

Exemplo:

```cpp
const auto bytes = assets.read("dados/paleta.bin");
if (bytes.empty()) {
    // Pode ser: sem manager, arquivo ausente, tamanho inválido, leitura falha
    // ou arquivo vazio. Use exists() antes se precisar separar as causas.
} else {
    const size_t n = bytes.size();  // tamanho efetivamente lido
}
```

Não há limite de tamanho, verificação de `size` máximo nem conversão de
endianness no helper.

---

## 6. `read_text()` — conteúdo como `std::string`

```cpp
std::string read_text(const std::string& path) const {
    const auto data = read(path);
    if (data.empty()) return {};
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}
```

* É um **wrapper direto** sobre `read()`: mesma abertura, mesmo
  `AAsset_getLength64`, mesma leitura curta, mesmos casos de falha.
* Constrói a string com `(ponteiro, tamanho)`, então bytes `0` dentro do arquivo
  são preservados — não há parada em NUL.
* Não há conversão de encoding, remoção de BOM, tratamento de quebras de linha
  nem validação de UTF-8.
* Arquivo vazio ou falha ⇒ `std::string{}`.

É exatamente o que `Shader::load()` usa para obter o código-fonte GLSL dos
arquivos de shader.

---

## 7. `manager()` — acesso direto ao gerenciador

```cpp
AAssetManager* manager() const { return manager_; }
```

Devolve o ponteiro bruto armazenado, que pode ser `nullptr`. O uso mais comum
é como **guarda antes de chamar APIs que não toleram ausência de manager**,
como `Shader::load()`:

```cpp
auto& assets = Assets::instance(nullptr);
if (!assets.manager()) {
    // Ainda não houve injeção de activity; não carregue shaders/texturas.
    return;
}
```

O ponteiro volta a ser válido assim que `set_activity()` recebe um activity
não nulo.

---

## 8. `write()` — stub de gravação

```cpp
bool write(const std::string&, const std::vector<uint8_t>&) const {
    return false;  // assets são somente leitura
}
```

* Os parâmetros **não têm nome** na implementação: não são usados.
* Sempre retorna `false`, independentemente do caminho ou do conteúdo.
* Não cria arquivo, não abre nada, não registra log.
* Não existe contrapartida de "remover" ou "listar" no header.

Se precisar gravar em armazenamento do app, use as APIs de arquivo do Android;
este helper não faz isso.

---

## 9. Exemplos com `Shader::load` e `Texture::load`

### 9.1 `Shader::load()` (GLES)

Trecho real de `include/nativeapp/gl.hpp`:

```cpp
/// Carrega de assets/ (caminhos relativos ao diretório assets).
void load(const std::string& vertex_path, const std::string& fragment_path) {
    auto& assets = Assets::instance(nullptr);
    if (!assets.manager()) {
        throw GLError("AAssetManager indisponível");
    }
    compile(assets.read_text(vertex_path), assets.read_text(fragment_path));
}
```

Pontos importantes:

* Usa `Assets::instance(nullptr)` — funciona porque o singleton já foi criado
  e injetado por `App::activate_surface()` (ou por `VulkanScene::init()`).
* Se não houver manager, **lança `GLError`** (não retorna `bool`).
* Se um arquivo estiver vazio/faltando, `read_text()` devolve string vazia e o
  erro aparece na compilação GLSL, não na leitura.

Exemplo dentro de `on_ready()` (quando a injeção do `App` já ocorreu):

```cpp
#include <nativeapp/app.hpp>
#include <nativeapp/gl.hpp>

class MeuApp : public nativeapp::App {
protected:
    void on_ready() override {
        // O App já injetou o AAssetManager aqui.
        auto& assets = nativeapp::Assets::instance(activity());

        if (!assets.manager()) {
            // Situação anômala: sem manager, Shader::load() lança GLError.
            return;
        }

        try {
            shader_.load("shaders/sprite.vert", "shaders/sprite.frag");
        } catch (const nativeapp::GLError& e) {
            // Trate falha de compilação/linkagem ou manager indisponível.
            (void)e;
        }
    }

private:
    nativeapp::Shader shader_;
};
```

### 9.2 `Texture::load()` (GLES)

Trecho real de `include/nativeapp/gl.hpp`:

```cpp
bool load(const std::string& path) {
    return load_with_size(path, 0, 0);
    // decode_asset_image() detecta PNG/JPEG (stb), WebP (Android) ou SVG (NanoSVG)
    // e faz upload RGBA → GL_RGBA.
}
```

* Retorna `false` se não há manager **ou** se `read()` não trouxe bytes **ou**
  se o decodificador do formato detectado falhar.
* WebP e SVG são suportados desde a v2.2.0; veja
  [image-formats.md](image-formats.md).
* O caminho é relativo à raiz de `assets/` (sem prefixo `assets/`).
* O `Material::texture` de `graphics_types.hpp` é justamente o nome de arquivo
  passado para `Texture::load()` pelo `GLES2Scene::draw_mesh()`.

Exemplo:

```cpp
nativeapp::Texture textura;
if (textura.load("textures/grade.png")) {
    textura.bind(0);                       // unidade de textura 0
    const int w = textura.width();
    const int h = textura.height();
    (void)w; (void)h;
} else {
    // manager nulo, arquivo vazio/ausente ou decodificação falhou.
}

nativeapp::Material mat;
mat.texture = "textures/grade.png";        // nome relativo a assets/
```

### 9.3 Leitura direta de dados arbitrários

```cpp
auto& assets = nativeapp::Assets::instance(activity());

if (assets.exists("dados/config.json")) {
    const std::string json = assets.read_text("dados/config.json");
    // Processe o texto; strings vazias indicam falha/arquivo vazio.
}

const auto blob = assets.read("dados/tabela.bin");
if (!blob.empty()) {
    // blob.data() / blob.size() são o conteúdo efetivamente lido.
}
```

---

## 10. Como colocar arquivos em `assets/` no projeto

O que o código do apkfast faz:

| Arquivo do toolchain | Comportamento |
|---|---|
| `apkfast/scaffold.py` | `create_project()` cria `<projeto>/assets` (junto de `jni/`, `src/main/cpp/`). |
| `apkfast/builder.py` | Se `<projeto>/assets` é diretório, passa `-A <caminho>` ao `aapt2 link`. |
| `apkfast/cli.py` | Flag `--assets <dir>` substitui o padrão `<projeto>/assets`. |

Na prática:

```
MeuProjeto/
├── AndroidManifest.xml
├── apkfast.json
├── jni/
│   └── main.cpp
├── assets/                    ← raiz: o que o AAssetManager enxerga
│   ├── shaders/
│   │   ├── sprite.vert
│   │   └── sprite.frag
│   ├── textures/
│   │   └── grade.png
│   └── dados/
│       └── config.json
└── src/main/cpp/
```

Mapeamento entre arquivo no disco e caminho no código:

| Arquivo no projeto | Caminho em `read()` / `load()` |
|---|---|
| `assets/shaders/sprite.vert` | `"shaders/sprite.vert"` |
| `assets/textures/grade.png` | `"textures/grade.png"` |
| `assets/dados/config.json` | `"dados/config.json"` |
| `assets/arquivo.txt` | `"arquivo.txt"` |

Depois de adicionar ou alterar arquivos, reconstrua o APK para que o `aapt2`
reempacote o diretório — o app lê o que está dentro do APK, não o diretório do
projeto em tempo de execução.

Se o projeto usa `--assets caminho/outro`, a raiz passa a ser esse outro
diretório; o código C++ continua usando caminhos relativos a essa raiz.

---

## 11. Armadilhas e diagnóstico

| Armadilha | Sintoma | Causa no código / como evitar |
|---|---|---|
| `AAssetManager` nulo fora do `App`/antes da injeção | `Shader::load()` lança `GLError("AAssetManager indisponível")`; `Texture::load()` retorna `false`; `read()`/`exists()` retornam vazio/`false` | O manager vem de `App::activate_surface()` ou `VulkanScene::init()`. Chame essas APIs depois de `on_ready()`/`init()`, ou garanta `set_activity()` antes. |
| Primeira chamada foi `Assets::instance(nullptr)` | `manager_` fica nulo para sempre até um `set_activity()` explícito | `instance()` só usa o argumento na criação; depois disso o argumento é ignorado. Use `instance(activity)` na primeira chamada ou chame `set_activity(activity)`. |
| `set_activity(nullptr)` para "limpar" | Manager continua válido | O método tem `if (activity)`; `nullptr` é ignorado. |
| Caminho com prefixo `assets/` | `exists()` retorna `false`, `read()` devolve vazio | O caminho já é relativo à raiz do diretório passado ao `aapt2` com `-A`. Use `"textures/x.png"`, não `"assets/textures/x.png"`. |
| Esperar que `write()` salve algo | Sempre `false`, nenhum arquivo criado | É um stub explícito: `// assets são somente leitura`. |
| Arquivo vazio vs. arquivo ausente | Ambos dão vetor vazio / string vazia | `read()` não distingue as situações; use `exists()` antes se precisar diferenciar. |
| Confiar que `AAsset_read` devolve tudo de uma vez | (Antes: arquivo truncado; agora:) o laço completa o buffer | `read()` repete a leitura em `result.data() + total` até `total == size`; use sempre `result.size()` de todo modo. |
| Ler `bytes <= 0` em qualquer iteração | Vetor inteiro descartado (`clear()`) | Qualquer retorno não positivo de `AAsset_read` zera o resultado, mesmo após leituras parciais anteriores. |
| `AAsset_getLength64()` maior que `SIZE_MAX` | Vetor vazio, sem alocação | A checagem compara como `uint64_t` contra `numeric_limits<size_t>::max()` (importante em builds 32-bit). |
| Usar `read_text()` em binário com encoding desconhecido | Texto com bytes estranhos | Não há conversão de encoding nem validação; é só `reinterpret_cast` + tamanho. |
| `exists()` usado como verificação de integridade | Arquivos existentes podem falhar na leitura | `exists()` só verifica abertura; `read()` tem verificações próprias. |

Checklist rápido de diagnóstico:

1. `Assets::instance(...).manager()` é não nulo?
2. `_activity` já foi injetado antes desta chamada?
3. O caminho está **sem** prefixo `assets/` e com caixa/ortografia corretas?
4. O arquivo foi incluído no build (`-A`/`--assets`) e o APK foi reconstruído?
5. Você está usando `.size()` do resultado, em vez de assumir o tamanho do arquivo?

---

## 12. Resumo rápido

```cpp
#include <nativeapp/assets.hpp>

// 1) Na primeira chamada, passe um activity válido (ou chame set_activity).
//    O singleton é um local static (thread-safe, destruído no shutdown).
auto& assets = nativeapp::Assets::instance(activity);
assets.set_activity(activity);              // idempotente; ignora nullptr

// 2) Guarda antes de APIs que exigem manager.
if (!assets.manager()) return;

// 3) Existe? (abre e fecha em AASSET_MODE_BUFFER)
const bool ok = assets.exists("dados/config.json");

// 4) Bytes: use o tamanho do resultado, não o do arquivo.
const auto data = assets.read("dados/config.json");
if (!data.empty()) {
    const auto n = data.size();
    (void)n;
}

// 5) Texto (mesma leitura, convertido para std::string).
const std::string txt = assets.read_text("dados/config.json");

// 6) Gravação não existe.
const bool saved = assets.write("dados/novo.json", data);  // sempre false
(void)saved;
```

| Precisa de... | Use |
|---|---|
| Saber se o arquivo abre | `exists(path)` |
| Bytes brutos | `read(path)` → `std::vector<uint8_t>` |
| Texto / código GLSL | `read_text(path)` → `std::string` |
| Carregar shader GLES | `Shader::load(vert, frag)` (lança `GLError`) |
| Carregar imagem GLES | `Texture::load(path)` (PNG/JPEG/WebP/SVG, retorna `bool`) |
| Escrever em `assets/` | Não existe — `write()` sempre retorna `false` |
