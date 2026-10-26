# Análise do LASRecomp → viabilidade de um recomp do Pokémon Let's Go Eevee

_Documento histórico: a análise de viabilidade feita antes de começar. O estado atual está em
[PROGRESSO.md](PROGRESSO.md)._

Data: 2026-10-14 · Referência: `reference/LASRecomp` (commit clonado com `--depth 1`, licença MIT, © 2026 Peiikai)

## TL;DR

O LASRecomp **não é um port "amarrado" ao Link's Awakening**: o núcleo é um recompilador estático genérico
de Switch (NSO ARM64 → C) e um runtime que reimplementa o Horizon OS e a API gráfica **NVN** por nome de
função. O que é específico do Zelda fica em camadas isoláveis (patches por endereço, features de
randomizer, UI). Para o Let's Go, a estratégia é **fazer um fork do núcleo, remover a camada do Zelda** e ir
preenchendo o que faltar (serviços, funções NVN, áudio) guiado pelos logs de "UNIMPLEMENTED".

Viabilidade: **boa no papel**. O risco real só aparece quando rodarmos o gerador sobre o `main` do Let's Go
(tamanho do binário, módulos extras, serviços e funções NVN que o Zelda nunca usou).

## Como o LASRecomp funciona (pipeline)

```
extracted/exefs/main, sdk   (NSO, do dump do usuário)
        │
        ▼  tools/nso.py      descomprime LZ4, monta imagem, lê MOD0 / símbolos / relocações
        ▼  tools/analyze.py  acha funções (eh_frame + símbolos + relocações RELATIVE)
        ▼  tools/lifter.py   traduz cada instrução ARM64 → C   (Capstone)
        ▼  tools/gen.py      gera C por módulo; resolve jump tables emulando com Unicorn
        ▼  tools/passes.py, tools/patches.py   otimizações + patches por endereço (widescreen etc.)
        ▼  clang/zig cc      → lasgame.dll (código do jogo, gerado NA MÁQUINA do usuário)
        │
lasrecomp.exe (runtime) ── carrega lasgame.dll + lê RomFS do dump
```

- `make gen` roda: `gen.py sdk … 0x7004000000 --hle hle_symbols.txt` e `gen.py main … 0x7000000000`.
- Endereços do guest == endereços do host (janela reservada em `0x70_0000_0000`, ver `runtime/rt.h`).
- O lançador confere o **build id** do `main` (`runtime/ui/launcher.mm:85`) para garantir v1.0.0.

## O que é genérico (reaproveitável) vs. específico do Zelda

| Parte | Arquivos | Genérico? | Observação para o Let's Go |
|---|---|---|---|
| Loader NSO / relocação | `tools/nso.py`, `runtime/loader.c` | ✅ | Só carrega `main` + `sdk` (fixo em `loader.c:177`). Se o Let's Go tiver `subsdk*`, precisa estender. `SDK_BASE` = main + 64 MB: verificar se o `main` do Let's Go cabe. |
| Lifter ARM64 → C | `tools/lifter.py`, `tools/gen.py`, `tools/analyze.py`, `tools/passes.py`, `tools/simd_c.py` | ✅ | Instruções não suportadas viram `rt_udf()` e aparecem nas estatísticas: dá pra medir a cobertura logo de cara. |
| CPU / SIMD em C | `runtime/cpu.h`, `simd.h`, `simd_fp.h` | ✅ | Exige AVX2 no x86-64. |
| Kernel (SVCs) | `runtime/kernel.c` | ✅ | ~40 SVCs: threads, sync, memória, eventos. Cobre o básico de qualquer jogo. |
| IPC / serviços | `runtime/ipc.c`, `runtime/services/*` | 🟡 | Já tem: acc, aoc, apm, audout, audren, fs/save/sdcard, hid, lm, pctl, set, time. O Let's Go provavelmente pede mais (ex.: nifm, friends, ldn/comunicação local, bluetooth/Pokémon GO, pl/fontes). A maioria pode ser stub ("sem rede"). |
| HLE por símbolo | `runtime/dispatch.c`, `runtime/hle.h` | ✅ | Função chamada e não implementada → loga `[hle] UNIMPLEMENTED <nome>` e retorna 0. Ótimo para descobrir o que falta. |
| HLE de libs do SDK | `hle/heaps.c`, `hle/input.c`, `hle/vi.c`, `hle/swkbd.c`, `hle/framepace.c` | 🟡 | nn::lmem, nn::mem, nn::hid, vi… genéricos, mas cobrindo só as variantes que o Zelda usa. |
| **NVN (GPU) → Vulkan** | `runtime/nvn/*` (495 funções NVN implementadas), `nvn/vulkan/*` | 🟡 | Implementado na **API NVN**, por nome (`hle/nvn_boot.c`), então vale para qualquer jogo NVN. Faltam funções que o Zelda não usa (ex.: `nvnTextureReadTexels` loga "not implemented"). |
| Shaders Maxwell → SPIR-V | `runtime/nvn/maxwell/translate.c` (~2100 linhas) | 🟡 | Genérico em princípio, mas só testado com os shaders do Zelda. Ponto de maior risco de bugs gráficos. |
| Áudio | `runtime/services/audio.c` | 🟡 | Mixer do audren baseado no do Ryujinx. **Efeitos e splitters não implementados**: se o Let's Go usar reverb etc., precisa adicionar. |
| Vídeo (cutscenes) | `hle/movie.c` (FFmpeg) | 🟡 | Implementa a lib `nn::movie` que o Zelda usa. O Let's Go pode usar outra lib de vídeo. |
| Patches por endereço | `tools/patches.py` (48 endereços), `nvn/metal.m:589` | ❌ | Widescreen e culling do Zelda. Descartar e refazer depois para o Let's Go. |
| Features do jogo | `hle/{chamber,digs,doorjump,escape,hookshot,keysanity,miniboss,pets,pot_glow,rescue,shopsanity,souls,warp,qol,flags_ext,pl}.c`, `rando.c`, `tools/rando/*`, `third_party/LAS-Randomizer` | ❌ | Randomizer e QoL do Zelda. Remover. |
| UI | `runtime/ui/*` (ImGui) | 🟡 | Launcher e menu são reaproveitáveis como esqueleto. Abas de rando/warp/tracker/save editor saem. |
| Plataforma | `runtime/platform/*`, build Windows via MSYS2 UCRT64 | ✅ | `docs/BUILD-windows.md`. |

## O que só vamos saber com o seu dump

1. **Módulos no ExeFS**: só `main` + `sdk`, ou tem `subsdk0..N`?
2. **Tamanho do `main`**: cabe em 64 MB antes do `SDK_BASE`?
3. **Cobertura do lifter**: quantas instruções viram `rt_udf` (não suportadas).
4. **Funções importadas**: quais `nn::*` e `nvn*` o jogo chama que o runtime ainda não tem.
5. **Serviços IPC** pedidos no boot.

Tudo isso sai rodando `tools/nso.py` e `tools/gen.py` sobre o `main`/`sdk` extraídos, antes de compilar
qualquer coisa.

## Plano proposto

**Fase 0: preparar (agora)**
- [x] Clonar o LASRecomp como referência.
- [ ] Você: extrair o ExeFS e o RomFS do XCI com o `prod.keys` do seu Switch (hactool/hactoolnet/nstool)
      para `extracted/exefs` e `extracted/romfs` (pastas já no `.gitignore`).
- [ ] Montar o ambiente Windows (MSYS2 UCRT64, clang, SDL3, FFmpeg, shaderc, Python + capstone/unicorn/lz4).

**Fase 1: diagnóstico (sem compilar o jogo)**
- Rodar `nso.py` no `main`/`sdk`: módulos, tamanhos, imports.
- Rodar `gen.py` e medir instruções não suportadas.
- Cruzar os imports com o que o runtime já implementa → lista do que falta.

**Fase 2: fork enxuto**
- Copiar o núcleo (lifter, loader, kernel, ipc, services, nvn, platform, UI base) para `src/`.
- Remover a camada do Zelda (patches, rando, hle de gameplay, BUILD_ID).
- Parametrizar módulos/bases no loader.

**Fase 3: boot**
- Meta: logo da Game Freak / tela de título, mesmo com gráficos quebrados.
- Ciclo: rodar → ler `UNIMPLEMENTED` / crash → implementar → repetir.

**Fase 4: jogável**
- Pallet Town, batalhas, menus, saves, áudio. Depois widescreen e melhorias.

## Regras do projeto

- **Nada do jogo entra no git**: `extracted/`, `generated/`, `*.xci`, `*.nsp`, `*.nca`, `prod.keys`, `title.keys`.
- O código do jogo é gerado na máquina de quem tem o cartucho, igual ao LASRecomp. Releases nunca incluem código ou assets da Nintendo/Game Freak.
- Projeto pessoal / de estudo. Publicar tem risco jurídico (Nintendo/The Pokémon Company).
- Licença do código de referência: MIT. Manter o aviso de copyright do LASRecomp em qualquer arquivo derivado.
