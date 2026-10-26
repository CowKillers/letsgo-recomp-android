// RenderDoc frame capture on demand (docs/PLANO-3D.md, fase 3). Run the game under RenderDoc
// (`renderdoccmd capture -w ./build/lasrecomp.exe ...`, see tools/repro.sh RENDERDOC=1) with
// LARECOMP_RENDERDOC_AT=<frame>: the frames after that one are captured into build/rdc/ (LARECOMP_RENDERDOC_FRAMES of
// them, default 4: the game doesn't draw its scene on every present). Without RenderDoc loaded this does nothing.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "../../rt.h"

// The parts of renderdoc_app.h (MIT, RenderDoc) used here: RENDERDOC_GetAPI and the 1.6.0 API table up to
// TriggerMultiFrameCapture.
typedef int (*pRENDERDOC_GetAPI)(int version, void **out);
typedef struct {
    void *GetAPIVersion, *SetCaptureOptionU32, *SetCaptureOptionF32, *GetCaptureOptionU32, *GetCaptureOptionF32,
        *SetFocusToggleKeys, *SetCaptureKeys, *GetOverlayBits, *MaskOverlayBits, *RemoveHooks, *UnloadCrashHandler;
    void (*SetCaptureFilePathTemplate)(const char *pathtemplate);
    void *GetCaptureFilePathTemplate, *GetNumCaptures, *GetCapture;
    void (*TriggerCapture)(void);
    void *IsTargetControlConnected, *LaunchReplayUI, *SetActiveWindow, *StartFrameCapture, *IsFrameCapturing,
        *EndFrameCapture;
    void (*TriggerMultiFrameCapture)(uint32_t frames);
} RenderDocApi;
enum { RENDERDOC_API_1_6_0 = 10600 };

void renderdoc_frame(uint64_t frame) {
    static int state;  // 0 not checked, 1 inactive, 2 armed
    static uint64_t at;
    static RenderDocApi *api;
    if (state == 1) return;
    if (state == 0) {
        state = 1;
        const char *e = getenv("LARECOMP_RENDERDOC_AT");
        if (!e) return;
        pRENDERDOC_GetAPI get = NULL;
#ifdef _WIN32
        HMODULE m = GetModuleHandleA("renderdoc.dll");
        if (m) get = (pRENDERDOC_GetAPI)(void *)GetProcAddress(m, "RENDERDOC_GetAPI");
#else
        void *m = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
        if (m) get = (pRENDERDOC_GetAPI)dlsym(m, "RENDERDOC_GetAPI");
#endif
        if (!get || !get(RENDERDOC_API_1_6_0, (void **)&api) || !api) {
            LOG("[renderdoc] LARECOMP_RENDERDOC_AT set but RenderDoc is not loaded (run under renderdoccmd capture)\n");
            return;
        }
        api->SetCaptureFilePathTemplate("build/rdc/capture");
        at = strtoull(e, NULL, 10);
        state = 2;
        LOG("[renderdoc] armed: capturing from frame %llu\n", (unsigned long long)at + 1);
    }
    if (frame == at) {
        const char *n = getenv("LARECOMP_RENDERDOC_FRAMES");
        api->TriggerMultiFrameCapture(n ? (uint32_t)atoi(n) : 4);
        LOG("[renderdoc] capture of %s frames triggered at frame %llu\n", n ? n : "4", (unsigned long long)frame);
        state = 1;
    }
}
