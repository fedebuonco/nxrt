// The lifter's differential test (tools/test_lift.py builds this with the generated tests.c): every case runs one guest
// instruction twice from the same state - as its lifted C on a Cpu, and as itself on this Mac's own CPU (Apple
// Silicon is AArch64: the reference is the hardware) - then compares x0-x30, sp, nzcv, v0-v31 and a memory buffer.
//
// The native run: a stub made at run time (MAP_JIT) loads every register from a state block, runs the instruction,
// stores every register back. It keeps the block's address in one register R the instruction doesn't name (chosen per
// case by test_lift.py); R and x18 (reserved on macOS) are neither loaded nor compared. A fault in either run (a bad
// address the inputs didn't foresee) skips the case.
//
// usage: harness <cases.bin>   (stdout: one line per case: "<case> ok" / "<case> skip <why>" / "<case> diff <what>")
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "cpu.h"

extern GuestFn tests[];
extern const uint32_t test_words[];
extern int ntests;

// what the lifted code may call (cpu.h): none of the tested instructions should reach these
void rt_dispatch(Cpu *c) { (void)c; abort(); }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)c, (void)imm, (void)pc; abort(); }
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) { (void)c, (void)imm, (void)pc; abort(); }
void rt_udf(Cpu *c, uint64_t pc) { (void)c, (void)pc; abort(); }
uint64_t rt_ticks(void) { return 0; }
volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0) { (void)a, (void)lr, (void)x0; }

#define BUF ((uint8_t *)0x7200000000ull)  // (test_lift.py points address registers here)
#define BUF_SIZE 0x40000

// the state block: Cpu's layout (x, sp, nzcv, v), then the host's callee-saved registers while the stub runs
enum { S_X = 0, S_SP = 256, S_NZCV = 272, S_V = 512, S_SAVE = 1024, S_SAVE_SP = 1024 + 12 * 8, S_SAVE_D = 1136 };
typedef struct __attribute__((aligned(16))) { uint8_t b[2048]; } State;

static uint32_t ldr_x(int t, int n, int off) { return 0xF9400000u | (uint32_t)(off / 8) << 10 | (uint32_t)n << 5 | (uint32_t)t; }
static uint32_t str_x(int t, int n, int off) { return 0xF9000000u | (uint32_t)(off / 8) << 10 | (uint32_t)n << 5 | (uint32_t)t; }
static uint32_t ldr_q(int t, int n, int off) { return 0x3DC00000u | (uint32_t)(off / 16) << 10 | (uint32_t)n << 5 | (uint32_t)t; }
static uint32_t str_q(int t, int n, int off) { return 0x3D800000u | (uint32_t)(off / 16) << 10 | (uint32_t)n << 5 | (uint32_t)t; }
static uint32_t add0(int d, int n) { return 0x91000000u | (uint32_t)n << 5 | (uint32_t)d; }  // mov d, n (31 = sp)
static uint32_t mov_x(int d, int m) { return 0xAA0003E0u | (uint32_t)m << 16 | (uint32_t)d; }
static uint32_t msr_nzcv(int t) { return 0xD51B4200u | (uint32_t)t; }
static uint32_t mrs_nzcv(int t) { return 0xD53B4200u | (uint32_t)t; }

static uint32_t *g_code;
enum { CODE_WORDS = 1024 };

// the stub for one instruction w, the block's address kept in register r
static void make_stub(uint32_t w, int r) {
    uint32_t c[CODE_WORDS];
    int n = 0;
    for (int i = 19; i <= 30; i++) c[n++] = str_x(i, 0, S_SAVE + (i - 19) * 8);  // (x0 = the block, as called)
    for (int i = 8; i <= 15; i++) c[n++] = str_q(i, 0, S_SAVE_D + (i - 8) * 16);
    c[n++] = add0(1, 31), c[n++] = str_x(1, 0, S_SAVE_SP);
    if (r) c[n++] = mov_x(r, 0);
    int t = r == 0 ? 1 : 0;  // (a temporary until the registers are loaded)
    c[n++] = ldr_x(t, r, S_NZCV), c[n++] = msr_nzcv(t);
    c[n++] = ldr_x(t, r, S_SP), c[n++] = add0(31, t);
    for (int i = 0; i < 32; i++) c[n++] = ldr_q(i, r, S_V + i * 16);
    for (int i = 0; i <= 30; i++)
        if (i != r && i != 18) c[n++] = ldr_x(i, r, S_X + i * 8);
    c[n++] = w;
    for (int i = 0; i <= 30; i++)
        if (i != r && i != 18) c[n++] = str_x(i, r, S_X + i * 8);
    for (int i = 0; i < 32; i++) c[n++] = str_q(i, r, S_V + i * 16);
    c[n++] = mrs_nzcv(t), c[n++] = str_x(t, r, S_NZCV);
    c[n++] = add0(t, 31), c[n++] = str_x(t, r, S_SP);
    c[n++] = ldr_x(t, r, S_SAVE_SP), c[n++] = add0(31, t);
    if (r) c[n++] = mov_x(0, r);
    for (int i = 8; i <= 15; i++) c[n++] = ldr_q(i, 0, S_SAVE_D + (i - 8) * 16);
    for (int i = 19; i <= 30; i++) c[n++] = ldr_x(i, 0, S_SAVE + (i - 19) * 8);
    c[n++] = 0xD65F03C0u;  // ret
    pthread_jit_write_protect_np(0);
    memcpy(g_code, c, (size_t)n * 4);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(g_code, (size_t)n * 4);
}

static sigjmp_buf g_jmp;
static volatile int g_in_run;
static void on_fault(int sig) {
    if (g_in_run) siglongjmp(g_jmp, sig);
    struct sigaction sa = {.sa_handler = SIG_DFL};
    sigaction(sig, &sa, NULL);
    raise(sig);
}

static uint64_t hash_buf(void) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < BUF_SIZE; i += 8) h = (h ^ *(uint64_t *)(BUF + i)) * 0x100000001b3ull;
    return h;
}

typedef struct {
    uint64_t test, r;
    uint64_t x[32], sp, nzcv;
    __uint128_t v[32];
} Case;

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (mmap(BUF, BUF_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != BUF) return 3;
    g_code = mmap(NULL, CODE_WORDS * 4, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (g_code == MAP_FAILED) return 4;
    static uint8_t buf0[BUF_SIZE];
    for (size_t i = 0; i < BUF_SIZE; i++) buf0[i] = (uint8_t)(i * 131 + (i >> 9) * 7);
    // (faults are handled on a stack of their own: during the native run, sp is the test's, inside the buffer)
    static uint8_t altstack[1 << 16];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {.sa_handler = on_fault, .sa_flags = SA_ONSTACK};
    sigaction(SIGSEGV, &sa, NULL), sigaction(SIGBUS, &sa, NULL), sigaction(SIGILL, &sa, NULL);
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 5;
    Case k;
    for (int n = 0; fread(&k, sizeof k, 1, f) == 1; n++) {
        if (k.test >= (uint64_t)ntests) { printf("%d skip bad-test\n", n); continue; }
        // lifted
        static Cpu c;
        memset(&c, 0, sizeof c);
        memcpy(c.x, k.x, sizeof k.x), c.sp = k.sp, c.nzcv = k.nzcv, memcpy(c.v, k.v, sizeof k.v);
        memcpy(BUF, buf0, BUF_SIZE);
        int sig;
        g_in_run = 1;
        if ((sig = sigsetjmp(g_jmp, 1))) { g_in_run = 0; printf("%d skip lifted-fault %d\n", n, sig); continue; }
        tests[k.test](&c);
        g_in_run = 0;
        uint64_t lh = hash_buf();
        // native
        static State s;
        memset(&s, 0, sizeof s);
        memcpy(s.b + S_X, k.x, sizeof k.x), memcpy(s.b + S_SP, &k.sp, 8), memcpy(s.b + S_NZCV, &k.nzcv, 8);
        memcpy(s.b + S_V, k.v, sizeof k.v);
        make_stub(test_words[k.test], (int)k.r);
        memcpy(BUF, buf0, BUF_SIZE);
        g_in_run = 1;
        if ((sig = sigsetjmp(g_jmp, 1))) { g_in_run = 0; printf("%d skip native-fault %d\n", n, sig); continue; }
        ((void (*)(State *))g_code)(&s);
        g_in_run = 0;
        uint64_t nh = hash_buf();
        // compare
        char what[1600] = "";
        size_t at = 0;
#define DIFF(...) (at += (size_t)snprintf(what + at, at < sizeof what ? sizeof what - at : 0, __VA_ARGS__))
        uint64_t nx[32], nsp, nnz;
        __uint128_t nv[32];
        memcpy(nx, s.b + S_X, sizeof nx), memcpy(&nsp, s.b + S_SP, 8), memcpy(&nnz, s.b + S_NZCV, 8);
        memcpy(nv, s.b + S_V, sizeof nv);
        for (int i = 0; i <= 30; i++)
            if (i != (int)k.r && i != 18 && c.x[i] != nx[i]) DIFF(" x%d:lifted=%llx,cpu=%llx", i, (unsigned long long)c.x[i], (unsigned long long)nx[i]);
        if (c.sp != nsp) DIFF(" sp:lifted=%llx,cpu=%llx", (unsigned long long)c.sp, (unsigned long long)nsp);
        if ((c.nzcv ^ nnz) & 0xF0000000u) DIFF(" nzcv:lifted=%llx,cpu=%llx", (unsigned long long)(c.nzcv >> 28 & 15), (unsigned long long)(nnz >> 28 & 15));
        for (int i = 0; i < 32; i++)
            if (c.v[i] != nv[i]) DIFF(" v%d:lifted=%016llx%016llx,cpu=%016llx%016llx", i, (unsigned long long)(c.v[i] >> 64),
                                      (unsigned long long)c.v[i], (unsigned long long)(nv[i] >> 64), (unsigned long long)nv[i]);
        if (lh != nh) DIFF(" memory");
        printf("%d %s%s\n", n, at ? "diff" : "ok", what);
    }
    fclose(f);
    return 0;
}
