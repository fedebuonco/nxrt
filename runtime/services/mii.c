// mii:e / mii:u: the console's Mii database. There is none here, so the database is empty: no Miis, never updated,
// not full; the default Miis are built (a game that makes its own Miis, Tomodachi Life, starts from one). What else a
// game asks is logged, so a game that needs more shows it.
#include <pthread.h>
#include <string.h>

#include "../hle.h"
#include "../rt.h"
#include "../ipc.h"

// The console's default Miis, built by the game's own SDK: nn::mii::detail::CharInfoRaw::SetDefaultMii(CharInfoRaw*,
// int index, CreateId (by value: x2, x3), FavoriteColor, const Nickname*), from its table of default Miis - every
// field as nn::mii checks it (some may not be 0). Called on the thread asking, inside its svc: its registers are kept.
static bool sdk_default_mii(uint8_t *out, uint32_t index, const uint8_t *id, const uint8_t *nick) {
    static uint64_t fn;
    static bool looked;
    static uint8_t *buf;  // guest-visible: the CharInfo (0x58) then the nickname (0x16)
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    Cpu *c = kernel_current_cpu();
    pthread_mutex_lock(&lock);
    if (!looked) {
        looked = true;
        fn = loader_find_symbol("_ZN2nn3mii6detail11CharInfoRaw13SetDefaultMiiEiNS0_8CreateIdENS0_13FavoriteColorENS0_8NicknameE");
        if (fn) buf = hle_alloc(0x80);
        LOG("[mii] default Miis: %s\n", fn ? "the SDK's SetDefaultMii" : "not in this SDK (fields 0)");
    }
    bool ok = fn && c;
    if (ok) {
        memset(buf, 0, 0x80);
        memcpy(buf + 0x58, nick, 0x16);
        Cpu saved = *c;
        c->x[0] = (uint64_t)(uintptr_t)buf;
        c->x[1] = index;
        memcpy(&c->x[2], id, 16);
        c->x[4] = 0;  // FavoriteColor: red
        c->x[5] = (uint64_t)(uintptr_t)(buf + 0x58);
        guest_call(c, fn);
        *c = saved;
        memcpy(out, buf, 0x58);
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

// A built Mii (BuildDefault / BuildRandom): a CharInfo (0x58 bytes) with a create id that has the UUID variant bits
// and the nickname a default Mii has on the console ("no name"); its fields are the SDK's default Mii.
static void built_mii(IpcResp *resp, uint32_t index) {
    uint8_t *m = resp->data;
    static const uint8_t id0[16] = {0x6e, 0x78, 0x72, 0x74, 0x6d, 0x69, 0x40, 0x00, 0x80, 0, 0, 0, 0, 0, 0, 0};
    uint8_t id[16], nick[0x16] = {0};
    memcpy(id, id0, 16);
    id[15] = (uint8_t)(index + 1);  // (one id per default Mii)
    static const char name[] = "no name";
    for (int i = 0; name[i]; i++) nick[2 * i] = (uint8_t)name[i];  // UTF-16LE, 10 characters at most
    if (!sdk_default_mii(m, index, id, nick)) {
        memset(m, 0, 0x58);
        memcpy(m, id, 16);
        memcpy(m + 0x10, nick, 0x16);
        m[0x28] = index >= 6;  // gender (0x26 font region, 0x27 favorite color: 0)
        m[0x29] = 64;          // height
        m[0x2a] = 64;          // build
    }
    resp_size(resp, 0x58);
}

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
    case 6: built_mii(resp, 0); break;               // BuildRandom(age, gender, race): the first default Mii
    case 7: built_mii(resp, IN(uint32_t, 0)); break;  // BuildDefault(index)
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
