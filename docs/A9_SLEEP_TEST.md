# A9 live sleep/wake test

The observer, audio pause/resume and GPU flush are implemented; this is the part that needs a real sleep.

```
tests/manual/sleep_live.sh
```

It launches FE3H (Metal 3 for the test), asks you to load a save, sleep the Mac, wake it, check audio/picture and quit.
It then reports `[sleep]` lines, crashes and audio lines from the log.

Pass criteria:

- `[sleep] will sleep: pausing audio, flushing the GPU` before the sleep;
- `[sleep] woke: resuming audio` after the wake;
- audio resumes, the picture is intact, the save is not corrupted;
- no `[crash]`, `signal 11` or `FATAL`.

If the notifications do not arrive, the log shows no `[sleep]` line; that is the first thing to fix.
