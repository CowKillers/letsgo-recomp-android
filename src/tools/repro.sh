#!/usr/bin/env bash
# repro.sh <nome> [frames a capturar] [VAR=valor ...]: reprodução determinística a partir do save.
#
# Carrega uma CÓPIA do save (save/ fica intocado) em build/test-save, ignora o controle real e aperta A só nos
# momentos mapeados (seleção de controle, confirmação, título, continuar). Do frame ~1150 em diante o jogo está no
# gameplay (Rota 1). Saída: logs/repro-<nome>.log e build/repro/<nome>/frame_N.png (+ contact.png).
# Um teste por vez: todos usam build/frames e build/test-save.
#
# Exemplo:  tools/repro.sh base 1300,1400
#           tools/repro.sh paleta 1300,1400 LARECOMP_VK_SKIN_IDENTITY=1
#           EXE=./build/jogo/lasrecomp.exe tools/repro.sh jogo 1400   (a cópia de make jogo)
#           RENDERDOC=1 tools/repro.sh rdc 1300 LARECOMP_RENDERDOC_AT=1290   (captura em build/rdc/)
#           SAVE="$HOME/Downloads/savedata.bin" tools/repro.sh outro 1400   (outro save, copiado)
# Turbo (padrão): LARECOMP_FPS=0, sem a trava de 30 fps. A lógica anda um passo por quadro, então os mesmos quadros
# chegam ~4x mais rápido e os scripts (contados em quadros) valem igual: cenário, câmera e posições saem idênticos; só
# animações de espera ligadas ao tempo real ficam em outra fase. TURBO=0 volta ao ritmo real (medir fps, áudio).
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"

name=${1:?nome do teste}
shots=${2:-1400}
shift $(( $# >= 2 ? 2 : 1 ))

rm -rf build/test-save "build/repro/$name"
rm -f build/frames/*.png  # (files only: a removed folder still open somewhere stays "delete pending" and vanishes later)
[ -n "${RENDERDOC:-}" ] && rm -rf build/rdc && mkdir -p build/rdc
mkdir -p build/test-save "build/repro/$name" logs build/frames
# SAVE=<savedata.bin or a save folder>: another save (copied too; the player's save/ stays untouched)
if [ -f "${SAVE:-}" ]; then mkdir -p build/test-save/save_0 && cp "$SAVE" build/test-save/save_0/savedata.bin
else cp -r "${SAVE:-save}"/* build/test-save/; fi

last=${shots##*,}
start=$(date +%s)
(   # the game itself is this background process (exec, no env wrapper): its Windows pid is the one killed below
    [ "${TURBO:-1}" = 0 ] || export LARECOMP_FPS=0
    export LARECOMP_INPUT="400:A:6,500:A:6,1150:A:6,1300:A:6" LARECOMP_NO_USER_INPUT=1 \
        LARECOMP_SAVE_DIR=build/test-save LARECOMP_NO_AUDIO=1 LARECOMP_SHOT_AT="$shots"
    for kv in "$@"; do export "$kv"; done
    if [ -n "${RENDERDOC:-}" ]; then  # under RenderDoc: LARECOMP_RENDERDOC_AT=<frame> picks the frame (build/rdc/)
        exec "/c/Program Files/RenderDoc/renderdoccmd.exe" capture -w -d "$(cygpath -w "$PWD")" ./build/lasrecomp.exe extracted --play             > "logs/repro-$name.log" 2>&1
    fi
    exec "${EXE:-./build/lasrecomp.exe}" extracted --play > "logs/repro-$name.log" 2>&1
) &
pid=$!
last_png=$(printf "build/frames/frame_%05d.png" "$last")
for _ in $(seq 1 "${WAIT:-240}"); do  # (WAIT=<seconds>: how long to wait for the last frame; long scenes need more)
    [ -f "$last_png" ] && break
    grep -q "FATAL\|exception c0" "logs/repro-$name.log" && { echo "CRASH: veja logs/repro-$name.log"; break; }
    sleep 1
done
sleep 1  # (the screenshot file is written; let the log catch up)
if [ -n "${RENDERDOC:-}" ]; then  # the capture is written in the background: wait until its size settles
    for _ in $(seq 1 120); do
        if ls build/rdc/*.rdc >/dev/null 2>&1; then
            s1=$(cat build/rdc/*.rdc | wc -c); sleep 5; [ "$s1" = "$(cat build/rdc/*.rdc | wc -c)" ] && break
        else sleep 1; fi
    done
    ls -la build/rdc/
fi
winpid=$(cat "/proc/$pid/winpid" 2>/dev/null || true)
if [ -n "$winpid" ]; then taskkill //F //T //PID "$winpid" >/dev/null 2>&1 || true; else kill "$pid" 2>/dev/null || true; fi
# The runtime re-executes itself when the guest address range is taken (runtime/main.c reserve_guest_space): the game
# is then outside the killed tree. Stop the copies of this exe started since this test began - never older ones (the
# player's game).
exe_win=$(cygpath -w "$(realpath "${EXE:-./build/lasrecomp.exe}")")
powershell.exe -NoProfile -Command "\$t = [DateTimeOffset]::FromUnixTimeSeconds($start).LocalDateTime; Get-CimInstance Win32_Process -Filter \"Name='lasrecomp.exe'\" | Where-Object { \$_.ExecutablePath -eq '$exe_win' -and \$_.CreationDate -ge \$t } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" >/dev/null 2>&1 || true
wait "$pid" 2>/dev/null || true
cp build/frames/*.png "build/repro/$name/" 2>/dev/null || true
FFMPEG=ffmpeg ./.venv/Scripts/python tools/contact_sheet.py "build/repro/$name/contact.png" build/repro/"$name"/frame_*.png >/dev/null
echo "build/repro/$name/contact.png"
