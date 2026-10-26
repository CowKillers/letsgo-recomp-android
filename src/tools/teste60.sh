#!/usr/bin/env bash
# teste60.sh <cena> <modo> [VAR=valor ...]: a mesma cena a 30 fps e num modo de 60 fps, quadro a quadro no tempo do jogo.
#
#   cena: captura (andar no mato da Rota 1 até um encontro, jogar a bola) | batalha (andar para baixo: a treinadora da
#         Floresta de Viridian, diálogo avançado com A)
#   modo: 0 (30 fps, referência) | 1 (mundo a 60, cenas a 30 + interpolação) | 2 (tudo a 60, contadores divididos)
# VAR=valor extras vão para o jogo (ex.: LARECOMP_WINDOW_SHOTS_REL=1 LARECOMP_WINDOW_SHOTS=... para os pares
# intermediário/final, medidos com tools/interp_metrics.py).
#
# Captura imagens da cena em instantes relativos ao início dela (LARECOMP_SHOT_REL; fps60.c marca o começo): a 30 fps
# em 30, 60, 90...; nos modos 60 em 60, 120, 180... (o mesmo tempo de jogo, se a lógica estiver na velocidade certa).
# Saída: build/teste60/<cena>-<modo>/rel_*.png e sheet.png. Compare as folhas de 0 e do modo testado: a mesma linha deve
# mostrar o mesmo momento da cena. Roda em turbo (LARECOMP_FPS=0): a lógica avança um passo por quadro, então os quadros
# de jogo são idênticos aos do ritmo real.
# A rota depende do sorteio do encontro: o número do quadro em que a cena começa varia; as imagens são relativas a ele.
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
cena=${1:?cena: captura ou batalha}
modo=${2:?modo: 0, 1 ou 2}
shift 2
base="400:A:6,500:A:6,1150:A:6,1300:A:6"
case "$cena" in
    captura) route="$base,1400:STICK_LEFT:300,1700:STICK_DOWN:120,1830:STICK_LEFT:150,1990:STICK_DOWN:100"
             for t in $(seq 2020 120 3200); do route="$route,$t:A:6"; done; last=3300 ;;
    batalha) route="$base,1400:STICK_DOWN:400"
             for t in $(seq 1900 40 3500); do route="$route,$t:A:6"; done; last=3700 ;;
    *) echo "cena desconhecida: $cena"; exit 1 ;;
esac
k=1; [ "$modo" = 0 ] || k=2
# In the 60 fps modes the world (and, in mode 2, the scenes) take two updates per game-time step: the inputs from the
# gameplay on (frame 1400) are stretched by k, so the route and the A presses land at the same game time.
route=$(echo "$route" | tr ',' '\n' | awk -F: -v k="$k" '{ t = $1; d = $3; if (t >= 1400) { t = 1400 + (t - 1400) * k; d = d * k } print t ":" $2 ":" d }' | paste -sd,)
last=$((1400 + (last - 1400) * k))
kr=1; [ "$modo" = 2 ] && kr=2  # (mode 1: the scenes keep one update per present)
rel=$(for t in $(seq 30 30 600); do echo -n "$((t * kr)),"; done)
out="build/teste60/$cena-$modo"
rm -rf "$out"; mkdir -p "$out"
fps=()
[ "$modo" = 0 ] || fps=("LARECOMP_FPS60=$modo")
tools/repro.sh "t60-$cena-$modo" "$last" "LARECOMP_INPUT=$route" "LARECOMP_SHOT_REL=${rel%,}" "${fps[@]}" "$@" >/dev/null 2>&1 || true
cp "build/repro/t60-$cena-$modo"/{rel,mid,fin}_*.png "$out/" 2>/dev/null || true
n=$(ls "$out"/rel_*.png 2>/dev/null | wc -l)
[ "$n" -gt 0 ] && FFMPEG=ffmpeg ./.venv/Scripts/python tools/contact_sheet.py "$out/sheet.png" $(ls "$out"/rel_*.png | sort -V) >/dev/null
echo "$out: $n imagens (cena começou no quadro $(grep -o 'scene start [0-9]*' "logs/repro-t60-$cena-$modo.log" | head -1 | grep -o '[0-9]*$' || echo '?'))"
