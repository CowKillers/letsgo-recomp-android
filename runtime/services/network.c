// Network services of a console that is offline (issue #1, from the second patch there): nifm (no internet), ssl,
// bsd:u (sockets) and ldn:u (local wireless, no network found). Before, sm handed the game a logging stub for them,
// which answers without the objects the game asks for (CreateGeneralService, CreateRequest...): the game used a null
// session and closed when opening local wireless, Mystery Gift or internet co-op. Now those screens open and wait;
// local wireless and the GO Park settings leave with B, the Mystery Gift / internet screens keep their spinner (the
// game polls RecvFrom with no timeout and no cancel there: nothing to download without Nintendo's servers).
// Command shapes as in switchbrew (nn::nifm IGeneralService / IRequest, nn::ldn, bsd).
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

// An event the game can wait on, signalled already: nothing will ever happen, so a wait on it ends at once.
static uint32_t signalled_event(void) {
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
    return r;
}

// ------------------------------------------------------------------ nifm:u
static void nifm_request(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;  // GetRequestState: free (done, not accepted)
    case 1: {  // GetResult: no internet connection (2110-0300); LARECOMP_NIFM_ERROR=<result> for experiments
        const char *e = getenv("LARECOMP_NIFM_ERROR");
        resp->result = e && *e ? (uint32_t)strtoul(e, NULL, 0) : MAKE_RESULT(110, 300);
        break;
    }
    case 2:  // GetSystemEventReadableHandles: two events
        resp->copy_handles[resp->ncopy++] = signalled_event();
        resp->copy_handles[resp->ncopy++] = signalled_event();
        break;
    case 4: break;  // Submit: done at once (GetRequestState)
    default: LOG("[nifm] IRequest cmd %u (ok)\n", req->cmd);
    }
}

static void nifm_general(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1: memset(resp->data, 0, 4); resp_size(resp, 4); break;                               // GetClientId
    case 4: ipc_return_object(resp, service_new("IRequest", nifm_request, NULL)); break;      // CreateRequest
    case 18: memset(resp->data, 0, 4); resp_size(resp, 4); break;                              // GetInternetConnectionStatus
    default: LOG("[nifm] IGeneralService cmd %u (ok)\n", req->cmd);
    }
}

SERVICE("nifm:u", nifm_u) {
    (void)s;
    if (req->cmd == 5) ipc_return_object(resp, service_new("IGeneralService", nifm_general, NULL));  // CreateGeneralService
    else LOG("[nifm] nifm:u cmd %u (ok)\n", req->cmd);
}

// ------------------------------------------------------------------ ssl (no TLS)
static void ssl_context(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    LOG("[ssl] ISslContext cmd %u (ok, no TLS)\n", req->cmd);
    memset(resp->data, 0, 0x10);
    resp_size(resp, 0x10);
}

SERVICE("ssl", ssl) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("ISslContext", ssl_context, NULL));  // CreateContext
    else LOG("[ssl] cmd %u (ok)\n", req->cmd);
}

// ------------------------------------------------------------------ bsd:u (sockets)
// Setting up succeeds; a new socket fails with ENETDOWN (the network is down) and a receive returns one zero byte.
SERVICE("bsd:u", bsd_u) {
    (void)s;
    switch (req->cmd) {
    case 2:  // Socket
    case 3:  // SocketExempt
        OUT(int32_t, 0) = -1;
        OUT(int32_t, 4) = 100;  // ENETDOWN (Linux errno values, switchbrew)
        resp_size(resp, 8);
        break;
    case 9:  // RecvFrom
        OUT(int32_t, 0) = 1;
        OUT(int32_t, 4) = 0;
        resp_size(resp, 8);
        break;
    default: LOG("[bsd] bsd:u cmd %u (ok)\n", req->cmd);
    }
}

// ------------------------------------------------------------------ ldn:u (local wireless)
typedef struct { bool init; } Ldn;

static void ldn_user(Service *s, IpcReq *req, IpcResp *resp) {
    Ldn *u = s->state;
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = u && u->init ? 1 : 0; resp_size(resp, 4); break;  // GetState: initialized or not
    case 100: resp->copy_handles[resp->ncopy++] = signalled_event(); break;       // GetStateChangeEvent
    case 102: OUT(uint16_t, 0) = 0; resp_size(resp, 4); break;                    // Scan: no network found
    case 400: if (u) u->init = true; break;                                        // Initialize
    case 401: if (u) u->init = false; break;                                       // Finalize
    default: LOG("[ldn] IUserLocalCommunicationService cmd %u (ok, no wireless)\n", req->cmd);
    }
}

SERVICE("ldn:u", ldn_u) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("IUserLocalCommunicationService", ldn_user, calloc(1, sizeof(Ldn))));
    else LOG("[ldn] ldn:u cmd %u (ok)\n", req->cmd);
}
