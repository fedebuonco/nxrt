"""Forked worker processes for the stages made of independent pieces: discover's scans, plan's chunk files.

The workers are forked, so they share the Module copy-on-write (nothing of it is pickled, only item numbers go to
them), and what they add to the module's stats comes back with each result. Results come back in item order.
"""
import gc
import multiprocessing as mp
import os
from concurrent.futures import ProcessPoolExecutor


def jobs():
    """NXRT_GEN_JOBS, else every core. 1 (or no fork(): workers would have to rebuild the Module) does everything in
    this process."""
    if 'fork' not in mp.get_all_start_methods():
        return 1
    try:
        n = int(os.environ.get('NXRT_GEN_JOBS', ''))
    except ValueError:
        n = 0
    return n if n > 0 else os.cpu_count() or 1


_job = None  # (m, fn, items): what the forked workers inherit


def _run(i):
    m, fn, items = _job
    before = m.stats.copy()
    r = fn(items[i])
    return r, m.stats - before


def fork_map(m, fn, items):
    """fn(item) for each item, in order (a generator). fn may use and change m; with more than one job it runs in
    forked workers, whose changes to m other than m.stats stay there."""
    global _job
    njobs = min(jobs(), len(items))
    if njobs <= 1:
        yield from map(fn, items)
        return
    _job = (m, fn, items)
    gc.collect()
    gc.freeze()  # (so the workers' collector doesn't touch, and copy, every page of the Module)
    # (not multiprocessing.Pool: a worker the system kills for memory would leave it waiting forever)
    pool = ProcessPoolExecutor(njobs, mp_context=mp.get_context('fork'))
    try:
        for r, stats in pool.map(_run, range(len(items))):
            m.stats.update(stats)
            yield r
    finally:
        pool.shutdown(cancel_futures=True)
        gc.unfreeze()
        _job = None
