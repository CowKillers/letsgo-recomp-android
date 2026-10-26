# BUILD.md — Tutorial de build do Let's Go Android

Como buildar o APK do zero. Leia tudo antes de começar: há pré-requisitos que você
precisa ter **antes** (dump próprio, chaves próprias) e armadilhas que já morderam.

## O que você precisa

### Hardware e sistema
- **Linux x86_64** (testado em Ubuntu/Debian). O build usa NDK + SDK Android.
- **Um aparelho Android arm64** para testar (testado no Galaxy S23, Android 16).
  O APK é arm64; não há build x86.

### Dump próprio (OBRIGATÓRIO — não está neste repo)
- **NSP do Pokémon Let's Go, Eevee!** extraído do **seu próprio console** (versão 1.0.0,
  title ID `0100187003A36000`). Sem ele, não há o que recompilar.
- **`prod.keys` e `title.keys` do seu próprio console.** Sem elas, o NSP não abre.
- ⚠️ **Não peça, não baixe e não distribua dumps ou chaves de terceiros.** Este projeto
  existe para uso pessoal com material próprio.

### Ferramentas
- **Android SDK** com `build-tools/35.0.0` e `platforms/android-34`
- **Android NDK 28.2.13676358** (`$ANDROID_SDK_ROOT/ndk/28.2.13676358`)
- **JDK 17** com `javac` (o `d8`, `aapt2`, `zipalign` e `apksigner` vêm do SDK)
- **git, python3, make, clang** (para a etapa de recompilação)
- **Um keystore de debug seu** (ex.: o `debug.keystore` padrão do Android SDK).
  ⚠️ **Use sempre o MESMO keystore em todas as builds** — o Android recusa instalar
  por cima um APK assinado com chave diferente (exige desinstalação limpa).

### Dependências pré-compiladas (`deps/`, não está neste repo)
O runtime precisa destas bibliotecas pré-compiladas para Android arm64:
- **SDL3** (não modificar o fonte — upstream proíbe código gerado por IA)
- **FFmpeg com Opus** (áudio do jogo usa Opus)
- **shaderc** (compilação de shaders em runtime)
- **adrenotools** (driver Turnip customizado — opcional, ver ISSUES.md)
- **VMA** (Vulkan Memory Allocator)

Compile-as para arm64 ou obtenha builds compatíveis antes da etapa 2.

## Passo a passo

### Etapa 0 — Gerar o código recompilado do jogo (só na 1ª vez ou com NSP novo)

```bash
cd src
make gen ARCH=aarch64
```

Isso lê o `main` do **seu** NSP e gera `src/generated-aarch64/` (~972 arquivos C,
~60 mil funções). É a etapa mais demorada e só precisa ser refeita se o NSP mudar.
**Nunca commite o conteúdo gerado** — ele é derivado do binário do jogo.

### Etapa 1 — Runtime (só compila, sem linkar ainda)

```bash
./build-runtime.sh
```

Espere **`SMOKE_OK`** no final. Objetos em `build/obj-android`.
Se falhar, veja `build-errors.log`.

### Etapa 2 — Importador de NSP

```bash
./build-importer.sh
```

Gera `build/liblgimport.so` (o importador que roda dentro do app no aparelho).

### Etapa 3 — Linkar o runtime

```bash
./link-runtime.sh
```

Gera `build/apk-lib/liblasrecomp.so`. Espere **`RUNTIME_LINK_OK`**.
Se falhar, veja `link-runtime-errors.log`.
Nota: o link inclui `-ladrenotools` (DT_NEEDED) para o suporte a driver Turnip.

### Etapa 4 — Biblioteca do jogo

```bash
./build-gamelib.sh
```

Gera `build/game/liblasgame.so` (o jogo recompilado) e `GAMELIB_ID.txt`.
Roda **depois** do runtime: a biblioteca do jogo é linkada contra `liblasrecomp.so`
(DT_NEEDED), porque no Android ela é aberta com `RTLD_LOCAL` e o loader só resolve
os símbolos dela pela própria lista de dependências.

### Etapa 5 — Montar e assinar o APK (sem Gradle)

```bash
./build-apk.sh
```

Cadeia: `javac` → `d8` → `aapt2` → `zipalign` → `apksigner`.
O APK sai na raiz (ex.: `LetsGoRecomp-android.apk`).
Espere **`APK_OK`**, confira o timestamp novo e o `versionCode`.

## Regras que já morderam (não quebrar de novo)

- **Incremente o `versionCode` a CADA build** — o Android ignora a instalação se o
  versionCode não subir (testes já foram perdidos por isso). Use timestamp Unix.
- **Nunca rode `llvm-strip` na `libavcodec.so`** — remove o símbolo `ff_opus_decoder`
  e o áudio morre. As outras libs podem ir com `--strip-unneeded`.
- **`System.loadLibrary` usa `RTLD_LOCAL`** — pré-carregar `.so` no Java NÃO exporta
  símbolo para o `.so` nativo; dependência nativa se resolve com `DT_NEEDED` no link.
- **Nunca force `preTransform` IDENTITY** — congelou o aparelho em teste.
- **Nunca reintroduza V-flip global** — foi regressão (v0062) e precisou ser removido (v0063).
- **Fullscreen imersivo se aplica DEPOIS de `super.onCreate()`** (o SDL reseta antes).
- **Não modifique `deps/SDL3/`** — upstream proíbe código gerado por IA.
- **Verifique antes de anunciar**: `SMOKE_OK` + `RUNTIME_LINK_OK` + `APK_OK`,
  timestamp novo, versionCode correto, assinatura válida com o seu keystore.

## Como preparar o jogo para o app aceitar (passo a passo)

O aplicativo **não** vem com o jogo e **não** aceita um NSP solto. Ele precisa de
duas coisas, em duas etapas separadas:

### Etapa A — Antes do build: gerar o código do jogo (na sua máquina Linux)

1. Tenha em mãos o **seu** NSP do Let's Go Eevee 1.0.0 e as **suas** `prod.keys` /
   `title.keys` (extraídas do seu próprio console).
2. Rode a geração (Etapa 0 acima): `cd src && make gen ARCH=aarch64`.
   Isso lê o `main` do seu NSP e cria `src/generated-aarch64/` — o código do jogo
   recompilado para arm64. **Esse código gerado nunca vai para o repositório** (é
   derivado do binário do jogo); cada pessoa gera o seu a partir do próprio dump.
3. Siga as Etapas 1–5 e instale o APK no aparelho.

### Etapa B — No aparelho: importar o jogo pelo app

1. Abra o app. O launcher mostra a tela de importação.
2. Aponte o importador para **o seu NSP** e para as **suas `prod.keys`**.
   O importador (SAF, sem root) lê o NSP e extrai, no storage externo do aparelho:
   - `extracted/exefs/` — executável e dados do jogo
   - `extracted/romfs.bin` — arquivos do jogo
   - `extracted/.import_ok` — marca de importação concluída
3. Se a importação for interrompida, ela é **retomável** — abra o app de novo e
   continue de onde parou.
4. Com o `.import_ok` presente, o app aceita o jogo e o boot começa. O save fica em
   `save/save_0/savedata.bin` (storage externo — **sobrevive à desinstalação do APK**).
5. No launcher, escolha a resolução: 0.5x=480p, 0.75x=540p, 1x=720p, 1.5x=1080p.

**Resumo:** o NSP próprio serve duas vezes — uma na sua máquina (para gerar o código
na Etapa A) e uma no aparelho (para o importador extrair os dados na Etapa B).
Sem o dump próprio, nenhuma das duas etapas funciona — e é por isso que o
repositório não inclui nenhum arquivo do jogo.

## Problemas? Veja ISSUES.md

O texto invisível, o Turnip que não assume, o atraso de 2s e as sombras blocadas
estão todos documentados em [ISSUES.md](ISSUES.md), com o que já foi tentado e eliminado.
