# TEXT_RENDER_AUDIT.md — Auditoria do pipeline de texto
**Projeto:** Pokémon Let's Go Eevee — port Android (LAS Recomp)
**Data:** 20/10/2026 | **Ordem:** ORDEM MESTRE — correção de textos invisíveis
**Base:** letsgo-recomp v0.3.2 (raulzrrs) + adaptações Android (Muse)

## 1. Estrutura do projeto

| Componente | Local | Origem |
|---|---|---|
| Jogo recompilado (ARM64) | `build/game/liblasgame.so` | `make gen ARCH=aarch64` (972 arquivos C) |
| Runtime (HLE + NVN→Vulkan) | `src/runtime/` → `build/apk-lib/liblasrecomp.so` | LAS Recomp + adaptações Android |
| Carregador RomFS | `src/runtime/services/fs.c` | LAS Recomp (romfs.bin) |
| Backend Vulkan | `src/runtime/nvn/vulkan/` (device.c, exec.c, textures.c, shaders.c) | LAS Recomp + Android |
| Serviço pl:u | `src/runtime/services/pl_u.c` | **Criado para Android** (não existe no upstream) |
| Fontes sistema (HLE) | `src/runtime/hle/pl.c` | LAS Recomp + ordem Android modificada |
| Launcher Android | `apk/java/com/letsgorecomp/` | **Criado para Android** |

## 2. Pipeline de texto (mapeado)

```
RomFS (romfs.bin)
  ├─ bin/font/*.BFOTF (FOT-Rodin 3.3MB, FOT-UDKakugo 3.4MB, beluga_font 2.6KB, num_00.bffnt 138KB)
  │   └─ Motor Beluga (código guest) → rasteriza glifos → atlas em NVN (R8)
  ├─ bin/message/English/{common,script}/*.dat + *.tbl (TRADUÇÃO PT-BR substituiu)
  │   └─ Sistema de mensagens (guest) → strings → layout → draw
  └─ nn::pl (fonte do sistema)
      ├─ HLE: _ZN2nn2pl20GetSharedFontAddress... → pl.c → HLE heap (guest-visível)
      └─ IPC: pl:u cmd 0 (RequestLoad) → pl_u.c → load state 1

Atlas de glifos (R8) → upload via textures.c (deswizzle Tegra) → Vulkan
Texto desenhado como quads 2D com alpha blending → shaders GLSL→SPIR-V (shaders.c)
```

## 3. Estado verificado (com evidência)

- [x] **Fonte carrega:** log `[pl] system font ... (759720 bytes)`. O load ocorre via
      `font_for()` chamado pelo HLE `GetSharedFontAddress` (não há cmd 1/2 no log do pl:u).
- [x] **Endereço guest-válido:** `hle_alloc` usa `HLE_HEAP` + `plat_commit` (dispatch.c:124).
- [x] **pl:u responde:** cmd 0 → load state 1 para types 1,2,3,4. Cmd 1/2/3/4 corrigidos
      para o protocolo real (antes: 1→size, 2→address, errado).
- [x] **Mensagens íntegras:** `bag.dat`, `script/*.dat` têm headers válidos (v1 + offsets).
- [x] **Título renderiza texto:** "Aperte o Botão A" aparece → pipeline básico funciona.
- [x] **R8 mapeado:** `fmt_of(0x01) = VK_FORMAT_R8_UNORM` (textures.c:22).
- [x] **FlushTexels:** implementado (objects.c:445) → `backend_pool_written` → re-upload.
- [x] **Falhas de shader são logadas:** `[shader] ... compile failed` (shaders.c:155).

## 4. O defeito

**Caixas de diálogo e menus aparecem VAZIOS.** O texto do título funciona, o diálogo não.

### Hipóteses (ordenadas)

| # | Hipótese | Status |
|---|---|---|
| H1 | DejaVu/Noto incompatível com o motor de fontes do jogo | TESTADO (v2345/v0010 usam Noto) — **não resolveu** |
| H2 | Diálogo usa BFOTF do RomFS por caminho diferente do título | **EM INVESTIGAÇÃO** |
| H3 | Tradução PT-BR corrompeu os arquivos de mensagem | PENDENTE (teste com ROM inglesa original) |
| H4 | Atlas de glifos nunca recebe dados (max=0) | **DIAGNÓSTICO EM v0020** (`[tex-audit]` min/max) |
| H5 | Shader específico do texto falha no Adreno 740 | PENDENTE (verificar `[shader] compile failed` no log) |
| H6 | Problema no draw/blending do texto | PENDENTE |

## 5. Diagnóstico implementado (v0020)

`textures.c` `upload_texture()`: para texturas R8/RG8, loga `min/max/nonzero` dos bytes
da memória guest no momento do upload:
```
[tex-audit] R8/RG8 tex fmt=0x1 512x512 min=0 max=0 nonzero=0/262144 dyn=0 uploaded=0
```
- **max=0 em tudo** → o jogo nunca escreve glifo → defeito no motor de fontes (H2/H3).
- **max>0** → glifos existem → defeito no draw/shader (H5/H6).

## 6. Arquivos alterados (até v0020)

- `src/runtime/services/pl_u.c` — serviço pl:u criado + protocolo corrigido
- `src/runtime/hle/pl.c` — ordem de fontes no Android (Noto/Roboto antes de DejaVu)
- `src/runtime/nvn/vulkan/textures.c` — diagnóstico `[tex-audit]`
- `src/runtime/nvn/objects.c` — `nvnTextureFlushTexels/InvalidateTexels` implementados
- `src/runtime/nvn/vulkan/device.c` — swap de extent (aspect), present_box()
- `src/runtime/nvn/vulkan/exec.c` — vk_present_draw com viewport separada
- `src/runtime/settings.c` — fullscreen forçado no Android
- `apk/java/com/letsgorecomp/` — launcher, overlay, importador de shaders

## 7. Dependências pendentes

- **ROM inglesa original:** a Emily vai baixar para o teste A/B (ETAPA 1).
  Sem ela, H3 não pode ser isolada.
- **Log com `[tex-audit]`:** aguardando ela rodar um diálogo na v0020 e mandar o log.

## 8. ETAPA 5 — Auditoria Vulkan (estática, 20/10)

- **Blending:** `blend_factor()`/`blend_op()` em exec.c mapeiam todos os fatores NVN
  (SRC_ALPHA=5, ONE_MINUS_SRC_ALPHA=6 corretos). O fundo da caixa de diálogo (que usa
  blending) renderiza — blending global funciona.
- **Swizzle:** `vk_view()` (textures.c) aplica o swizzle NVN solicitado pelo jogo;
  default = identidade. Para R8 sem swizzle explícito, o canal R chega intacto.
- **Color write mask:** aplicado por attachment a partir do estado NVN (exec.c:1711).
- **Shaders:** falhas de compilação GLSL→SPIR-V são logadas (`[shader] compile failed`)
  e o draw é PULADO quando o programa não fica pronto (exec.c) — verificar no log.
- **Conclusão estática:** o caminho Vulkan é o mesmo que funciona no PC (upstream).
  A diferença restante é o driver Adreno 740. A evidência decisiva é o log da v0030:
  `[tex-audit]` (atlas tem dados?) + `[shader] compile failed` (algum shader falhou?).
