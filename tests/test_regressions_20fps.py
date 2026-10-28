"""The four 20 fps regressions measured on the Fold7 on 28/10 (t_8cc15f32), checked on the host: real product code, no
game, no Vulkan, no phone.

1. Battles / captures / menus at the world's 720p by default; an older file's 1080 (the old default) follows the world.
2. The world profile comes back < 1 s after a battle, even when the world updates at one present out of two.
3. The game stops at its present while the activity is in the background.
4. Diagnostic probes (pipeline_create.log, atlas dump, texture audit, dialog overrides) do nothing unless
   LARECOMP_DIAG=1 (Android: debug.larecomp.diag=1), and the launcher exports it from that property only.

Run: python3 -m unittest discover -s tests -p test_regressions_20fps.py -v
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
VULKAN = SRC / "runtime/nvn/vulkan"
JAVA = ROOT / "apk/java/com/letsgorecomp"


def clang():
    ndk = os.environ.get("ANDROID_NDK_HOME")
    cand = [ndk and f"{ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin/clang", "clang"]  # (cpu.h needs clang, not gcc)
    for c in cand:
        if c and shutil.which(c):
            return shutil.which(c)
    return None


def javac():
    home = os.environ.get("JAVA_HOME")
    for c in (home and f"{home}/bin/javac", "javac"):
        if c and shutil.which(c):
            return shutil.which(c)
    return None


def method(source, signature):
    """The full text of a Java method, from its signature to its closing brace."""
    start = source.index(signature)
    depth, i = 0, source.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(source[i], 0)
        i += 1
        if depth == 0:
            return source[start:i]


class Regressions20fps(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="lg-20fps-", dir=os.environ.get("TMPDIR")))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_c(self, args, env=None, cwd=None):
        cc = clang()
        self.assertIsNotNone(cc, "clang required (runtime/cpu.h uses its builtins)")
        exe = self.tmp / ("t" + str(abs(hash(tuple(args)))))
        build = subprocess.run([cc, "-D_GNU_SOURCE", "-I", str(SRC / "runtime"), *args, "-o", str(exe), "-lm", "-lpthread"],
                               capture_output=True, text=True)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        run = subprocess.run([str(exe)], capture_output=True, text=True, cwd=cwd or self.tmp, env=env, timeout=60)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        return run.stdout + run.stderr

    # 1 + 2 ---------------------------------------------------------------------------------------------------
    def test_profiles_default_upgrade_and_fast_world_switch(self):
        out = self.run_c([str(SRC / "tools/test_profile_switch.c")])
        for mark in ("DEFAULT_OK", "UPGRADE_OK", "SWITCH_OK"):
            self.assertIn(mark, out)
        self.assertIn("after_world_again=", out)  # the switch log says how long it took after the battle

    def test_existing_scene_profiles_still_pass(self):
        out = self.run_c([str(SRC / "tools/test_scene_profiles.c")])
        for mark in ("SCENE_OK", "SETTINGS_OK", "GUARD_OK", "LIVE_OK", "NOWARM_OK"):
            self.assertIn(mark, out)

    def test_launcher_shows_what_the_runtime_uses(self):
        jc = javac()
        self.assertIsNotNone(jc, "javac required (build/env.sh)")
        src = (JAVA / "LetsGoLauncherActivity.java").read_text(encoding="utf-8")
        body = method(src, "static String battleHeight(")
        (self.tmp / "B.java").write_text("public class B {\n" + body + """
            static void check(boolean ok, String what) { if (!ok) throw new AssertionError(what); }
            public static void main(String[] a) {
                check(battleHeight(null, null, null).equals("720"), "unset -> 720 (settings.c default)");
                check(battleHeight("1080", null, "720").equals("720"), "older file 1080 -> world");
                check(battleHeight("1080", null, "540").equals("540"), "older file 1080 -> world 540");
                check(battleHeight("1080", null, "90000").equals("720"), "world out of bounds -> 720");
                check(battleHeight("1080", "2", "720").equals("1080"), "1080 chosen since -> kept");
                check(battleHeight("900", null, "720").equals("900"), "an older non-default choice -> kept");
                System.out.println("LAUNCHER_OK");
            }
        }
        """, encoding="utf-8")
        build = subprocess.run([jc, "-d", str(self.tmp), str(self.tmp / "B.java")], capture_output=True, text=True)
        self.assertEqual(build.returncode, 0, build.stderr)
        java = str(Path(jc).with_name("java"))
        run = subprocess.run([java, "-cp", str(self.tmp), "B"], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('"720");  // (both at 720p by default', src)
        self.assertIn('kv.put("profiles_version", PROFILES_VERSION)', src)

    # 3 -------------------------------------------------------------------------------------------------------
    def test_background_stops_the_game(self):
        out = self.run_c([str(SRC / "tools/test_lifecycle.c"), str(SRC / "runtime/lifecycle.c")])
        self.assertIn("LIFECYCLE_OK", out)
        objects = (SRC / "runtime/nvn/objects.c").read_text()
        present = objects[objects.index("NVN(nvnQueuePresentTexture)"):]
        self.assertLess(present.index("rt_background_wait();"), present.index("present_pace("))
        act = (JAVA / "LetsGoActivity.java").read_text(encoding="utf-8")
        pause, resume = method(act, "protected void onPause()"), method(act, "protected void onResume()")
        self.assertIn("setBackground(true)", pause)
        self.assertIn("setBackground(false)", resume)
        self.assertIn("nativeSetBackground", (SRC / "runtime/platform/android.c").read_text())
        kernel = (SRC / "runtime/kernel.c").read_text()  # the other guest threads: at their SleepThread / yield
        sleep = kernel[kernel.index("case 0x0B: {  // SleepThread"):]
        self.assertLess(sleep.index("rt_background_wait();"), sleep.index("sched_yield()"))
        entry = kernel[kernel.index("static void svc_entry("):]
        self.assertLess(entry.index("rt_background_wait();"), entry.index("svc_impl("))  # every SVC (timed waits, polls)
        # the host clocks that wake the guest threads (vsync 60 Hz, audio renderer / audout) stop too
        vi = (SRC / "runtime/hle/vi.c").read_text()
        vsync = vi[vi.index("static void *vsync_thread"):]
        self.assertLess(vsync.index("rt_background_wait();"), vsync.index("kernel_signal(vsync_event)"))
        audio = (SRC / "runtime/services/audio.c").read_text()
        for fn, signal in (("static void *renderer_tick", "kernel_signal(r->event)"), ("static void *audout_thread", "SDL_PutAudioStreamData")):
            body = audio[audio.index(fn):]
            self.assertLess(body.index("rt_background_wait();"), body.index(signal), fn)

    # 4 -------------------------------------------------------------------------------------------------------
    def diag_native(self, body):
        """body runs under LARECOMP_DIAG = each value; EXPECTED_DIAG says whether it must be on."""
        cc = clang()
        self.assertIsNotNone(cc)
        c = self.tmp / "probe.c"
        c.write_text('#include <assert.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdatomic.h>\n'
                     '#include <errno.h>\n#include <sys/stat.h>\n#include <pthread.h>\n#include "diag.h"\n' + body)
        exe = self.tmp / "probe"
        build = subprocess.run([cc, "-D_GNU_SOURCE", "-Wall", "-Werror", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                                "-I", str(SRC / "runtime"), str(c), "-o", str(exe)], capture_output=True, text=True)
        self.assertEqual(build.returncode, 0, build.stderr)
        for value in (None, "", "0", "1", "2", "true", " 1", "1 "):
            with self.subTest(diag=value):
                d = self.tmp / ("run_" + ("unset" if value is None else value.encode().hex()))
                d.mkdir()
                env = {k: v for k, v in os.environ.items() if not k.startswith("LARECOMP_")}
                if value is not None:
                    env["LARECOMP_DIAG"] = value
                env["EXPECTED_DIAG"] = "1" if value == "1" else "0"
                env["LARECOMP_DATA_ROOT"] = str(d)
                run = subprocess.run([str(exe)], cwd=d, env=env, capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def segment(self, filename, start, end):
        s = (VULKAN / filename).read_text()
        i = s.index(start)
        return s[i:s.index(end, i)]

    def test_dialog_overrides_only_with_diag(self):
        mask = self.segment("exec.c", "    // v0086: dialog text produces", "    // v0088: a0 binding diagnostic")
        cull = self.segment("exec.c", "    // v0085: dialog text draws", "    if (exp_hit && exp_mode == 1)")
        self.diag_native('''
#define LOG(...) ((void)0)
#define VK_CULL_MODE_NONE 0
#define VK_FALSE 0
static int cull = 7, depth = 1;
static void vkCmdSetCullMode(int cb, int m) { (void)cb; cull = m; }
static void vkCmdSetDepthTestEnable(int cb, int m) { (void)cb; depth = m; }
static uint64_t hmix(uint64_t h, const void *p, int n) { (void)h; (void)p; (void)n; return 0x6443b63b341c73caULL; }
int main(void) {
    struct { unsigned ncolor; unsigned write_mask[8]; } k = {.ncolor = 1};
    struct { unsigned out_mask; } fi = {0};
    struct { unsigned channel_mask; } st = {0};
    struct { uint64_t code[2]; __typeof__(fi) *fi; } prog = {.code = {0, 1}, .fi = &fi}, *pr = &prog;
    uint64_t g_text_fshash = 0x6443b63b341c73caULL;
    int cb = 0;
''' + mask + cull + '''
    int on = atoi(getenv("EXPECTED_DIAG"));
    assert(k.write_mask[0] == (on ? 15u : 0u));
    assert(cull == (on ? 0 : 7) && depth == (on ? 0 : 1));
    return 0;
}
''')

    def test_atlas_scan_and_dump_only_with_diag(self):
        scan = self.segment("textures.c", "    // Text audit: for small textures", "    if (t->flags & 0x10)")
        self.diag_native('''
#define LOG(...) ((void)0)
int main(void) {
    static uint8_t pixels[9010]; memset(pixels, 1, sizeof pixels);
    struct { int bpb; } fmt = {1}, *fi = &fmt;
    struct { uintptr_t addr; unsigned width, height, format; } tex = {(uintptr_t)pixels, 2048, 1024, 1}, *t = &tex;
    struct { int uploaded, dynamic; } be = {0}, *b = &be;
    uint64_t frame = 0, total = 9000;
    for (int i = 0; i < 6; ++i) {
        total++;
''' + scan + '''
    }
    FILE *f = fopen("atlas_dump.raw", "rb");
    assert((f != NULL) == atoi(getenv("EXPECTED_DIAG")));
    if (f) fclose(f);
    return 0;
}
''')

    def test_pipeline_log_only_with_diag(self):
        s = (VULKAN / "exec.c").read_text()
        begin = self.segment("exec.c", "    static _Atomic uint64_t pipe_attempt_id", "    // v0074: bypass")
        end_start = s.index("\n", s.index("    VkResult r = vkCreateGraphicsPipelines")) + 1
        end = s[end_start:s.index("    atomic_fetch_add(&stat_pipe_ns", end_start)]
        self.diag_native('''
#define NDYN 4
static uint64_t SDL_GetTicksNS(void) { return 17; }
static uint64_t hmix(uint64_t h, const void *p, int n) { (void)p; (void)n; return h; }
int main(void) {
    struct { uint64_t code[2]; } prog = {{0, 0}};
    struct { __typeof__(prog) *prog; unsigned ncolor, color[8], depth, stencil, samples, topo_class, blend_on; } key = {.prog = &prog}, *k = &key;
    struct { int cullMode; } rs = {0};
    struct { void *pcache; } g_vk = {NULL};
    uint64_t tp = 3;
    int r = 0;
''' + begin + end + '''
    FILE *f = fopen("pipeline_create.log", "rb");
    assert((f != NULL) == atoi(getenv("EXPECTED_DIAG")));
    if (f) fclose(f);
    return 0;
}
''')

    def test_every_listed_probe_is_behind_the_switch(self):
        exec_c = (VULKAN / "exec.c").read_text()
        tex = (VULKAN / "textures.c").read_text()
        sh = (VULKAN / "shaders.c").read_text()
        self.assertIn("if (rt_diag() && (is_font_atlas || is_small_font))", exec_c)            # [font-bind]
        self.assertIn("if (rt_diag() && atomic_load(&g_font_atlas_bound))", exec_c)           # [font-draw/geo/state], capture_font
        self.assertEqual(len(re.findall(r"if \(rt_diag\(\)\) \{\n        uint64_t fs_fsh_(?:wm|a0|cb3)", exec_c)), 3)  # v0086 v0088 v0089
        self.assertIn("if (rt_diag() && g_text_fshash == 0x6443b63b341c73caULL) {\n            static int sc_logged", exec_c)
        self.assertIn("if (rt_diag() && fi->bpb <= 2 && !b->uploaded)", tex)                  # [tex-audit] first upload
        self.assertIn("if (rt_diag() && nvn_hash)", sh)                                       # [shader] NVN2GLSL
        for f in (exec_c, tex, sh):
            self.assertIn('#include "../../diag.h"', f)

    def test_launcher_exports_the_switch_from_the_property_only(self):
        act = (JAVA / "LetsGoActivity.java").read_text(encoding="utf-8")
        self.assertNotIn('System.getenv("LARECOMP_DIAG")', act)   # an environment left by an earlier launch never counts
        self.assertIn('Os.setenv("LARECOMP_DIAG", "1", true)', act)
        self.assertIn('Os.unsetenv("LARECOMP_DIAG")', act)
        jc = javac()
        self.assertIsNotNone(jc)
        (self.tmp / "D.java").write_text("public class D {\n" + method(act, "static boolean diagEnabled(") + """
            public static void main(String[] a) {
                String[] off = {null, "", "0", "true", "2", "01"};
                for (String v : off) if (diagEnabled(v)) throw new AssertionError(String.valueOf(v));
                if (!diagEnabled("1") || !diagEnabled("1\\n")) throw new AssertionError("1");
                System.out.println("DIAG_OK");
            }
        }
        """, encoding="utf-8")
        build = subprocess.run([jc, "-d", str(self.tmp), str(self.tmp / "D.java")], capture_output=True, text=True)
        self.assertEqual(build.returncode, 0, build.stderr)
        run = subprocess.run([str(Path(jc).with_name("java")), "-cp", str(self.tmp), "D"], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
