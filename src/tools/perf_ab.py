"""perf_ab.py: interleaved A/B performance test, one run at a time (the GPU time of the same frame varies 10-18 ms between
runs, so only runs taken in turn mean anything).

  perf_ab.py <eevee|pikachu> [--runs N] [--mode turbo|60] [--route mundo] A=VAR=1,VAR2=x B=VAR=2 ...

Each variant is NAME=VAR=value[,VAR=value...] (NAME= alone: no variables). EXE=<path> in a variant runs that runtime
build instead of build/lasrecomp.exe (with LARECOMP_GAME_LIB=<its lasgame.dll> when the game code differs too). Runs A, B, A, B... (N each), each a scripted
route from a copy of the save (tools/bateria.sh's "mundo": walking Pewter City), unlimited frame rate (turbo) or the
paced 60 fps mode, until present LARECOMP_EXIT_AT_PRESENT. Per run: median and p1/p99 frame time over the walking window;
then per variant the median of the runs' medians with min-max, and for two variants the ratio with a bootstrap 95%
interval over the runs. A ratio whose interval contains 1.0 is not a result.
"""
import argparse
import os
import random
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = "400:A:6,500:A:6,1150:A:6,1300:A:6"
ROUTE = BASE + ",1400:STICK_RIGHT:150,1560:STICK_DOWN:120,1690:STICK_RIGHT:120,1820:STICK_UP:150,1980:STICK_LEFT:100,2090:RSTICK_LEFT:90,2190:RSTICK_RIGHT:90"
FROM, TO = 1420, 2300


def run_once(game, mode, env_extra, tag):
    out = os.path.join(ROOT, "build", "perf", tag)
    os.makedirs(out, exist_ok=True)
    sav = os.path.join(out, "save")
    os.makedirs(os.path.join(sav, "save_0"), exist_ok=True)
    with open(os.path.join(ROOT, "build", "user-save", "savedata.bin"), "rb") as f, open(os.path.join(sav, "save_0", "savedata.bin"), "wb") as g:
        g.write(f.read())
    ini = os.path.join(out, "larecomp.ini")
    with open(ini, "w") as f:
        f.write(f"fps = {60 if mode == '60' else 30}\nwild_battle = 0\nrender_height = 0\nresolution = match\nsmaa = 1\nshow_fps = 0\n")
    csv = os.path.join(out, "frametimes.csv")
    env = dict(os.environ, LARECOMP_NO_AUDIO="1", LARECOMP_NO_USER_INPUT="1", LARECOMP_NO_LAUNCHER="1", LARECOMP_SAVE_DIR=sav,
               LARECOMP_INPUT=ROUTE, LARECOMP_FRAMETIME_CSV=csv, LARECOMP_SETTINGS=ini, LARECOMP_EXIT_AT_PRESENT=str(TO))
    if mode == "turbo":
        env["LARECOMP_FPS"] = "0"
    root = "extracted"
    if game == "pikachu":
        root = "extracted-pikachu"
        env["LARECOMP_GAME_LIB"] = os.path.join(ROOT, "extracted-pikachu", "lasgame.dll")
    env["PATH"] = "C:/msys64/ucrt64/bin;C:/msys64/usr/bin;" + env.get("PATH", "")  # (the runtime's DLLs: SDL3, ffmpeg...)
    env.update(env_extra)
    exe = env.pop("EXE", os.path.join(ROOT, "build", "lasrecomp.exe"))
    log = os.path.join(out, "log.txt")
    with open(log, "w") as lf:
        p = subprocess.Popen([exe, root, "--play"], cwd=ROOT, env=env, stdout=lf, stderr=subprocess.STDOUT)
        t0 = time.time()
        done = False
        while time.time() - t0 < 400:
            time.sleep(1)
            with open(log, errors="replace") as r:
                txt = r.read()
            if "exit at present" in txt or "FATAL" in txt:
                done = True
                break
        time.sleep(1)
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True)
        for line in txt.splitlines():  # (the re-executed game: its pid is in no parent's tree)
            pass
    if not done:
        return None
    dts = []
    with open(csv) as f:
        next(f)
        for line in f:
            n, d = line.split(",")
            if FROM <= int(n) <= TO:
                dts.append(float(d))
    if len(dts) < 100:
        return None
    s = sorted(dts)
    pct = lambda q: s[min(len(s) - 1, int(len(s) * q))]
    return {"med": statistics.median(dts), "p1": pct(0.01), "p99": pct(0.99), "n": len(dts)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", choices=["eevee", "pikachu"])
    ap.add_argument("variants", nargs="+")
    ap.add_argument("--runs", type=int, default=4)
    ap.add_argument("--mode", choices=["turbo", "60"], default="turbo")
    a = ap.parse_args()
    variants = []
    for v in a.variants:
        name, _, rest = v.partition("=")
        env = dict(kv.split("=", 1) for kv in rest.split(",") if kv)
        variants.append((name, env))
    res = {n: [] for n, _ in variants}
    for i in range(a.runs):
        for name, env in variants:
            r = run_once(a.game, a.mode, env, f"{a.game}-{name}")
            if r is None:
                print(f"run {i + 1} {name}: failed")
                continue
            res[name].append(r)
            print(f"run {i + 1} {name:8s} median {r['med']:.2f} ms ({1000 / r['med']:.1f} fps)  p1 {r['p1']:.2f}  p99 {r['p99']:.2f}", flush=True)
    meds = {}
    for name, rs in res.items():
        if not rs:
            continue
        m = [r["med"] for r in rs]
        meds[name] = m
        print(f"{name:8s} runs {len(m)}  median of medians {statistics.median(m):.2f} ms ({1000 / statistics.median(m):.1f} fps)"
              f"  range {min(m):.2f}-{max(m):.2f}  p99 {statistics.median(r['p99'] for r in rs):.2f}")
    names = list(meds)
    if len(names) >= 2:
        x, y = meds[names[0]], meds[names[1]]
        rng = random.Random(1)
        ratios = sorted(statistics.median(rng.choices(x, k=len(x))) / statistics.median(rng.choices(y, k=len(y))) for _ in range(4000))
        lo, hi = ratios[int(len(ratios) * 0.025)], ratios[int(len(ratios) * 0.975)]
        print(f"{names[0]} / {names[1]} frame time ratio: {statistics.median(x) / statistics.median(y):.3f}  95% [{lo:.3f}, {hi:.3f}]"
              f"  -> {'significant' if lo > 1 or hi < 1 else 'NOT significant (interval contains 1)'}")


if __name__ == "__main__":
    main()
