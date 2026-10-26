# Let's Go Eevee — Port Android (letsgo-recomp)

**Relatório atualizado: 26/10/2026**
**Versão atual: v0090 (versionCode 1792949803)**
**Aparelho-alvo: Galaxy S23 (Snapdragon 8 Gen 2, Adreno 740, Android 16)**
**Uso: estritamente pessoal, sem distribuição. Dump próprio da usuária.**

---

## O QUE FUNCIONA

- **Boot e jogo completo jogável**: o jogo abre, carrega o save, o mundo 3D renderiza, personagens, mapa, batalhas e menus funcionam.
- **Importador**: importa o NSP próprio (Eevee 1.0.0, title `0100187003A36000`) e extrai `exefs`/`romfs` no aparelho. Funciona nas ROMs PT-BR e inglesa.
- **Controles**: touch na tela e controle físico Gamesir X3 funcionam.
- **Aspecto 16:9 correto**, resolução escalável no launcher (0.5x=480p, 0.75x=540p, 1x=720p, 1.5x=1080p).
- **Save externo**: sobrevive à desinstalação do APK (`save/save_0/savedata.bin`).
- **FPS**: ~22–30 fps no mundo (driver do sistema); overlay de FPS no canto.
- **Atlas de fonte**: Beluga rasteriza texto real — dump do atlas (2048×1024 R8) contém glifos legíveis (8.358 pixels não-zero).
- **Mundo 3D**: renderiza normalmente (usa vertex buffers in-place, não memória global de shader).

## O QUE NÃO FUNCIONA

### 1. Texto invisível (problema principal, NÃO resolvido até v0090)
- **Sintoma**: nenhum texto aparece em menus, diálogos, caixas de seleção ou HUD. As caixas de diálogo aparecem vazias.
- **O que já foi provado com logs do aparelho**:
  - O driver Adreno do sistema **não suporta `VK_EXT_external_memory_host`** → o heap do jogo nunca é importado como buffer GPU → todas as leituras de memória global dos shaders retornavam zero. Fallback `gcopy` implementado (v0078): copia ranges da CPU para buffer GPU antes de cada draw.
  - O diálogo produz **zero fragmentos** (TEXT_DIAG força magenta e nada aparece), enquanto o menu produz fragmentos (magenta apareceu na v0081).
  - O VS do diálogo usa `gmem`/`gfind`; o VS do menu usa só atributos + matriz.
  - Dados do `gcopy` são reais (janela cobre o sbuf, `gfind` mapeia certo, floats de posição/escala plausíveis).
  - **Eliminados por prova**: cull (v0085), depth (v0085), write_mask (v0086, já era 0xF), scissor/viewport (v0086, cheios), pipeline stale (v0086), CB0 truncado, `ok=false`, `a0` desvinculado (v0088, vinculado com layout padrão), cb3 (v0089, projeção ortográfica válida, NDC dos 4 cantos dentro de [-1,1]).
  - **v0090 (pendente de teste)**: VS_RANGE_DIAG — override condicional no VS do diálogo: se `gl_Position` final for ruim (w≤0, NaN, fora do clip), força quad centrado; se for boa, deixa como está. Resultado decide: quads magenta centrados = VS emite fora do range; nada = VS exonerado e o problema é downstream do VS.
- **Causa raiz**: ainda desconhecida. Todo o estado do draw foi verificado; o problema não é estado.

### 2. Driver Turnip não assume (pendente)
- O adrenotools localiza o arquivo (`turnip-8g2` ou `turnip-v26-3-0-r6-oneui`), mas a GPU ativa continua o Adreno do sistema (driver v512.676.73).
- Hipótese: falha do `linkernsbypass` no Android 16.
- Turnip é o único caminho com host import (resolveria o texto sem `gcopy`), mas é mais lento no Let's Go (18–22 fps vs 30 do sistema, medição antiga).
- Preferência da usuária: prefere que funcione com Turnip, se viável.

### 3. Atraso de ~2s na primeira abertura pós-v0078 (provável custo único)
- Causa provável: recompilação em massa de shaders (o `gcopy` mudou o GLSL → cache SPIR-V invalidou → shaderc recompila tudo no primeiro uso).
- Cache persiste em `cache/spirv/` no storage externo. Se voltar em toda abertura, investigar persistência do cache.

### 4. Sombras blocadas na Viridian Forest (parcialmente tratado)
- v0080 forçou samplers de sombra de NEAREST para LINEAR (PCF bilinear 4 taps em hardware). Resultado visual **não confirmado** pela usuária.
- Se sem mudança, próxima investigação = tamanho do shadow map.
- Remoção total das sombras NÃO implementada (existe `LARECOMP_NO_SHADOW`, força `VK_COMPARE_OP_ALWAYS`; decisão pendente).

### 5. Performance na floresta (não resolvido)
- Teto de ~13–14 fps na Viridian Forest em 1x (720p), ~17k draws de vegetação.
- `vk_wait_seq()` é GPU real; uploads proporcionais aos draws. Instancing/merging fora de escopo sem profiling (P3 pendente).

## HISTÓRICO DE BUILDS

- **v0010–v0076**: builds iniciais até a limpeza total de projetos (24/10/2026). 65 APKs apagados na limpeza.
- **v0077–v0090**: builds pós-limpeza (nova keystore, ver abaixo). Todas instalam por cima umas das outras.
  - v0077 gwin (inerte, heap < 2 GB)
  - v0078 gcopy (fallback CPU copies)
  - v0079 turnip-gcopy (extractNativeLibs + scan de CBs)
  - v0080 texto-sombras (scan CB 512 B + PCF nas sombras)
  - v0081 menu-diag (TEXT_DIAG estendido ao menu — tela toda magenta, shader errado)
  - v0082 magenta-alpha (TEXT_DIAG preserva alpha com piso 0,15)
  - v0083 sem-magenta-menu (TEXT_DIAG removido do menu, `[font-geo]` corrigido, caps 10→100)
  - v0084 gcopy-margem-diag (margem trailing 4096 + `[gcopy-diag]`)
  - v0085 cull-depth-off (força cull NONE + depth OFF no diálogo)
  - v0086 writemask (força write_mask 0xF — era no-op, já era 0xF; loga scissor final)
  - v0087 vs-diag (VS_DIAG: quad forçado — **magenta apareceu, provando que posições eram o killer**)
  - v0088 a0-fix (A0_FIX: `a0` de `gl_VertexIndex` — no-op, `a0` já estava vinculado)
  - v0089 cb3-diag (loga cb3 do diálogo e do menu — cb3 exonerado)
  - **v0090 vs-range-diag (ATUAL, pendente de teste)**: override condicional no VS do diálogo

## BUILD

- Pipeline: `bash build-runtime.sh` → `bash link-runtime.sh` → `bash build-apk.sh`
- Verificação obrigatória antes de anunciar: `SMOKE_OK` + `RUNTIME_LINK_OK` + `APK_OK`, timestamp novo, versionCode correto, assinatura válida.
- **Assinatura (ATUAL)**: keystore em `~/workspace/mvc2-xbox/apk-test/debug.keystore` (alias `androiddebugkey`, senha `android`, SHA-256 `F2:86:3C:0F:AB:D8:88:6A:EA:8C:19:04:2F:61:D0:D2:E9:60:60:0A:0F:C8:D5:3C:75:9D:A9:5C:54:55:3F:D8`). A keystore antiga foi perdida na limpeza de 24/10; builds a partir da v0077 usam esta. **Nunca** usar `~/.android/debug.keystore`.
- **JDK some da VM após restarts**: backup em `~/jdk-dl/jdk-17.0.20.1+1`; restaurar em `/usr/lib/jvm/java-17-openjdk-amd64`.
- **Nunca** forçar preTransform IDENTITY (congelou o aparelho). **Nunca** reintroduzir V-flip global (regressão v0062). Não modificar `deps/SDL3/`.
- Recompilação: `make gen ARCH=aarch64` → `src/generated-aarch64/` (972 arquivos C, 60.380 funções). Upstream letsgo-recomp 0.3.2 (PC da usuária mostra 0.4.2, fora de escopo).
- `libavcodec.so` deve ir **unstripped** (llvm-strip remove `ff_opus_decoder`).

## PENDÊNCIAS

1. **Testar v0090** (VS_RANGE_DIAG) — decide VS vs downstream.
2. Turnip: por que o hook não redireciona (adrenotools instrumentado nunca compilado — APK usa `libadrenotools.so` prebuilt).
3. Atraso de 2s: confirmar se é custo único ou persiste.
4. Sombras: confirmação visual do PCF na Viridian Forest.
5. P3: profiling de draws/uploads na floresta.
6. Embutir `cache/spirv/` compilado quando a usuária enviar o zip certo (os caches enviados são GLSL, não SPIR-V).
