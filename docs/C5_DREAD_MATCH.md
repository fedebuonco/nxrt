# C5: a real function match (Metroid Dread base → 2.1.0)

Inputs:

- base v1.0.0: `/Volumes/PortableSSD/dread-base/ExeFS/main`, build id `49161d9ccbc15df944d0b6278a3c446c...`;
- update 2.1.0: `/Volumes/FE3HDev/runs/dread-recomp/data/extracted/exefs/main`, build id
  `646761f643afebb379edd5e6a5151af2...`.

Both were extracted from the player's own XCI/NSP. The matcher is `tools/fnmap.py` (FNV-1a 64 over the function's
length and bytes, unique matches only) and the CLI is `tools/nso_fnmap.py`:

```
python3 tools/nso_fnmap.py <base>/main <update>/main --json
```

Result:

- old functions: 102,288
- new functions: 102,993
- exact unique matches: **8,660**

Both surveys report 547 imports / 37 exports / 8 HLE-covered; the update's text grows by about 286 KB. The match is
deliberately conservative: a function whose fingerprint is duplicated in either build, or whose bytes changed, is not
mapped. The JSON map (address pairs) is the output; the TSV form marks unmatched old functions with `-` for review.
