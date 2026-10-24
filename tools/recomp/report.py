"""Stage `report`: what the generator found, on stderr. The start screen's setup reads the progress lines for its bar
(runtime/gamebuild.c): keep their format."""
import sys


def discovered(name, nstarts, nfunctions, rounds, stats, seconds):
    print(f'{name}: {nstarts} boundaries, {nfunctions} functions, {rounds} rounds, {dict(stats)}, {seconds:.0f}s',
          file=sys.stderr)


def progress(name, done, total, files):
    print(f'{name}: progress {done}/{total} files {files}', file=sys.stderr, flush=True)


def finished(name, nchunks, seconds):
    print(f'{name}: wrote {nchunks} chunks, {seconds:.0f}s total', file=sys.stderr)
