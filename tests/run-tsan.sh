#!/bin/sh
# The host suite under ThreadSanitizer (tests/run.sh SAN=thread). Slower; the same checks.
set -eu
cd "$(dirname "$0")/.."
SAN=thread exec ./tests/run.sh "$@"
