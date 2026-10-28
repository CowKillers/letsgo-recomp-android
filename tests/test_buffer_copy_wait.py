"""CMD_COPY_BUFFER_TO_BUFFER (exec.c): wait for the GPU only when it may touch the game's memory in place.

The case body is compiled as-is into a host harness with the backend stubbed: no GPU, phone or game data.
Without the heap import (Adreno) the copy must not wait and must drop the shared copies after the memmove;
with the import, or LARECOMP_VK_COPY_WAIT=1, it waits first as before (t_68ff7bf4).
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXEC = ROOT / 'src/runtime/nvn/vulkan/exec.c'

def case_body():
    src = EXEC.read_text()
    m = re.search(r'case CMD_COPY_BUFFER_TO_BUFFER: \{\n(.*?)\n            break;\n        \}', src, re.S)
    assert m, 'CMD_COPY_BUFFER_TO_BUFFER case not found'
    return m.group(1)

HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct { bool host_import; } g_vk;
static int flushes, copy_flushes, flush_saw_old;
static unsigned char src_buf[16] = "new bytes", dst_buf[16] = "old bytes";
static void renderer_flush(void) { flushes++; if (!strcmp((char *)dst_buf, "old bytes")) flush_saw_old++; }
static void guest_copy_flush(void) { copy_flushes++; if (strcmp((char *)dst_buf, "new bytes")) abort(); }
typedef struct { uint64_t x[4]; } CmdRec;
static void copy(CmdRec *r) {
@BODY@
}
int main(int argc, char **argv) {
    g_vk.host_import = argc > 1 && argv[1][0] == '1';
    CmdRec r = {{(uint64_t)(uintptr_t)src_buf, (uint64_t)(uintptr_t)dst_buf, sizeof src_buf}};
    copy(&r);
    printf("%d %d %d %s\n", flushes, copy_flushes, flush_saw_old, (char *)dst_buf);
    return 0;
}
"""

@unittest.skipUnless(shutil.which('cc'), 'no host C compiler')
class BufferCopyWait(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        c = Path(cls.tmp.name) / 'b2b.c'
        c.write_text(HARNESS.replace('@BODY@', case_body()))
        cls.exe = Path(cls.tmp.name) / 'b2b'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-o', str(cls.exe), str(c)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, host_import, env_wait=None):
        env = {k: v for k, v in os.environ.items() if k != 'LARECOMP_VK_COPY_WAIT'}
        if env_wait is not None:
            env['LARECOMP_VK_COPY_WAIT'] = env_wait
        out = subprocess.run([str(self.exe), '1' if host_import else '0'], env=env, check=True,
                             capture_output=True, text=True).stdout.split()
        return int(out[0]), int(out[1]), int(out[2]), out[3] + ' ' + out[4]

    def test_no_heap_import_copies_without_waiting(self):
        self.assertEqual(self.run_case(False), (0, 1, 0, 'new bytes'))

    def test_heap_import_waits_before_the_copy(self):
        self.assertEqual(self.run_case(True), (1, 1, 1, 'new bytes'))

    def test_env_restores_the_old_wait(self):
        self.assertEqual(self.run_case(False, '1'), (1, 1, 1, 'new bytes'))
        self.assertEqual(self.run_case(False, '0'), (0, 1, 0, 'new bytes'))

if __name__ == '__main__':
    unittest.main()
