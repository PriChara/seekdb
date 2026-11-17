"""Compare indexed search, durable writes, and peak RSS on repeatable workloads."""

import argparse
import hashlib
import json
from pathlib import Path
import random
import sys
import tempfile
import time


def peak_rss_mib():
    if sys.platform == 'win32':
        import ctypes
        from ctypes import wintypes

        class MemoryCounters(ctypes.Structure):
            _fields_ = [('size', wintypes.DWORD), ('faults', wintypes.DWORD)] + [
                (name, ctypes.c_size_t) for name in (
                    'peak', 'current', 'peak_paged', 'paged', 'peak_nonpaged',
                    'nonpaged', 'pagefile', 'peak_pagefile')
            ]

        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        psapi = ctypes.WinDLL('psapi', use_last_error=True)
        kernel.GetCurrentProcess.restype = wintypes.HANDLE
        psapi.GetProcessMemoryInfo.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(MemoryCounters), wintypes.DWORD]
        counters = MemoryCounters()
        counters.size = ctypes.sizeof(counters)
        if not psapi.GetProcessMemoryInfo(kernel.GetCurrentProcess(), ctypes.byref(counters), counters.size):
            raise ctypes.WinError(ctypes.get_last_error())
        return round(counters.peak / (1024 ** 2), 2)

    import resource
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return round(peak / (1024 ** 2 if sys.platform == 'darwin' else 1024), 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('engine', type=Path, help='path to a version of minivectordb.py')
    parser.add_argument('--seed', type=int, default=719)
    parser.add_argument('--rows', type=int, default=10_000)
    parser.add_argument('--queries', type=int, default=30)
    args = parser.parse_args()
    namespace = {}
    exec(compile(args.engine.read_bytes(), str(args.engine), 'exec'), namespace)
    rng = random.Random(args.seed)

    def probe():
        return json.dumps([round(rng.uniform(-1, 1), 6) for _ in range(128)], separators=(',', ':'))

    with tempfile.TemporaryDirectory() as directory:
        db = namespace['Database'](Path(directory))
        try:
            db.execute('CREATE TABLE docs (id BIGINT PRIMARY KEY, label BIGINT, '
                       'unique_label BIGINT, vec VECTOR(128))', 'exact')
            write_sec = 0.0
            for start in range(0, args.rows, 100):
                rows = ','.join(f"({key},{key % 16},{key},'{probe()}')"
                                for key in range(start, min(start + 100, args.rows)))
                before = time.perf_counter()
                db.execute('INSERT INTO docs VALUES ' + rows, 'exact')
                write_sec += time.perf_counter() - before
            db.execute('CREATE VECTOR INDEX ix ON docs(vec)', 'exact')
            probes = [probe() for _ in range(args.queries)]
            stats = {'write_ms': round(write_sec * 1000, 1),
                     'log_bytes': Path(directory, 'database.log').stat().st_size}
            digest = hashlib.sha256()
            for name, condition in (
                ('unfiltered', ''),
                ('low_cardinality', ' WHERE label = {value}'),
                ('high_cardinality', ' WHERE unique_label = {value}'),
                ('primary_key', ' WHERE id = {value}'),
            ):
                elapsed = 0.0
                for i, query in enumerate(probes):
                    value = i % 16 if name == 'low_cardinality' else (i * 97) % args.rows
                    sql = f"SELECT id FROM docs{condition.format(value=value)} ORDER BY L2_DISTANCE(vec, '{query}') LIMIT 10"
                    before = time.perf_counter()
                    result = db.execute(sql, 'exact')
                    elapsed += time.perf_counter() - before
                    digest.update(json.dumps(result['rows']).encode())
                stats[name + '_ms'] = round(elapsed * 1000, 1)
            stats['peak_rss_mib'] = peak_rss_mib()
            stats['results_sha256'] = digest.hexdigest()
            print(json.dumps(stats))
        finally:
            db.log.close()


if __name__ == '__main__':
    main()
