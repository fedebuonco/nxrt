// Is this file a Metal binary archive? A cheap Mach-O check before MTLBinaryArchive opens it: a truncated download or a
// file from another tool must become a new archive, not a crash in Metal's loader.
#pragma once
#include <stdbool.h>

bool archive_file_ok(const char *path);
