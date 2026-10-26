# BELUGA_GLYPH_TRACE.md

## Status: CAUSA AINDA NÃO ISOLADA — MAS RASTERIZAÇÃO CONFIRMADA!

**BREAKTHROUGH (log v0047, 20/10 09:10):**
```
[font-watch] WRITE DETECTED at 0x729c3aa92a (pc=0x7cfa29d19c) - game WROTE to font texture buffer!
[tex-audit-v2] atlas 2048x1024 nonzero 0 -> 3480 (frame 12)
[tex-audit-v2] atlas 2048x1024 nonzero 3480 -> 7524 (frame 35)
[tex-audit-v2] atlas 2048x1024 nonzero 7524 -> 4889 (frame 638)
```

O jogo **ESCREVE** glifos no atlas! A rasterização **FUNCIONA**.
- **Teste B (Rasterização)**: PASS — função chamada, PC `0x7cfa29d19c`
- **Teste C (Escrita)**: PASS — pixels escritos, conteúdo muda ao longo do tempo

**O problema está DEPOIS da rasterização.** Possibilidades:
- **Teste D**: O diálogo usa um atlas DIFERENTE (as 51 texturas pequenas em `0x7285e32000` estão zeradas — talvez o diálogo use essas, não a 2048x1024!)
- **Teste E**: O upload lê do endereço errado
- **Teste F**: O draw usa UVs errados ou a textura não está bound

**Próximo passo**: Instrumentar as 51 texturas pequenas para ver se o jogo escreve nelas.

Investigação do motor de fontes Beluga no port Android ARM64 de Pokémon Let's Go.

## Evidências Confirmadas

### 1. Texturas R8 Criadas mas Zeradas
- **51 texturas R8** (64x64, 32x32, 16x16, 8x8, 4x4) com `min=0 max=0 nonzero=0`
- Endereço guest: `0x7285e32000` (pequenas), `0x729c1bc000` (2048x1024)
- A 2048x1024 tem **3480 bytes não-zero** (0.16% preenchida) - alguns glifos foram rasterizados, mas não os do diálogo

### 2. Arquivos BFOTF Lidos Corretamente
- `FOT-RodinNTLGPro-DB.BFOTF`: 3,440,016 bytes
- `FOT-UDKakugoC80Pro-DB.BFOTF`: 3,540,728 bytes
- `beluga_font.BFOTF`: 2,564 bytes (provavelmente índice/config)
- Log `[TEXT_FILE_OPEN]` confirma leitura via `fs.c`

### 3. Idioma Correto
- `[am] GetDesiredLanguage: en-US`
- Arquivos `bin/message/English/` lidos (a tradução PT-BR substitui o English, então o jogo DEVE pedir en-US)

### 4. Driver GPU
- **Turnip FALHA ao carregar**: `[vk] custom GPU driver .../turnip-v26-3-0-r6-oneui/libvulkan_freedreno.so failed to load; using the system driver`
- O jogo sempre usa o driver do sistema Adreno (explica "não sinto diferença")
- Causa do fail ainda não diagnosticada (melhoria no log preparada, não compilada)

## Caminho Suspeito

```
Beluga (guest) → CPU → memória guest → buffer de glifos → textura R8 → Vulkan
```

**NÃO** é GMEM (`LARECOMP_VK_NO_GMEM`): investigação encerrada. Nenhum shader usa LDG/STG para memória global.

## Testes Planejados (Etapa 1-2)

### Teste A — Mensagens
- [ ] Confirmar que o `.dat` da mensagem é decodificado
- [ ] Verificar se o texto chega ao sistema de layout
- **Ponto de instrumentação**: `fs.c` após leitura do `/message/` (já temos o path, falta o conteúdo decodificado)

### Teste B — Rasterização
- [ ] Confirmar se a função Beluga de rasterização é chamada
- [ ] Identificar endereço guest, parâmetros, retorno
- **Ponto de instrumentação**: Watchpoint via `mprotect` no atlas 2048x1024 (implementado em v0047, não validado no aparelho)
- Se `[font-watch] WRITE DETECTED` aparecer: rasterização acontece, problema é posterior
- Se não aparecer: rasterização nunca escreve (ou escreve em outro buffer)

### Teste C — Escrita dos Glifos
- [ ] Verificar se pixels não-zero aparecem na memória guest
- [ ] Comparar hash/contagem antes/depois
- **Ponto de instrumentação**: `tex-audit-v2` (implementado em v0046, loga mudanças no `nonzero`)
- Atlas está em 3480 bytes e não cresce → novos glifos não são rasterizados

### Teste D — Memória
- [ ] Ponteiros inválidos, tradução de endereços, offsets, alinhamento, stride
- **Hipótese**: O jogo aloca o atlas em `0x729c1bc000` mas o Beluga escreve em outro endereço
- **Verificação**: Comparar `t->addr` da textura com o endereço que o Beluga usa (requer instrumentação do guest)

### Teste E — Textura Vulkan
- [ ] Confirmar que pixels chegam à textura do diálogo
- **Evidência atual**: Upload acontece (tex-audit loga), mas dados são zeros
- Se Teste C mostrar escrita mas Teste E mostrar zeros no upload: problema na cópia ou no `t->addr`

### Teste F — Desenho
- [ ] Descriptors, blending, swizzle, sincronização, coordenadas do atlas
- **Nota**: Se o texto usar outro formato (não R8), seguir o caminho real
- **Evidência**: Diálogo aparece (caixa renderiza) mas vazio → draw acontece, mas sem glifos ou com UVs errados

## Funções Guest Identificadas

**Nenhuma ainda.** O `liblasgame.so` tem 60.380 funções sem símbolos ELF (stripped).
As funções são nomeadas `f_<guest_addr>` no código-fonte gerado (ex: `f_70000001c0`).

**Estratégia de identificação** (não executada):
1. Usar o PC do watchpoint (quando disparar) para mapear para `f_<addr>`
2. Buscar referências cruzadas no código gerado para funções que acessam o atlas
3. Instrumentar seletivamente via wrappers HLE nos pontos de transferência

## Arquivos Modificados (preparados, não compilados)

1. `src/runtime/nvn/vulkan/device.c`: Log melhorado para falha do Turnip (inclui `dlerror()`)
2. `src/runtime/nvn/vulkan/textures.c`: 
   - Watchpoint via `mprotect` + handler SIGSEGV (v0047)
   - `tex-audit-v2` para rastrear mudanças no atlas (v0046)
3. `src/runtime/nvn/vulkan/glyph_trace.h`: (NOVO) Sistema `LARECOMP_TRACE_GLYPHS=1`

## Próximo Experimento Necessário

1. **Validar v0047 no aparelho**: Verificar se `[font-watch]` dispara
   - Se SIM: obter o PC, mapear para a função, instrumentar ela especificamente
   - Se NÃO: o Beluga nunca escreve no atlas → investigar se escreve em outro buffer (Teste D)

2. **Diagnosticar o Turnip**: Com o log melhorado, identificar por que `adrenotools_open_libvulkan` falha para o `turnip-v26-3-0-r6-oneui`

## Limitações

- Sem acesso ao Galaxy S23 para validação física
- Código guest sem símbolos dificulta a identificação estática
- O watchpoint via `mprotect` pode ter overhead se o jogo escrever frequentemente (mas o atlas parece ser write-once)

## Classificação

**CAUSA AINDA NÃO ISOLADA**

A instrumentação está preparada mas não foi validada no aparelho. O próximo passo crítico é o teste da v0047 para determinar se o Beluga escreve no atlas.
