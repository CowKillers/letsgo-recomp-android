# HOWTO-BUILD — Let's Go Recomp Android

Como buildar o APK do zero a partir deste projeto (pedido da Emily em 19/10/2026).

## Pré-requisitos (Linux x86_64)

- Android SDK com `build-tools/35.0.0` e `platforms/android-34`
- Android NDK **28.2.13676358** (`~/Android/Sdk/ndk/28.2.13676358`)
- JDK com `javac` (o `d8`/`aapt2`/`zipalign`/`apksigner` vêm do SDK)
- O keystore de debug compartilhado: `~/workspace/dr2cw-android/apk-test/debug.keystore`
  (alias `androiddebugkey`, senha `android` — o MESMO de todos os APKs dela)
- O dump próprio dela (NSP Let's Go Eevee 1.0.0 + prod.keys) só é preciso na etapa 1

## O que está onde

| Pasta/arquivo | Conteúdo |
|---|---|
| `src/` | código-fonte: `runtime/` (C/C++), `platform/android.c`, `apk/java/` (launcher), `generated-aarch64/` (código recompilado do jogo — gerado na etapa 1) |
| `deps/` | livrarias pré-compiladas: `SDL3`, `ffmpeg-android` (com Opus), `shaderc`, `adrenotools`, `vma` |
| `importer/` | importador de NSP in-app (`liblgimport.so`) |
| `*.sh` | scripts de build na raiz (ordem abaixo) |

## Ordem de build

```bash
# 0. (só 1ª vez ou com NSP novo) gerar o código recompilado do jogo:
cd src && make gen ARCH=aarch64        # lê o main do NSP dela -> generated-aarch64/

# 1. runtime (só compila, sem linkar ainda):
./build-runtime.sh                    # espera SMOKE_OK (objetos em build/obj-android)

# 2. importador de NSP:
./build-importer.sh                   # gera build/liblgimport.so

# 3. biblioteca do jogo:
./build-gamelib.sh                    # gera build/game/liblasgame.so + GAMELIB_ID.txt

# 4. linkar o runtime:
./link-runtime.sh                     # gera build/apk-lib/liblasrecomp.so (espera RUNTIME_LINK_OK)
                                      # nota: linka com -ladrenotools (DT_NEEDED) p/ o Turnip

# 5. montar e assinar o APK (sem Gradle: javac -> d8 -> aapt2 -> zipalign -> apksigner):
./build-apk.sh                        # APK sai na raiz (ex.: LetsGoRecomp-v0053-perf.apk)
```

## Regras que já morderam (não quebrar de novo)

- **Incrementar o versionCode a CADA build** — o Android ignora a instalação se o
  versionCode não subir (a Emily já perdeu testes por isso).
- **Nunca `llvm-strip` na libavcodec** — mata o `ff_opus_decoder` e o áudio morre.
- `System.loadLibrary` usa `RTLD_LOCAL` — pré-carregar `.so` no Java NÃO exporta
  símbolo p/ o `.so` nativo; dependência nativa se resolve com `DT_NEEDED` no link.
- Fullscreen imersivo se aplica **depois** de `super.onCreate()` (o SDL reseta antes).
- Erros de compilação: `build-errors.log`; erros de link: `link-runtime-errors.log`.

## Rebuild do FFmpeg com Opus

Se o áudio Opus quebrar, `rebuild-ffmpeg-opus.sh` recompila o `ffmpeg-android/`
com o decoder Opus (foi assim que o `[hwopus] decoder opened` voltou).
