# Investigação: Resolução Escalável estilo Emuladores

## Pergunta
É possível implementar opções de resolução tipo 1x=720p, 2x=1080p, 0.75x=540p, 0.5x=480p como nos emuladores de Switch?

## Resposta: SIM, a infraestrutura já existe.

## Como funciona hoje

### 1. `LARECOMP_SCALE` (forçado no Android)
**Arquivo**: `src/runtime/platform/android.c:121`
```c
setenv("LARECOMP_SCALE", "0.6667", 1);
```
Isso força render em 1280x720 (1080 * 0.6667 = 720). **Sobrescreve tudo.**

### 2. `output_scale.c` (lógica de escala)
**Arquivo**: `src/runtime/nvn/output_scale.c:72-90`

A altura de render é calculada como:
```c
if (forced > 0) h = forced;  // LARECOMP_SCALE
else if (g_settings.res_height > 0) h = g_settings.res_height / 1080.0;
else if (mode >= 1 && mode <= 8) h = mode;  // 1x a 8x
else {
    // ...
    else if (mode == RES_Q75) h *= 0.75;
    else if (mode == RES_Q25) h *= 0.25;
    else if (mode == RES_Q50) h *= 0.5;
}
```

Onde `h` é a altura em unidades de 1080p:
- `h = 1.0` → 1920x1080
- `h = 0.6667` → 1280x720
- `h = 0.5` → 960x540
- `h = 0.25` → 480x270

### 3. Settings do launcher
**Arquivo**: `src/runtime/settings.c:57-59`
```
resolution = dynamic | 75% | 50% | 25% | 1x | 2x | 3x | 4x
render_height = <360-4320>
```

## O que falta para implementar

### Passo 1: Remover o forçado no Android
Em `src/runtime/platform/android.c`, remover ou condicionar:
```c
// REMOVER:
setenv("LARECOMP_SCALE", "0.6667", 1);
```

### Passo 2: Adicionar UI no launcher
Em `LetsGoLauncherActivity.java`, adicionar um seletor (Spinner ou botões):
- 0.5x (960x540) — performance máxima
- 0.75x (1440x810) — equilibrado
- 1.0x (1280x720) — padrão atual
- 1.5x (1920x1080) — qualidade

### Passo 3: Aplicar a seleção
Duas opções:

**Opção A (env var)**: No `LetsGoActivity.java`, antes do `System.loadLibrary`:
```java
String scale = prefs.getString("resolution_scale", "0.6667");
Os.setenv("LARECOMP_SCALE", scale, true);
```

**Opção B (settings)**: Escrever no `larecomp.ini`:
```
resolution = 1x  // ou 2x, etc.
```
Ou:
```
render_height = 720  // pixels diretos
```

A Opção A é mais simples e não requer mudança no parser de settings.

## Mapeamento sugerido (base 720p)

| Opção | LARECOMP_SCALE | Resolução | Uso |
|-------|---------------|-----------|-----|
| 0.5x  | 0.3333        | 640x360   | Performance extrema |
| 0.75x | 0.5           | 960x540   | Performance |
| 1x    | 0.6667        | 1280x720  | Padrão (atual) |
| 1.5x  | 1.0           | 1920x1080 | Qualidade |

*Nota: Os valores da usuária (0.5x=480p) sugerem base diferente. Ajustar conforme necessário.*

## Impacto no desempenho

Áreas externas: 19/21 fps @ 720p. Reduzir para 540p (0.75x) deve dar ~30-40% mais fps (teórico).
Áreas internas: 27/30 fps @ 720p. Já bom.

## Riscos

1. **FSR1 upscaler**: O upscaler espera uma resolução de entrada específica. Mudar a escala pode exigir ajuste no FSR.
2. **UI/Touch**: As coordenadas de toque (`g_present_rect`) são calculadas baseadas na resolução. Precisa verificar se escala corretamente.
3. **Dynamic resolution**: Se `RES_DYNAMIC` estiver ativo, pode conflitar com a escala manual.

## Veredito

**VIÁVEL.** A infraestrutura existe e é usada no PC. Falta apenas:
1. Remover o `setenv` forçado (1 linha)
2. Adicionar UI no launcher (~30 linhas Java)
3. Passar a escala via env var (~5 linhas Java)

Estimativa: 1-2 horas de trabalho + teste no aparelho.
