// mii:e / mii:u: the console's Mii database. There is none here, so the database is empty: no Miis, never updated,
// not full. A game that makes its own Miis (Tomodachi Life) only needs the service to open; what it would read is
// logged, so a game that needs more shows it.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

static void database(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;   // IsUpdated(source): no
    case 1: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;   // IsFullDatabase: no
    case 2: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // GetCount(source): no Miis
    case 3: case 4: case 8: case 9: {                          // Get / Get1 / Get2 / Get3: none listed
        IpcBuf *b = out_buf(req, 0);
        if (b) memset((void *)(uintptr_t)b->addr, 0, b->size);
        OUT(uint32_t, 0) = 0;
        resp_size(resp, 4);
        break;
    }
    case 22: break;  // SetInterfaceVersion
    default: LOG("[mii] IDatabaseService cmd %u (ok, zeroes)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void static_service(Service *s, IpcReq *req, IpcResp *resp) {
    switch (req->cmd) {
    case 0: ipc_return_object(resp, service_new("IDatabaseService", database, NULL)); break;  // GetDatabaseService
    default: LOG("[mii] %s cmd %u (ok)\n", s->name, req->cmd); resp_size(resp, 0x10);
    }
}

SERVICE("mii:e", mii_e) { static_service(s, req, resp); }
SERVICE("mii:u", mii_u) { static_service(s, req, resp); }
