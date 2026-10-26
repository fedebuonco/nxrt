#include "nx_log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static const char *const k_levels[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
static const char *const k_cats[NX_LOG_CAT_COUNT] = {
    "app", "pkg", "romfs", "nso", "gen", "hle", "input", "settings", "save", "mods", "gpu", "audio", "ui", "sys",
    "crash",
};

#define NX_LOG_OFF (-1)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_inited;
static int g_levels[NX_LOG_CAT_COUNT];
static int g_all = NX_LOG_INFO;
static FILE *g_file;
static char g_run[80];
static bool g_has_run;
static NxLogSink g_sink;
static void *g_sink_user;

static const char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static int parse_level(const char *s, int *out) {
    if (!strcasecmp(s, "error") || !strcasecmp(s, "err")) *out = NX_LOG_ERROR;
    else if (!strcasecmp(s, "warn") || !strcasecmp(s, "warning")) *out = NX_LOG_WARN;
    else if (!strcasecmp(s, "info")) *out = NX_LOG_INFO;
    else if (!strcasecmp(s, "debug")) *out = NX_LOG_DEBUG;
    else if (!strcasecmp(s, "trace")) *out = NX_LOG_TRACE;
    else if (!strcasecmp(s, "off") || !strcasecmp(s, "none")) *out = NX_LOG_OFF;
    else return 0;
    return 1;
}

static bool configure_locked(const char *spec) {
    if (!spec || !*spec) return true;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *t = (char *)trim(tok);
        if (!*t) continue;
        char *eq = strchr(t, '=');
        int level;
        if (eq) {
            *eq = 0;
            const char *cat = trim(t);
            const char *val = trim(eq + 1);
            int c = nx_log_category_from_name(cat);
            if (c < 0 || !parse_level(val, &level)) return false;
            g_levels[c] = level;
        } else {
            if (!parse_level(t, &level)) return false;
            g_all = level;
            for (int i = 0; i < NX_LOG_CAT_COUNT; i++) g_levels[i] = level;
        }
    }
    return true;
}

void nx_log_init(void) {
    pthread_mutex_lock(&g_lock);
    if (g_inited) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_all = NX_LOG_INFO;
    for (int i = 0; i < NX_LOG_CAT_COUNT; i++) g_levels[i] = NX_LOG_INFO;
    configure_locked(getenv("NXRT_LOG"));
    const char *run = getenv("NXRT_RUN_ID");
    if (run && *run) {
        size_t n = 0;
        for (const char *p = run; *p && n + 1 < sizeof g_run; p++)
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')
                g_run[n++] = *p;
        g_run[n] = 0;
        g_has_run = n > 0;
    }
    const char *path = getenv("NXRT_LOG_FILE");
    if (path && *path) {
        g_file = fopen(path, "a");
        if (g_file) setvbuf(g_file, NULL, _IOLBF, 0);
        else fprintf(stderr, "[logging] cannot append to %s\n", path);
    }
    g_inited = true;
    pthread_mutex_unlock(&g_lock);
}

void nx_log_reset(void) {
    pthread_mutex_lock(&g_lock);
    if (g_file) fclose(g_file);
    g_file = NULL;
    g_inited = false;
    g_all = NX_LOG_INFO;
    for (int i = 0; i < NX_LOG_CAT_COUNT; i++) g_levels[i] = NX_LOG_INFO;
    memset(g_run, 0, sizeof g_run);
    g_has_run = false;
    g_sink = NULL;
    g_sink_user = NULL;
    pthread_mutex_unlock(&g_lock);
}

void nx_log_flush(void) {
    pthread_mutex_lock(&g_lock);
    fflush(stderr);
    if (g_file) fflush(g_file);
    pthread_mutex_unlock(&g_lock);
}

bool nx_log_configure(const char *spec) {
    nx_log_init();
    pthread_mutex_lock(&g_lock);
    bool ok = configure_locked(spec);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void nx_log_set_level(NxLogLevel level) {
    nx_log_init();
    pthread_mutex_lock(&g_lock);
    g_all = (int)level;
    for (int i = 0; i < NX_LOG_CAT_COUNT; i++) g_levels[i] = (int)level;
    pthread_mutex_unlock(&g_lock);
}

NxLogLevel nx_log_level(void) { return (NxLogLevel)g_all; }

bool nx_log_wanted(NxLogLevel level, NxLogCategory category) {
    if ((int)category < 0 || category >= NX_LOG_CAT_COUNT) return false;
    int threshold = g_inited ? g_levels[category] : NX_LOG_INFO;
    return (int)level <= threshold;
}

const char *nx_log_level_name(NxLogLevel level) {
    return (unsigned)level < sizeof k_levels / sizeof k_levels[0] ? k_levels[level] : "?";
}

const char *nx_log_category_name(NxLogCategory category) {
    return (unsigned)category < NX_LOG_CAT_COUNT ? k_cats[category] : "?";
}

int nx_log_category_from_name(const char *name) {
    for (int i = 0; i < NX_LOG_CAT_COUNT; i++)
        if (!strcasecmp(name, k_cats[i])) return i;
    return -1;
}

void nx_log_set_sink(NxLogSink sink, void *user) {
    nx_log_init();
    pthread_mutex_lock(&g_lock);
    g_sink = sink;
    g_sink_user = user;
    pthread_mutex_unlock(&g_lock);
}

static void stamp(char *out, size_t n) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        snprintf(out, n, "-");
        return;
    }
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
}

void nx_log_v(NxLogLevel level, NxLogCategory category, const char *fmt, va_list ap) {
    nx_log_init();
    if (!nx_log_wanted(level, category)) return;
    char msg[4096];
    int n = vsnprintf(msg, sizeof msg, fmt, ap);
    if (n < 0) return;
    size_t len = (size_t)n < sizeof msg ? (size_t)n : sizeof msg - 1;
    while (len && (msg[len - 1] == '\n' || msg[len - 1] == '\r')) msg[--len] = 0;

    char ts[40], line[4600];
    stamp(ts, sizeof ts);
    int k = snprintf(line, sizeof line, "[%s] [%s] [%s]", ts, nx_log_level_name(level), nx_log_category_name(category));
    if (k < 0) return;
    size_t off = (size_t)k;
    if (off < sizeof line && g_has_run) {
        k = snprintf(line + off, sizeof line - off, " [run=%s]", g_run);
        if (k < 0) return;
        off = off + (size_t)k < sizeof line ? off + (size_t)k : sizeof line - 1;
    }
    if (off < sizeof line) {
        k = snprintf(line + off, sizeof line - off, " %s\n", msg);
        if (k < 0) return;
        off = off + (size_t)k < sizeof line ? off + (size_t)k : sizeof line - 1;
    }
    if (off == 0) return;

    pthread_mutex_lock(&g_lock);
    if (g_sink) {
        g_sink(line, off, g_sink_user);
    } else {
        fwrite(line, 1, off, stderr);
        fflush(stderr);
        if (g_file) {
            fwrite(line, 1, off, g_file);
            fflush(g_file);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void nx_log(NxLogLevel level, NxLogCategory category, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    nx_log_v(level, category, fmt, ap);
    va_end(ap);
}
