"""Cross-check the indexed path against an independently sorted float32 scan."""

import json
import math
from pathlib import Path
import random
import struct
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from minivectordb import Database


def f32(values):
    layout = f'<{len(values)}f'
    return struct.unpack(layout, struct.pack(layout, *values))


def main():
    rng = random.Random(4312)
    vectors = [f32([rng.uniform(-2, 2) for _ in range(32)]) for _ in range(1024)]
    with tempfile.TemporaryDirectory() as directory:
        db = Database(Path(directory))
        try:
            db.execute('CREATE TABLE items (id BIGINT PRIMARY KEY, category BIGINT, vec VECTOR(32))', 'exact')
            values = ','.join(f"({i},{i % 4},'{json.dumps(row)}')" for i, row in enumerate(vectors))
            db.execute('INSERT INTO items VALUES ' + values, 'exact')
            db.execute('CREATE VECTOR INDEX v ON items(vec)', 'exact')
            total_recall = 0.0
            start = time.perf_counter()
            for i in range(24):
                query = f32([rng.uniform(-2, 2) for _ in range(32)])
                category = i % 4 if i % 2 else None
                where = '' if category is None else f' WHERE category = {category}'
                sql = f"SELECT id FROM items{where} ORDER BY L2_DISTANCE(vec, '{json.dumps(query)}') LIMIT 10"
                truth = sorted((math.dist(row, query), key) for key, row in enumerate(vectors)
                               if category is None or key % 4 == category)[:10]
                expected = [key for _, key in truth]
                exact = [row[0] for row in db.execute(sql, 'exact')['rows']]
                approx = [row[0] for row in db.execute(sql, 'ann')['rows']]
                assert exact == expected, (exact, expected)
                assert len(approx) == len(set(approx)) == 10
                assert category is None or all(key % 4 == category for key in approx)
                total_recall += len(set(approx) & set(expected)) / 10
            recall = total_recall / 24
            assert recall >= 0.95
            print(f'PASS: exact matches 24 independent TopK results, ANN recall={recall:.3f}, elapsed={time.perf_counter()-start:.3f}s')

            probe = json.dumps(vectors[0])
            ordered = f" ORDER BY L2_DISTANCE(vec, '{probe}') LIMIT 1"
            query = 'SELECT id FROM items' + ordered
            assert db.execute(query, 'exact')['rows'] == [[0]]
            primary = 'SELECT id FROM items WHERE id = 7 AND category = 3' + ordered
            assert db.execute(primary, 'exact')['rows'] == [[7]]
            db.execute('BEGIN', 'exact')
            db.execute('DELETE FROM items WHERE id = 0', 'exact')
            assert db.execute(query, 'exact')['rows'] != [[0]]
            db.execute('ROLLBACK', 'exact')
            assert db.execute(query, 'exact')['rows'] == [[0]]
            db.invalidate_index('items')
            db.VECTOR_CACHE_BYTES = 0
            assert db.execute(query, 'exact')['rows'] == [[0]]
            assert db.vector_cache is None
            db.COORDINATE_INDEX_ROWS = 0
            multi_filter = 'SELECT id FROM items WHERE category = 0 AND category >= 0' + ordered
            assert db.execute(multi_filter, 'ann')['rows'] == [[0]]
            print('PASS: primary-key TopK, transactional cache invalidation, and disk scan fallback')
        finally:
            db.log.close()


if __name__ == '__main__':
    main()
