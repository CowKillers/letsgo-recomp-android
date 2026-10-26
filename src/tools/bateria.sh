#!/usr/bin/env bash
# bateria.sh <eevee|pikachu|ambos> <mundo|menus|saida|todas> [VAR=valor ...]: the regression battery (docs/DEPURACAO.md).
#
# Each route is a fixed input script from the same save (a copy of build/user-save: Pewter City), run in turbo (same
# frames every run). The frames go straight to ONE raw file (LARECOMP_SHOT_RAW, 480x270 RGB per frame, no PNG, no ffmpeg),
# the routes run in parallel (own save copy and output folder each) and stop by themselves at the route's last frame.
# Per route, in build/bateria/<TAG>/<jogo>/<rota>[-rN]/:  frames.raw  log.txt  frametimes.csv  blink.txt
# and one line per route in build/bateria/resumo.txt: blinks, Vulkan validation errors (VALIDACAO=1), unimplemented HLE
# calls, skipped draws, crashes.
#
# Environment:  TAG=<name>   output folder (default "run");  REPS=<n>  repeats of each route (default 1; the blink count
#               and frame differences between repeats are the run-to-run noise, see tools/bateria_scan.py noise)
#               PAR=<n>    routes at once (default 6; the frames do not depend on it, the frametimes do: for
#               performance use tools/perf_ab.sh, one run at a time);  FPS60=1 / WILD=1  the experimental modes
#               VALIDACAO=1  Vulkan validation layers;  TIMEOUT=<s>  per route (default 300)
#               EXE=<path>  another runtime build (default build/lasrecomp.exe), e.g. a baseline to compare against
set -uo pipefail
cd "$(dirname "$0")/.."
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
jogo=${1:?jogo: eevee, pikachu ou ambos}
rota=${2:?rota: mundo, menus, saida ou todas}
shift 2
TAG=${BAT_TAG:-${TAG:-run}}; REPS=${REPS:-1}; PAR=${PAR:-6}; TIMEOUT=${TIMEOUT:-300}

base="400:A:6,500:A:6,1150:A:6,1300:A:6"   # controller screen, title, continue (the world from ~1350)
declare -A IN FROM TO
# walking around Pewter City (away from the rock wall on the left: walking into it makes the player shake) and the camera
IN[mundo]="$base,1400:STICK_RIGHT:150,1560:STICK_DOWN:120,1690:STICK_RIGHT:120,1820:STICK_UP:150,1980:STICK_LEFT:100,2090:RSTICK_LEFT:90,2190:RSTICK_RIGHT:90"
FROM[mundo]=1400; TO[mundo]=2300
# the menu, the Pokédex (scrolling), the bag and back
IN[menus]="$base,1450:X:6,1520:LEFT:4,1545:LEFT:4,1570:A:6,1700:DOWN:40,1760:DOWN:40,1820:UP:20,1880:B:6,1960:RIGHT:4,1985:A:6,2080:DOWN:20,2140:B:6,2220:B:6"
FROM[menus]=1440; TO[menus]=2300
# south, toward the way out of the city (an area change: loading, new models and shaders)
IN[saida]="$base,1400:STICK_DOWN:700"
FROM[saida]=1400; TO[saida]=2200

case "$jogo" in ambos) jogos="eevee pikachu" ;; eevee|pikachu) jogos=$jogo ;; *) echo "jogo: $jogo?"; exit 2 ;; esac
case "$rota" in todas) rotas="mundo menus saida" ;; *) [ -n "${IN[$rota]:-}" ] || { echo "rota: $rota?"; exit 2; }; rotas=$rota ;; esac
[ -f build/user-save/savedata.bin ] || { echo "falta build/user-save/savedata.bin (cópia do save)"; exit 2; }
mkdir -p build/bateria
# fixed settings, not the developer's larecomp.ini (fps = 60 there would put the menus' in-between images in the frames)
{
    echo "fps = $([ "${FPS60:-0}" = 1 ] && echo 60 || echo 30)"
    echo "wild_battle = ${WILD:-0}"
    echo "render_height = 0"
    echo "resolution = match"
    echo "smaa = 1"
    echo "show_fps = 0"
} > build/bateria/larecomp.ini
EXTRA=("$@")
PYTHON=./.venv/Scripts/python; [ -x "$PYTHON" ] || PYTHON=python

job() {  # job <jogo> <rota> <rep>
    local j=$1 r=$2 rep=$3 name=$2 root=extracted lib=
    [ "$REPS" -gt 1 ] && name="$2-r$3"
    [ "$j" = pikachu ] && root=extracted-pikachu && lib=$(cygpath -w "$PWD/extracted-pikachu/lasgame.dll")
    [ "$j" = eevee ] && [ -n "${EXE:-}" ] && lib=$(cygpath -w "$PWD/build/lasgame.dll")  # (found next to the default exe)
    [ -d "$root/exefs" ] || { echo "$j: falta $root/"; return; }
    local out="build/bateria/$TAG/$j/$name" sav="build/bateria/$TAG/$j/$name/save"
    rm -rf "$out"; mkdir -p "$sav/save_0"
    cp build/user-save/savedata.bin "$sav/save_0/"
    local val=()
    [ "${VALIDACAO:-0}" = 1 ] && val=(LARECOMP_VK_VALIDATION=1 VK_ADD_LAYER_PATH='C:\msys64\ucrt64\bin')
    local t0=$(date +%s)
    (
        export LARECOMP_FPS=0 LARECOMP_NO_AUDIO=1 LARECOMP_NO_USER_INPUT=1 LARECOMP_NO_LAUNCHER=1 \
            LARECOMP_SAVE_DIR="$sav" LARECOMP_SCALE=0.5 LARECOMP_INPUT="${IN[$r]}" \
            LARECOMP_SCREENSHOT_EVERY=1 LARECOMP_SCREENSHOT_FROM="${FROM[$r]}" \
            LARECOMP_SHOT_RAW="$out/frames.raw" LARECOMP_SHOT_RAW_LAST="$((TO[$r] - 1))" \
            LARECOMP_FRAMETIME_CSV="$out/frametimes.csv" LARECOMP_SETTINGS=build/bateria/larecomp.ini
        [ -n "$lib" ] && export LARECOMP_GAME_LIB="$lib"
        for kv in "${val[@]}" "${EXTRA[@]}"; do export "$kv"; done
        exec "${EXE:-./build/lasrecomp.exe}" "$root" --play > "$out/log.txt" 2>&1
    ) &
    local pid=$! timed_out=1
    # Done when the game says it wrote the route's last frame (and left), on a crash, or at the timeout. Not by the pid:
    # the runtime may re-execute itself (runtime/main.c reserve_guest_space) and then runs outside this subshell.
    for _ in $(seq "$TIMEOUT"); do
        grep -q "raw frames done" "$out/log.txt" 2>/dev/null && { timed_out=0; break; }
        grep -qE "FATAL|exception c0" "$out/log.txt" 2>/dev/null && { timed_out=0; break; }
        sleep 1
    done
    [ "$timed_out" = 1 ] && echo "[bateria] timeout" >> "$out/log.txt"
    sleep 1
    local gp=$(grep -o 'raw frames -> [^ ]* (pid [0-9]*' "$out/log.txt" | grep -o '[0-9]*$' | tail -1)
    [ -n "$gp" ] && taskkill //F //T //PID "$gp" >/dev/null 2>&1
    local winpid=$(cat "/proc/$pid/winpid" 2>/dev/null || true)
    [ -n "$winpid" ] && taskkill //F //T //PID "$winpid" >/dev/null 2>&1
    wait "$pid" 2>/dev/null
    local n=0 blinks=0
    if [ -s "$out/frames.raw" ]; then
        n=$(( $(stat -c %s "$out/frames.raw") / (4 + 480 * 270 * 3) ))
        $PYTHON tools/bateria_scan.py blink "$out/frames.raw" "${FROM[$r]}" "$((TO[$r] - 1))" > "$out/blink.txt" 2>&1
        blinks=$(awk '/^STRONG/ {print $2}' "$out/blink.txt")
    fi
    local L="$out/log.txt"
    printf '%-8s %-9s %4ss  frames %4s  blinks %3s  vkerr %4s  unimpl %3s  skipped %5s  crash %s  [%s]\n' "$j" "$name" \
        $(( $(date +%s) - t0 )) "$n" "${blinks:-?}" \
        "$(grep -c 'Validation Error' "$L")" \
        "$(grep -c '\[hle\] UNIMPLEMENTED' "$L")" \
        "$(grep -o 'skipped [0-9]*' "$L" | awk '{s+=$2} END {print s+0}')" \
        "$(grep -qE 'exception|FATAL|\[bateria\] timeout' "$L" && echo SIM || echo nao)" "$TAG" | tee -a build/bateria/resumo.txt
    rm -rf "$sav"
}

t_all=$(date +%s)
for rep in $(seq "$REPS"); do
    for j in $jogos; do
        for r in $rotas; do
            while [ "$(jobs -rp | wc -l)" -ge "$PAR" ]; do sleep 1; done
            job "$j" "$r" "$rep" &
            sleep 3   # (the start-up probe that checks the folder is writable collides when two start together)
        done
    done
done
wait
echo "[bateria] total $(( $(date +%s) - t_all ))s"
