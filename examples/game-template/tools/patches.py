"""The game's own code changes, applied by tools/gen.py (--game tools).

PATCHES: guest address -> function(gen, addr) returning the C that replaces that one instruction.
VARIANTS (optional): main-module offset -> instruction words the runtime may pick instead at run time
(nx_code_variant -> the game's code_variant hook).
"""
PATCHES = {}
VARIANTS = {}
