# 02_TURNIP_DIAGNOSIS.md

## Diagnóstico Turnip/Adrenotools
**Data:** 2026-10-21
**Status:** BLOQUEADO — causa raiz não identificável sem fonte do adrenotools

---

## Evidências

### Log do aparelho (v0054/v0056)
```
[vk] custom GPU driver /data/user/0/com.letsgorecomp/files/gpu_drivers/turnip-gen8-v37/libvulkan_freedreno.so failed to load via adrenotools: (null)
[vk] active GPU: Adreno (TM) 740 (vendor 0x5143, device 0x43050a01, driver v512.676.73)
```

### Verificações realizadas
1. ✅ Arquivo existe e é legível (`access(R_OK)` passa — padrão Wonder implementado v0052)
2. ✅ Arquitetura ELF AArch64 válida
3. ✅ Mesma chamada do Wonder (`adrenotools_open_libvulkan` com mesmos parâmetros)
4. ❌ Retorna NULL
5. ❌ `dlerror()` retorna `(null)` — sem informação de erro

### Comparação com Wonder
O Wonder usa o **mesmo código** e o Turnip funciona lá. Diferenças possíveis:
- Versão do Turnip (Wonder usa turnip-8g2, testamos gen8-v37 e 8g2)
- Versão do Android/OneUI
- Dependências do .so

---

## Causa Raiz

**Desconhecida.** O `adrenotools_open_libvulkan` falha internamente sem reportar erro via `dlerror()`. Sem o código-fonte do adrenotools, não é possível diagnosticar além.

---

## Ações Tentadas
1. Verificação `access()` antes da chamada (v0052) — arquivo legível, falha persiste
2. Log do driver ativo via `VkPhysicalDeviceProperties` (v0054) — confirma Adreno 740
3. Teste com múltiplas versões do Turnip (8g2, gen8-v37) — todas falham
4. Comparação com implementação do Wonder — código idêntico

---

## Recomendação

**Não bloquear o projeto no Turnip.** O jogo roda a 26.7 FPS médio com o driver do sistema. O Turnip é uma otimização, não um requisito.

Se o Turnip for crítico no futuro:
- Obter código-fonte do adrenotools para debug
- Ou testar com `dlopen()` direto (bypass adrenotools)
- Ou aguardar atualização do Turnip compatível

---

## Critério de Aprovação (não atingido)

- [ ] Driver selecionado realmente ativo
- [x] Fallback seguro para Qualcomm (funciona)
- [x] Causa informada (falha silenciosa documentada)
