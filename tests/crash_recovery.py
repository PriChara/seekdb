"""Process-kill check for durability, transaction atomicity and log reuse."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent


def start(directory):
    process = subprocess.Popen([sys.executable, str(ROOT / 'minivectordb.py'), directory],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding='utf-8')
    assert json.loads(process.stdout.readline()) == {'type': 'ready'}
    return process


def request(process, sql=None):
    message = {'id': 1, 'op': 'shutdown'} if sql is None else {'id': 1, 'op': 'execute', 'sql': sql}
    process.stdin.write(json.dumps(message) + '\n')
    process.stdin.flush()
    answer = json.loads(process.stdout.readline())
    assert answer['id'] == 1
    return answer


def main():
    with tempfile.TemporaryDirectory() as directory:
        first = start(directory)
        for sql in ('CREATE TABLE docs (id BIGINT PRIMARY KEY, value BIGINT)', 'BEGIN',
                    'INSERT INTO docs VALUES (1, 10)', 'COMMIT', 'BEGIN',
                    'UPDATE docs SET value = 99 WHERE id = 1', 'INSERT INTO docs VALUES (2, 20)'):
            assert request(first, sql)['ok'], sql
        first.kill()
        first.communicate(timeout=5)

        second = start(directory)
        assert request(second, 'SELECT * FROM docs ORDER BY id')['rows'] == [[1, 10]]
        assert request(second, 'BEGIN')['ok']
        assert not request(second, 'INSERT INTO docs VALUES (3, 30), (1, 999)')['ok']
        assert request(second, 'INSERT INTO docs VALUES (2, 22)')['ok']
        assert request(second, 'COMMIT')['ok']
        second.kill()
        second.communicate(timeout=5)

        third = start(directory)
        assert request(third, 'SELECT * FROM docs ORDER BY id')['rows'] == [[1, 10], [2, 22]]
        assert request(third)['ok']
        assert third.wait(timeout=5) == 0
        print('PASS: two process kills, committed rows recovered, uncommitted and failed rows absent')


if __name__ == '__main__':
    main()
