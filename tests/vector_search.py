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
            db.execute('CREATE TABLE items (id BIGINT PRIMARY KEY, category BIGINT, '
                       'unique_label BIGINT, vec VECTOR(32))', 'exact')
            values = ','.join(f"({i},{i % 4},{1000 + i},'{json.dumps(row)}')"
                              for i, row in enumerate(vectors))
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

            order = f" ORDER BY L2_DISTANCE(vec, '{json.dumps(vectors[0])}') LIMIT 10"
            prefix = 'SELECT id FROM items WHERE unique_label = 1005'
            for mode in ('exact', 'ann'):
                assert db.execute(prefix + order, mode)['rows'] == [[5]]
                assert db.execute(prefix + ' AND category = 2' + order, mode)['rows'] == []
            query = "SELECT id FROM items WHERE category = 99 ORDER BY L2_DISTANCE(vec, '" + json.dumps(vectors[0]) + "') LIMIT 10"
            assert db.execute(query, 'exact')['rows'] == []
            db.execute('BEGIN', 'exact')
            db.execute('UPDATE items SET category = 99 WHERE id = 0', 'exact')
            assert db.execute(query, 'exact')['rows'] == [[0]]
            db.execute('ROLLBACK', 'exact')
            assert db.execute(query, 'exact')['rows'] == []
            db.execute('UPDATE items SET category = 99 WHERE id = 0', 'exact')
            assert db.execute(query, 'exact')['rows'] == [[0]]
            db.execute('DELETE FROM items WHERE id = 0', 'exact')
            assert db.execute(query, 'exact')['rows'] == []
            print('PASS: unique filters, compound predicates and equality buckets across rollback, updates and deletion')
        finally:
            db.log.close()
        reopened = Database(Path(directory))
        try:
            assert reopened.execute(prefix + order, 'exact')['rows'] == [[5]]
            assert reopened.execute(query, 'exact')['rows'] == []
            print('PASS: indexed equality filters return the same results after restart')
        finally:
            reopened.log.close()


if __name__ == '__main__':
    main()
