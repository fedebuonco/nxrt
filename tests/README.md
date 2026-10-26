# Host tests

These are unit/integration tests for the parts of nxrt that don't need a game, a GPU or a window. They compile the
real sources (no mocks of the code under test) and run with AddressSanitizer + UndefinedBehaviorSanitizer by default,
so a parser that reads one byte past a buffer fails the suite.

```sh
tests/run.sh                 # all tests
tests/run.sh lz4 romfs       # names containing "lz4" or "romfs"
SAN=0 tests/run.sh           # no sanitizers (faster; same checks)
CC=... CXX=... tests/run.sh  # another toolchain
```

From a game project that includes `nxrt.mk`, `make test` does the same.

## What is covered

| Test | Code | What it pins down |
|---|---|---|
| `test_lz4` | `runtime/lz4_block.c`, `runtime/nxpkg.c` | LZ4 decoding differentially against liblz4 (when installed), every truncated prefix and single-bit mutation of valid blocks, malformed tokens/offsets, random input; the `nx_lz4_decompress` wrapper |
| `test_aes` | `runtime/nxpkg.c` | AES-128 ECB/CTR against FIPS-197 and NIST SP 800-38A, including the AArch64 AES path, counter carry and partial blocks |
| `test_nxpkg` | `runtime/nxpkg.c` | `nx_romfs_open`/`nx_romfs_pread` on a synthetic package: uncompressed, CompressedStorage with LZ4/zero/copy blocks; corrupt counts, offsets, types and sizes are refused |
| `test_tegra` | `runtime/nvn/tegra.c`, `formats.c` | format table, known block-linear sizes, mip/layer/3D rules, swizzle → deswizzle round trips at every level |
| `test_romfs` | `runtime/romfs_build.c` | the generated RomFS image read back by an independent reader (header, hash buckets, directory/file entries, data offsets) |
| `test_hle_libc` | `runtime/hle/libc.c` | the host `mem*`/`str*` replacements: exact byte difference, `n == 0`, NUL handling, overlapping copies |
| `test_gamelib_id` | `runtime/gamelib.h` | the game-code id: build-id bytes, uppercase hex, missing/short/bad NSO |
| `test_settings` | `settings/settings.c` | defaults, first-section-wins, comments, CRLF, empty values, save→load→save idempotence |
| `test_save` | `save/save.c` | save folder selection, once-per-boot backups, keeping the newest N, the list the UI shows, listeners |
| `test_mods` | `runtime/mods.c` | scanning, `disabled_mods`/`mod_order`, swaps and moves, the override priority |
| `test_sha256` | `runtime/cache_sha256.h` | NIST vectors, padding boundaries, incremental updates |
| `test_bindings` | `input/bindings.c` | default layout, name tables, capture/cancel/swap, the settings lines (needs SDL3) |
| `test_nso` | `tools/nso.py` | a synthetic NSO: header, plain and LZ4 segments, MOD0, dynamics, symbols, relocations |
| `test_gen` | `tools/recomp/plan.py` | the chunk files written by parallel workers against one process: every file byte for byte, the progress lines the builder reads, the stats, unchanged files left alone, `NXRT_GEN_JOBS` |

## Optional dependencies

- **liblz4**: used as the reference decoder in `test_lz4`. Without it the test still runs, using a built-in
  literal-only encoder and hand-built match blocks. `run.sh` finds it through `pkg-config` or Homebrew.
- **SDL3**: only `test_bindings` needs it (scancode names); it is skipped when SDL3 isn't installed.
- **Python + `lz4`**: `test_nso.py`; with `capstone` and `unicorn` too, `test_gen.py`. They use
  `tools/venv/bin/python` when present, else `python3`.

## Adding a test

Put a `tests/test_<name>.c` (or `.cpp`/`.py`) with a `main` that returns non-zero on failure, and register it in
`tests/run.sh` with `run_c`, `run_cxx` or `run_py`. C tests can use `tests/harness.h` (`CHECK`, `CHECK_EQ`,
`CHECK_STR`, `CHECK_MEM`, `TEST_END`). Prefer building the real translation unit: if a piece is static and hard to
reach, consider moving the pure part into its own file (as `lz4_block.c` was) rather than duplicating it in the test.
