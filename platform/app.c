#include "nx_app.h"

#include <stdio.h>
#include <stdlib.h>

#include "platform.h"

NxAppInfo nx_app = {"app", "app", "App", "org.example.app", "app", "app.ini", "dev"};
char g_window_shot_request[256];

void nx_app_init(const NxAppInfo *info) { nx_app = *info; }

// ---- crash handling: the host's (runtime/dispatch.c) when set
static PlatFaultWatch g_watch;
static PlatFaultReport g_report;
static PlatSymbolizer g_symbolizer;

void plat_set_fault_handlers(PlatFaultWatch watch, PlatFaultReport report) { g_watch = watch, g_report = report; }

static PlatCrashExplain g_explain;
void plat_set_crash_explain(PlatCrashExplain fn) { g_explain = fn; }
void plat_crash_explain_text(char *out, size_t n, const char *log_path) {
    if (g_explain) { g_explain(out, n, log_path); return; }
    snprintf(out, n, "%s stopped because of an error.\n\nThis is a bug in the app, not a problem with your game files. "
                     "The log file has the details%s%s.", nx_app.title ? nx_app.title : "The app",
             log_path && *log_path ? ": " : "", log_path && *log_path ? log_path : "");
}
void plat_set_symbolizer(PlatSymbolizer fn) { g_symbolizer = fn; }

bool plat_fault_watch(uint64_t fault_addr, uint64_t host_pc) { return g_watch && g_watch(fault_addr, host_pc); }

void plat_fault_report(const char *what, uint64_t fault_addr, uint64_t host_pc, uint64_t host_lr) {
    if (g_report) g_report(what, fault_addr, host_pc, host_lr);
    fprintf(stderr, "FATAL: %s at %llx (pc %llx, lr %llx)\n", what, (unsigned long long)fault_addr,
            (unsigned long long)host_pc, (unsigned long long)host_lr);
    plat_print_backtrace();
    plat_fatal_notice();
    abort();
}

const char *plat_host_symbol(uint64_t pc, uint64_t *offset) { return g_symbolizer ? g_symbolizer(pc, offset) : NULL; }
