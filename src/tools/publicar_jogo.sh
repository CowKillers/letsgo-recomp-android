#!/usr/bin/env bash
# publicar_jogo.sh (make jogo): copia o build atual (build/lasrecomp.exe + build/lasgame.dll) para build/jogo/, a
# cópia para jogar. Assim dá para recompilar e testar em build/ enquanto o jogo está aberto (o exe em uso fica travado
# no Windows). Rode só com o jogo fechado e com um build que já foi validado (tools/repro.sh).
# Leva junto as DLLs do MSYS2 de que o exe precisa e o ícone: build/jogo/lasrecomp.exe abre também com duplo clique
# (sem o MSYS2 no PATH); o runtime acha a pasta extracted/ duas pastas acima (runtime/paths.c).
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
dest=${1:-build/jogo}  # (another folder: a test copy)
mkdir -p "$dest"
cp build/lasrecomp.exe "$dest/lasrecomp.exe"
cmp -s build/lasgame.dll "$dest/lasgame.dll" 2>/dev/null || cp build/lasgame.dll "$dest/lasgame.dll"
cp build/lasgame.dll.id "$dest/lasgame.dll.id"  # (the runtime refuses a game library without its matching .id)
ldd build/lasrecomp.exe | awk '/ucrt64/ {print $3}' | sort -u | while read -r dll; do cp -u "$dll" "$dest/"; done
cp docs/icon/icon_1024.png "$dest/icon.png"  # (window icon)
echo "$dest atualizado: $(date "+%d/%m %H:%M")" | tee "$dest/versao.txt"
