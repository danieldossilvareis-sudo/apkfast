# Codecs de áudio (`codecs.hpp` e `music.hpp`)

## Formatos suportados na v2.0.0

| Formato | API | Uso típico |
|---|---|---|
| WAV PCM 16-bit | `Audio::load_wav()` | Efeitos curtos |
| OGG Vorbis | `Codecs::decode_ogg()` / `Audio::load()` | Música/efeitos |
| MP3 | `Codecs::decode_mp3()` / `Audio::load()` | Música/efeitos |
| Qualquer acima | `Audio::load(path, buffer)` | Detecção por extensão |

## Decode completo (efeitos)

```cpp
AudioBuffer beep;
Audio::instance().load("beep.wav", beep);
Audio::instance().load("tone.ogg", beep);  // detecta .ogg
Audio::instance().play(beep, 0.8f);
```

`AudioBuffer` contém `samples`, `sample_rate`, `channels` — o `play()` converte
mono→estéreo e aplica volume.

## Streaming (`Music`)

Para trilhas longas (3–5 min), **não** decodifique tudo na memória. Use `Music`:

```cpp
Music music;
music.open("tone.mp3");        // .ogg ou .mp3
music.set_volume(0.35f);
music.set_loop(true);
music.play();                  // AAudio data callback
music.pause();
music.stop();
```

- Requer **API 26+ (AAudio)** — em API 21–25 use decode completo.
- O decoder roda na thread de áudio, em chunks — memória constante.
- Resample linear para 44100 Hz e conversão mono→estéreo automáticos.
- `open()` falha silenciosamente em API < 26; verifique com `is_playing()`.

## Caminhos de assets

Use caminhos **relativos a `assets/`**: `"tone.ogg"`, não `"assets/tone.ogg"`.

## Licenças

- stb_vorbis e stb_image: domínio público / MIT
- dr_mp3: domínio público / MIT-0
- Implementações em `src/nativeapp_impl.cpp` (injetado pelo builder).
