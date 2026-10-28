# Manual tests

Tests that need a GPU, a display or hardware behaviour the host suite does not assume. Run them on a Mac by hand; they
are not part of `tests/run.sh`.

## test_mtl4 — Metal 4 base + specialisation

Builds a render pipeline with an unspecialised colour attachment (`MTLPixelFormatInvalid`,
`MTL4BlendStateUnspecialized`), then specialises it to BGRA8 with blending disabled. This is the mechanism B2 needs
(nxnative's base + specialise): a pipeline the preparation can build once, with the per-draw colour state applied in
microseconds.

```
clang -fobjc-arc -framework Metal -framework Foundation tests/manual/test_mtl4.m -o /tmp/test_mtl4
/tmp/test_mtl4
```

Exit 0: both pipelines built. It was verified on macOS 26.3.1, Apple silicon, 2026-10-27.

## test_mtl4_draw — a Metal 4 pipeline in an MTLRenderCommandEncoder

The same base + specialisation, a real triangle draw through the renderer's existing Metal 3 command encoder, and a
readback of the centre pixel (red). This proves the Metal 4 path can drop into `renderer.m` without moving the command
stream to Metal 4 encoders, and that the specialised pipeline really writes colour.

```
clang -fobjc-arc -framework Metal -framework Foundation tests/manual/test_mtl4_draw.m -o /tmp/test_mtl4_draw
/tmp/test_mtl4_draw
```

Exit 0: the command buffer completed.

## test_sleep — the sleep/wake observer

Posts `NSWorkspaceWillSleepNotification` / `NSWorkspaceDidWakeNotification` and checks that `plat_sleep_watch` forwards
them. No real sleep is needed.

```
clang -fobjc-arc -framework AppKit -framework Foundation -Iplatform tests/manual/test_sleep.m \
    platform/sleep_macos.m -o /tmp/test_sleep
/tmp/test_sleep
```

## test_metal_archive — a function + pipeline archive round-trip

Compiles a trivial MSL library, adds a function and a render pipeline to an `MTLBinaryArchive`, serializes it, reloads
it, and checks the file is written. This is the #52 path without a game: it verifies the API sequence and gives a
baseline for the crash that remains off by default.

```
clang -fobjc-arc -framework Metal -framework Foundation tests/manual/test_metal_archive.m -o /tmp/test_metal_archive
/tmp/test_metal_archive
```
