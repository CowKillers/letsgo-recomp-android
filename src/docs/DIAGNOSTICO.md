# Diagnóstico: Pokémon Let's Go Eevee (v1.0.0, cartucho)

Data: 2026-10-14 · Extraído com hactool 1.4.0 + `prod.keys` do console do usuário · Gerador: `tools/gen` (do LASRecomp, sem os patches do Zelda)

## Módulos do ExeFS

| Módulo | O que é | Imagem | Build id | Exports / imports |
|---|---|---|---|---|
| `main` | o jogo (engine da Game Freak) | 30,9 MB (text 13,2 MB) | `0a3f0d21e67dad80` | 34 / 842 |
| `sdk` | nnSdk (bibliotecas da Nintendo, NVN incluso) | 12,2 MB | `eee08631aff2601e` | 22.986 / 13 |
| `subsdk0` | **glslc**: compilador de shaders GLSL → Maxwell | 14,2 MB | `7a7ca048269ec2e6` | 17 / 84 |
| `subsdk1` | **multimedia**: player de vídeo (`movie::`, base Android Stagefright + FDK AAC) | 7,0 MB | `15c1c7f4f364010c` | 10.605 / 642 |
| `rtld` | loader dinâmico | 16 KB | n/a | n/a |

- `main` cabe com folga antes do `SDK_BASE` do LASRecomp (64 MB).
- O runtime do LASRecomp carrega só `main` e `sdk`. **Precisamos estender o loader** para `subsdk0`/`subsdk1`, ou implementar o que o `main` usa deles direto no host (HLE).

## Cobertura do tradutor ARM64 → C

| Módulo | Funções | Instruções não suportadas | Jump tables resolvidas | Tempo |
|---|---|---|---|---|
| `main` | 60.380 | **0** | 1.457 | 34 s |
| `sdk` | 31.906 | **0** | 467 | 7 s |
| `subsdk0` | 17.984 | **0** | 1.603 | 21 s |
| `subsdk1` | 13.342 | **0** | 430 | 4 s |

O lifter do LASRecomp traduziu **100% do código** dos quatro módulos. Os `rt_udf` presentes no C gerado são
armadilhas para instruções `udf`/padding e fins de função, não falhas de tradução.

C gerado: ~550 MB em `generated/` (fora do git).

## O que o `main` usa de cada módulo

- **sdk**: 802 imports. A maior parte roda como código recompilado do próprio sdk. O que importa para nós é o
  que o sdk pede ao sistema (IPC e SVCs), que só aparece em execução.
- **glslc (subsdk0)**: 8 funções (`glslcInitialize`, `glslcCompile`, `glslcCompileSpecialized`,
  `glslcCompilePreSpecialized`, `glslcGetDefaultOptions`, `glslcSetAllocator`, `glslcFinalize`, `glslcGetVersion`).
  ⚠️ O jogo pode **compilar shaders GLSL em tempo de execução**. O Zelda não faz isso. Há duas opções:
  1. recompilar o `subsdk0` junto (já traduz 100%) para gerar Maxwell, que o tradutor de shaders do runtime já entende;
  2. HLE do glslc: compilar o GLSL direto para SPIR-V com shaderc e pular o Maxwell. Mais rápido e confiável, mas exige mudar a camada NVN.
- **multimedia (subsdk1)**: 28 funções `movie::`, **26 já implementadas** no `hle/movie.c` do LASRecomp.
  Faltam só `movie::MediaData::FindInt64` e `movie::SetCoreMask`. O vídeo deve funcionar quase de graça.

## Bibliotecas do sistema usadas pelo `main` (imports por namespace)

| Namespace | Imports | Situação no LASRecomp |
|---|---|---|
| nn::os | 86 | ✅ kernel/SVCs |
| nn::hid | 80 | 🟡 parcial (Npad). Let's Go usa Joy-Con com giroscópio e Poké Ball Plus |
| nn::socket | 40 | ❌ stub (sem rede) |
| nn::bluetooth | 30 | ❌ stub (Pokémon GO / Poké Ball Plus) |
| nn::ldn | 26 | ❌ stub (comunicação local) |
| nn::fontll / nn::pl | 24 + 4 | 🟡 fontes compartilhadas do sistema |
| nn::audio | 22 | 🟡 audren existe, sem efeitos |
| nn::bcat | 20 | ❌ stub (eventos/notícias) |
| nn::fs | 19 | ✅ |
| nn::account | 17 | ✅ acc:u0 |
| nn::oe | 16 | 🟡 am/applet |
| nn::time | 15 | ✅ |
| nn::err, swkbd, nifm, prepo, ssl, friends, ngc, nsd | ~45 | ❌ quase tudo stub |

A maioria dos ❌ é rede ou recursos online, que podem responder "indisponível" sem quebrar o jogo single-player.

## Conclusão

O cenário é melhor do que o esperado:
- **Tradução da CPU: resolvida** (0 instruções faltando).
- **Vídeo: quase resolvido** (26/28 funções já existem).
- **Riscos principais**:
  1. glslc / shaders em tempo de execução;
  2. a camada NVN → Vulkan e o tradutor de shaders Maxwell com os shaders da engine da Game Freak;
  3. muitos serviços de rede para stubar;
  4. controles com giroscópio, para os momentos de arremessar a Poké Ball.

## Próximos passos

1. Montar o ambiente de build no Windows (MSYS2 UCRT64: clang, SDL3, FFmpeg, shaderc, Vulkan SDK).
2. Fork enxuto do runtime do LASRecomp em `src/`: remover rando/QoL/UI do Zelda e o check do BUILD_ID;
   loader para 4 módulos (`main`, `sdk`, `subsdk0`, `subsdk1`).
3. Compilar tudo e rodar. Ler os logs `[hle] UNIMPLEMENTED`, serviços IPC desconhecidos e crashes.
4. Meta: tela de título.
