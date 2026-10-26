# Como o LASRecomp-Android-Plus faz APK de 80MB

## Arquitetura

**Nosso port (390MB):**
- Compila o código do jogo no PC → `liblasgame.so` (~300MB)
- Empacota o `.so` pronto no APK
- APK = 390MB (runtime + game code pré-compilado)

**LASRecomp-Android-Plus (80MB):**
- **NÃO** inclui código do jogo no APK
- Inclui **ferramentas de build** no APK (Python, Zig, CCKit)
- O usuário fornece os arquivos do jogo (NSP/XCI ou exefs/romfs)
- O **app compila o código no celular** na primeira execução
- APK = 80MB (apenas runtime + ferramentas)

## Componentes no APK (80MB)

De `tools/android/ondevice/`:
1. **build_python.sh** → Python para scripts de build
2. **build_zig.sh** → Compilador Zig (para compilar o runtime)
3. **build_cckit.sh** → Kit de compilação C (para o código do jogo)
4. **make_assets.sh** → Empacota ferramentas em `ondevice.zip`

## Fluxo no Celular

1. Usuário instala APK (80MB)
2. Usuário fornece NSP/XCI ou pastas exefs/romfs
3. App extrai os arquivos
4. App roda o recompilador (LASRecomp) **no celular**
5. App compila o código gerado com Zig/CCKit **no celular**
6. Jogo pronto para jogar

## Vantagens
- APK 5x menor (80MB vs 390MB)
- Não redistribui código do jogo (mais seguro legalmente)
- Sempre compila da fonte do usuário

## Desvantagens
- Primeira execução demora (compilação no celular)
- Requer ~1GB livre para ferramentas + compilação
- Mais complexo de implementar

## Para Implementar no Let's Go

Seria necessário:
1. Remover `liblasgame.so` pré-compilado do APK
2. Adicionar Python + Zig + CCKit ao APK (~80MB)
3. Criar UI para o usuário fornecer o NSP
4. Portar os scripts de build para rodar no Android
5. Testar compilação on-device no S23

**Estimativa:** 2-3 semanas de trabalho (mudança arquitetural grande).
