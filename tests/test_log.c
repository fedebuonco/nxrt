// The logging core (runtime/nx_log.c): levels and per-category filters, the line format, the run id, and a sink.
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "nx_log.h"

static char cap[4096];
static size_t cap_len;

static void sink(const char *line, size_t len, void *user) {
    (void)user;
    size_t room = sizeof cap - cap_len - 1;
    if (len > room) len = room;
    memcpy(cap + cap_len, line, len);
    cap_len += len;
    cap[cap_len] = 0;
}

static void clear(void) { cap_len = 0; cap[0] = 0; }

int main(void) {
    unsetenv("NXRT_LOG_FILE");
    setenv("NXRT_LOG", "warn,input=trace", 1);
    setenv("NXRT_RUN_ID", "test-run", 1);
    nx_log_reset();
    nx_log_init();
    nx_log_set_sink(sink, NULL);

    CHECK(!nx_log_wanted(NX_LOG_INFO, NX_LOG_APP));
    CHECK(nx_log_wanted(NX_LOG_WARN, NX_LOG_APP));
    CHECK(nx_log_wanted(NX_LOG_ERROR, NX_LOG_APP));
    CHECK(nx_log_wanted(NX_LOG_TRACE, NX_LOG_INPUT));

    nx_log(NX_LOG_WARN, NX_LOG_APP, "hello %d", 7);
    CHECK(strstr(cap, "[WARN]") != NULL);
    CHECK(strstr(cap, "[app]") != NULL);
    CHECK(strstr(cap, "[run=test-run]") != NULL);
    CHECK(strstr(cap, "hello 7") != NULL);
    CHECK(strchr(cap, '\n') != NULL);

    clear();
    nx_log(NX_LOG_INFO, NX_LOG_APP, "hidden by the app level");
    CHECK_EQ(cap_len, 0);

    nx_log(NX_LOG_INFO, NX_LOG_INPUT, "input line");
    CHECK(strstr(cap, "[input]") != NULL);

    clear();
    CHECK(nx_log_configure("debug"));
    CHECK(nx_log_wanted(NX_LOG_DEBUG, NX_LOG_APP));
    nx_log(NX_LOG_DEBUG, NX_LOG_APP, "debug line");
    CHECK(strstr(cap, "[DEBUG]") != NULL);

    CHECK(nx_log_configure("input=off"));
    CHECK(!nx_log_wanted(NX_LOG_INFO, NX_LOG_INPUT));
    CHECK(nx_log_wanted(NX_LOG_ERROR, NX_LOG_APP));
    CHECK(!nx_log_configure("bogus"));
    CHECK(!nx_log_configure("input=bogus"));

    CHECK_STR(nx_log_level_name(NX_LOG_WARN), "WARN");
    CHECK_STR(nx_log_category_name(NX_LOG_INPUT), "input");
    CHECK_EQ(nx_log_category_from_name("input"), NX_LOG_INPUT);
    CHECK_EQ(nx_log_category_from_name("bogus"), -1);

    nx_log_set_level(NX_LOG_ERROR);
    CHECK(!nx_log_wanted(NX_LOG_WARN, NX_LOG_APP));
    CHECK(nx_log_wanted(NX_LOG_ERROR, NX_LOG_APP));

    unsetenv("NXRT_RUN_ID");
    unsetenv("NXRT_LOG");
    unsetenv("NXRT_LOG_FILE");
    nx_log_reset();
    nx_log_init();
    nx_log_set_sink(sink, NULL);
    clear();
    nx_log(NX_LOG_INFO, NX_LOG_APP, "no run");
    CHECK(strstr(cap, "[run=") == NULL);
    CHECK(strstr(cap, "[INFO]") != NULL);
    nx_log_reset();
    TEST_END();
}
