// nxrt logging: one line per record, a level and a category, a run id for the manager, stderr and an optional file.
// Configured once from the environment (docs/LOGGING.md):
//   NXRT_LOG=debug | warn | info | trace | error | input=trace,gen=debug,category=level ...
//   NXRT_LOG_FILE=<path>   append to this file as well
//   NXRT_RUN_ID=<id>       the manager's run id, shown as [run=<id>]
// Nothing here writes stdout: the manager protocol (::step, nxrt_progress, ...) stays on stdout.
#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NX_LOG_ERROR = 0,
    NX_LOG_WARN = 1,
    NX_LOG_INFO = 2,
    NX_LOG_DEBUG = 3,
    NX_LOG_TRACE = 4,
} NxLogLevel;

typedef enum {
    NX_LOG_APP = 0, NX_LOG_PKG, NX_LOG_ROMFS, NX_LOG_NSO, NX_LOG_GEN, NX_LOG_HLE,
    NX_LOG_INPUT, NX_LOG_SETTINGS, NX_LOG_SAVE, NX_LOG_MODS, NX_LOG_GPU, NX_LOG_AUDIO,
    NX_LOG_UI, NX_LOG_SYS, NX_LOG_CRASH,
    NX_LOG_CAT_COUNT
} NxLogCategory;

void nx_log_init(void);              // once, from main; later calls do nothing
void nx_log_reset(void);             // tests: forget the environment, file and sink
void nx_log_flush(void);
bool nx_log_configure(const char *spec);  // "debug", "warn,input=trace", ... (false: a bad token)
void nx_log_set_level(NxLogLevel level);
NxLogLevel nx_log_level(void);
bool nx_log_wanted(NxLogLevel level, NxLogCategory category);
void nx_log(NxLogLevel level, NxLogCategory category, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void nx_log_v(NxLogLevel level, NxLogCategory category, const char *fmt, va_list ap);
const char *nx_log_level_name(NxLogLevel level);
const char *nx_log_category_name(NxLogCategory category);
int nx_log_category_from_name(const char *name);

// A writer for tests/embedding; NULL returns to stderr + NXRT_LOG_FILE.
typedef void (*NxLogSink)(const char *line, size_t len, void *user);
void nx_log_set_sink(NxLogSink sink, void *user);

#define NX_LOG_ENABLED(level, category) nx_log_wanted((level), (category))
#define NX_LOGE(category, ...) nx_log(NX_LOG_ERROR, (category), __VA_ARGS__)
#define NX_LOGW(category, ...) nx_log(NX_LOG_WARN, (category), __VA_ARGS__)
#define NX_LOGI(category, ...) nx_log(NX_LOG_INFO, (category), __VA_ARGS__)
#define NX_LOGD(category, ...) nx_log(NX_LOG_DEBUG, (category), __VA_ARGS__)
#define NX_LOGT(category, ...) nx_log(NX_LOG_TRACE, (category), __VA_ARGS__)

#ifdef __cplusplus
}
#endif
