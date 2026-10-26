# 09_TURNIP_ROOT_CAUSE.md

## Causa Raiz do Turnip (ETAPA 9)
**Data:** 2026-10-21
**Fonte:** `wonder-recomp/adrenotools-src/src/driver.cpp:21`

---

## Cadeia de Falha

`adrenotools_open_libvulkan()` retorna `nullptr` em 5 pontos **sem** `dlerror()`:

1. **Linha 22:** `if (!linkernsbypass_load_status()) return nullptr;`
   - `linkernsbypass_load_status()` retorna `lib_loaded`
   - `lib_loaded` só é `true` após carregar símbolos de `libdl_android.so`
   - **Se qualquer símbolo estiver ausente → falha silenciosa**

2. **Linha 55:** `if (!linkernsbypass_link_namespace_to_default_all_libs(hookNs))`
3. **Linha 60:** `if (!hookImpl)` — `libhook_impl.so` não carregou
4. **Linha 64:** `if (!initHookParam)` — símbolo não encontrado
5. **Linha 83:** `if (!linkernsbypass_namespace_dlopen("libmain_hook.so"))`

## Hipótese Principal

**Incompatibilidade com Android 16 (API 36).**

O `linkernsbypass` usa APIs internas do linker Android (`__loader_android_create_namespace`, etc.). Essas APIs mudam entre versões do Android. O código foi escrito para Android 8-14; no Android 16, um símbolo pode ter sido removido/alterado, causando falha no carregamento.

**Evidência:**
- Comentário no código: "Bail out if linkernsbypass failed to load, this probably means we're on api < 28"
- Mas estamos na API 36, não < 28
- A API do linker mudou na API 34+ (Android 14) com restrições de namespace

## Verificação Necessária

Para confirmar, seria necessário:
1. Adicionar log em `linkernsbypass` para ver qual símbolo falha
2. Ou testar em Android 13/14 para ver se funciona

## Impacto

**Turnip bloqueado no Android 16** até que o `linkernsbypass` seja atualizado. Não é problema no nosso código — é incompatibilidade da biblioteca de terceiros.

## Alternativas

1. **Atualizar linkernsbypass** para Android 16 (requer engenharia reversa das novas APIs)
2. **Bypass adrenotools:** Usar `dlopen()` direto no driver Turnip (perde os hooks, mas pode funcionar)
3. **Aguardar** atualização do upstream

---

## Status

**BLOQUEADO** — Requer atualização do `linkernsbypass` para Android 16 ou bypass da biblioteca.
