# 06_SHADER_CACHE_AUDIT.md

## Auditoria de Shaders e Pipeline Cache
**Data:** 2026-10-21
**Status:** Sistema funcionando corretamente — nenhuma otimização necessária

---

## Componentes

### 1. SPIR-V Disk Cache
**Local:** `shaders.c:20-42`
**Path:** `<data_root>/cache/spirv/<hash>.spv`
**Chave:** FNV-1a do código GLSL + stage + versão (`+3`)

**Evidência de funcionamento:**
- Log 170, janela crítica: **0 compilações**
- Shaders foram compilados em janelas anteriores e reutilizados
- Cache persiste entre execuções

### 2. Pipeline Cache
**Local:** `device.c:638`
**Path:** `cache/vk_pipelines.bin`
**Função:** `vk_pipeline_cache_save()` (salvamento periódico)

**Evidência:**
- Pipelines são criados uma vez e reutilizados
- Draws não são permanentemente ignorados (0 skipped no log 176)

### 3. Compilação Assíncrona
**Workers:** 3 no Android (v0058)
**Mecanismo:** `run_job()` → fila → workers compilam em background
**Draws esperam:** `wait_built()` — necessário para correção

**Métricas (log 170, início):**
- 212 shaders: translate 181ms, spirv 250ms, pipelines 73ms
- Distribuído em 3 workers = eficiente

---

## Verificações

| Item | Status |
|------|--------|
| Chaves de shader únicas | ✅ FNV-1a + stage + versão |
| Chaves de pipeline | ✅ Derivadas do SPIR-V + estado |
| Persistência | ✅ Disco (`cache/spirv/`, `cache/vk_pipelines.bin`) |
| Invalidação por driver | ✅ Versão no hash (`+3`) |
| Duplicação | ✅ Nenhuma detectada |
| Compilação assíncrona | ✅ 3 workers |
| Reutilização | ✅ 0 compiles em janela aquecida |

---

## Veredito

**Sistema de shaders OTIMIZADO.** Nenhuma alteração necessária. O cache funciona, a compilação é assíncrona, e não há draws perdidos.

**Critério de aprovação:** ✅ Atingido
