# 01_BASELINE.md

## Baseline Reprodutível — Let's Go Android
**Data:** 2026-10-21
**Fonte:** `letsgo-log_170_zf8a.txt` (10 minutos de jogo, v0053)

---

## Configuração

| Item | Valor |
|------|-------|
| GPU | Adreno (TM) 740 |
| Vulkan | 1.3.128 |
| Driver ativo | Qualcomm v512.676.73 (sistema) |
| Driver solicitado | Turnip v26.3.0 R6 OneUI |
| Turnip | **Falha** no carregamento (`adrenotools: (null)`) |
| Resolução interna | 1280x720 (1x, scale=0.6667) |
| Upscaler | FSR1 → 1920x1080 |
| APK | v0053-perf (dynamic reset, SMAA off, GCOPY 16384, 2 workers) |

---

## Métricas Gerais (68 janelas de 2s)

| Métrica | Valor |
|---------|-------|
| FPS mínimo | 17.1 |
| FPS médio | 26.7 |
| FPS máximo | 30.4 |
| Draws máximo | 22.521 |
| Upload máximo | 883.6 MB/janela |
| Wait máximo | 2.930 ms |
| Compilações máximas | 251 shaders/janela |

---

## Trecho Crítico (Floresta)

| Métrica | Valor observado |
|---------|-----------------|
| FPS | 13.9 – 18 |
| Draws | 18.387 |
| Passes | 1.701 |
| Flushes | 62 |
| Upload | 654.7 MB |
| Wait | 2.944 ms |
| Compilações | 0 (cache aquecido) |

**Interpretação:** O gargalo NÃO é compilação de shaders (0 nessa janela). É a quantidade de draws (18k) e o tempo de espera da GPU (2.9s).

---

## Análise de Causa Raiz

### 1. Draw Calls (22k)
- **Causa:** Conteúdo do jogo (vegetação da floresta com milhares de meshes)
- **Corrigível no runtime?** NÃO — exigiria merging de geometria, fora do escopo
- **Mitigação:** Turnip (menor overhead por draw), resolução menor (ajuda fill-rate, não overhead)

### 2. Uploads (883 MB)
- **Causa:** Sintoma dos 22k draws — cada draw pode gerar uploads
- **Não são** 883 MB de texturas únicas; são alocações transient somadas
- **Mitigação aplicada (v0053):** GCOPY_TAB 4096→16384, dynamic flag reset

### 3. Wait (2.9s)
- **Causa:** CPU bloqueada em `vk_wait_seq()` esperando a GPU processar 22k draws
- **Mitigação potencial:** Watcher thread com fences (LASRecomp-Android-Plus) — não portado

### 4. Turnip
- **Status:** Falha no `adrenotools_open_libvulkan` (sem `dlerror`)
- **Arquivo:** Legível (`access()` passa)
- **Causa raiz:** Desconhecida — falha interna do adrenotools

---

## Frametime Estimado

Com FPS médio 26.7:
- Frametime médio: ~37.5 ms
- Alvo 30 FPS: 33.33 ms
- **Gap:** ~4 ms acima do alvo em média; 25 ms acima no pior caso (17 FPS = 58.8 ms)

---

## Próximos Passos (ETAPAs 2-4)

1. **ETAPA 2:** Diagnosticar Turnip — sem ele, o overhead de 22k draws é máximo
2. **ETAPA 3:** Investigar uploads — confirmar se há reuploads redundantes
3. **ETAPA 4:** Reduzir `wait` — portar watcher thread ou otimizar sincronização

---

## Arquivos

- Log fonte: `workspace/user/files/letsgo-log_170_zf8a.txt`
- APK: `LetsGoRecomp-v0053-perf.apk` (Drive `1o6_gOy8keEMQR8Qoqwsh4yYtN9A3L6U6`)
