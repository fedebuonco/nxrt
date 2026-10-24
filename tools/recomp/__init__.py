"""The static recompiler: one game module (an NSO) -> C, in stages. tools/gen.py runs them in this order:

    load      module.py    the NSO, its code words, the lifted-instruction cache
    discover  discover.py  function starts: symbols, unwind data, relocations, call / address targets, to a fixpoint
    cfg       cfg.py       per function: branch targets, jump tables (emulated with unicorn), the dispatch fallback
    lift      lifter.py    one instruction -> C (tools/lifter.py)
    emit      emit.py      one function -> C lines, the game's patches and variants applied
    plan      plan.py      functions -> chunk files, the address table, the main module's build id
    report    report.py    what each stage found (stderr; the start screen's progress bar reads the plan's lines)

Each stage takes the previous stage's result; only `load` reads the file. The output is the same C as the single-file
generator it was split from (byte for byte), so splitting it changed no game code.
"""
