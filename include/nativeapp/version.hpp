// nativeapp/version.hpp — versão contratual da API nativeapp (v2.4.0).
//
// IMPORTANTE: os headers são distribuídos como código-fonte e compilados
// junto com o aplicativo. A API é **source-compatible**, não ABI-compatible:
// não prometemos layout de classes, símbolos, tamanho de structs nem
// compatibilidade com objetos/.so gerados por outra versão do apkfast.
// Sempre recompile o aplicativo ao atualizar o apkfast ou a NDK.
//
// Contrato versionado com Semantic Versioning:
//   - MAJOR: quebra de compatibilidade de código-fonte (o app precisa mudar).
//   - MINOR: nova funcionalidade/funcionalidade substituta, compatível.
//   - PATCH: correção sem mudança de API.
//
// Use APKFAST_API_VERSION_* em #if para escrever código que compila com
// múltiplas versões, e APKFAST_DEPRECATED para marcar APIs em retirada.

#pragma once

#define APKFAST_API_VERSION_MAJOR 2
#define APKFAST_API_VERSION_MINOR 7
#define APKFAST_API_VERSION_PATCH 1

#define APKFAST_API_VERSION_STR_(a, b, c) #a "." #b "." #c
#define APKFAST_API_VERSION_STRING \
    APKFAST_API_VERSION_STR_( \
        APKFAST_API_VERSION_MAJOR, APKFAST_API_VERSION_MINOR, APKFAST_API_VERSION_PATCH \
    )

// Inteiro monotonamente crescente, útil em diretivas #if:
//   #if APKFAST_API_VERSION >= 20400
#define APKFAST_API_VERSION \
    ((APKFAST_API_VERSION_MAJOR * 10000) \
     + (APKFAST_API_VERSION_MINOR * 100) \
     + APKFAST_API_VERSION_PATCH)

#if defined(_MSC_VER)
#  define APKFAST_DEPRECATED(msg) __declspec(deprecated(msg))
#elif defined(__GNUC__) || defined(__clang__)
#  define APKFAST_DEPRECATED(msg) __attribute__((deprecated(msg)))
#else
#  define APKFAST_DEPRECATED(msg)
#endif
