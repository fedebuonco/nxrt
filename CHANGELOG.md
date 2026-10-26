# Changelog

## 0.5.0 — macOS core, a manager, updates

macOS on Apple silicon only: the Vulkan renderer and the Windows / Linux builds are out of this release.

**Recompiler**
- The generator in stages (`tools/recomp`: load, discover, cfg, emit, plan, report).
- Proven jump tables compile as a `switch`; the all-instruction fallback only for unproven ones. Tables whose index
  comes from memory stay unproven (the two proof runs see different memory).
- Stable chunk files: each holds the functions of one 64 KiB range of a module's text, so a change recompiles little.
- Inline caches at indirect call sites.
- The lifter tested instruction by instruction against the Mac's own CPU (`make test-lift`).

**Runtime**
- Two condition-variable races that deadlocked games, fixed.
- Audio: a realistic work-buffer size, and mixes that send through splitters.
- Library applets hand their result back (the controller-support applet: one player, accepted).
- Games set up from their package *and its update*: the update's code, the base game's RomFS with the update's changes
  (BKTR: indirect and AES-CTR-Ex tables), read in place.
- One leveled logger for the runtime and the generator (`NXRT_LOG`, [docs/LOGGING.md](docs/LOGGING.md)).
- `NXRT_CONTROLLER`: controller settings a manager passes for a launch.

**Graphics (Metal)**
- Programs bound per stage; buffer textures; compute shared memory (LDS / STS / ATOMS).
- Shader instructions: HSETP2 / HSET2 with a constant-buffer operand, LEA.HI's constant-buffer form, TXD, AL2P,
  register-indexed fragment inputs; LDC.64 whose index register is also its destination (UI text was blank).
- A renderer feature is paid for only by the games that declare it.
- Frame capture tooling: `NXRT_DUMP_TRIGGER` (a frame's draws, its passes as images, the textures it samples) and
  `NXRT_CAPTURE_WATCH` (a watched texture's or program's draws, one image each).

**For a manager app**
- `libnxrt_manage` (`manage/`): game folders, package identity (name, version, icon, build), setup from a package and
  its update, the RomFS's files — no runtime, window or GPU.
- The apps answer `--describe`, `--status`, `--setup-files`, `--generate`, `--compile` and `--setup` (JSON progress),
  and build their own game code from a source checkout.
- The start screen's background from the title movie inside a game package.

Brought up on it: Fire Emblem: Three Houses (plays, with sound and UI text), and recomps made from a package alone by
nxrt-frontend: Skyward Sword HD (boots and runs), Metroid Dread 2.1.0 (boots on the patched RomFS, stops at its first
movie: port work).

## 0.4.0 — hardening, host tests and tooling

Parsers hardened, host tests (`tests/run.sh`), formatting and static analysis (`make format`, `make tidy`).
