# Bibliotecas próprias (`.a` / `.so`) — v2.1.0

O apkfast agora linka bibliotecas que você já compilou, sem Makefile e sem
Gradle.

## Convenção de diretórios

```text
meu-app/
├── libs/
│   ├── arm64-v8a/
│   │   ├── libfoo.a          → linkada dentro de libmain.so
│   │   └── libvendor.so      → copiada para lib/arm64-v8a/ no APK
│   ├── armeabi-v7a/
│   │   └── libfoo.a
│   ├── x86/
│   └── x86_64/
├── jni/ (ou src/main/cpp/)
└── apkfast.json
```

- `.a` → entra na linha do linker (estática)
- `.so` → é copiada para `lib/<abi>/` no APK (compartilhada)

Se o projeto tiver **uma única ABI**, libs soltas em `libs/` (sem subpasta)
também funcionam. Em **fat APK**, isso é ambíguo e o build falha com
mensagem orientando a usar `libs/<abi>/`.

## Via CLI

```sh
python3 apkfast.py build . --lib libs/arm64-v8a/libfoo.a
python3 apkfast.py build . --libs-dir /caminho/outras/libs
python3 apkfast.py build . --abi all   # usa libs/<abi>/ automaticamente
```

`--lib` é repitível. Um diretório passado a `--lib` é tratado como raiz de
libs (com subpastas por ABI).

## Via `apkfast.json`

```json
{
  "nativeLibraryName": "main",
  "libraries": ["libs/arm64-v8a/libfoo.a"],
  "libsDir": "libs"
}
```

Flags de CLI têm precedência sobre o JSON.

## Como gerar uma `.a` com o NDK (exemplo)

```sh
NDK=~/Android/android-ndk-r30
CC=$NDK/toolchains/llvm/prebuilt/linux-arm64/bin/aarch64-linux-android36-clang
AR=$NDK/toolchains/llvm/prebuilt/linux-arm64/bin/llvm-ar
$CC -c foo.c -o foo.o
$AR rcs libs/arm64-v8a/libfoo.a foo.o
```

## Segurança entre ABIs

Uma `.a` ou `.so` dentro de `libs/<outra-abi>/` é **ignorada** no build da ABI
atual. Isso evita o erro clássico de linkar arm64 dentro de x86 num fat APK.
Para forçar, use `--lib` com o caminho explícito.

## Ordem de linkagem

As `.a` do projeto entram **antes** das libs do sistema (`-lm`, `-landroid`,
`-llog`, ...), que é a ordem correta para resolução de símbolos. As libs do
apkfast (raylib/SDL) continuam sendo injetadas automaticamente depois,
conforme o `#include` detectado.

## Validação real

Projeto de teste com `libapkfasttest.a` retornando 42:

```
[*] Bibliotecas do projeto (arm64-v8a): 1 estática(s), 0 compartilhada(s)
mylib-test: answer=42
```

E com `.so` externa, o APK continha:
```
lib/arm64-v8a/libapkfastso.so
lib/arm64-v8a/libmain.so
```
