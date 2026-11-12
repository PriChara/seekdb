"""Single-client SQL and vector execution over a disk-backed transaction log."""

import bisect
from collections import OrderedDict
import heapq
from itertools import islice
import json
import math
import operator
import os
from pathlib import Path
import re
import struct
import sys


class SQLError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def require(condition, message, code='invalid_argument'):
    if not condition:
        raise SQLError(code, message)


def encode(value):
    return json.dumps(value, ensure_ascii=True, allow_nan=False, separators=(',', ':'))


class Log:
    """Only row keys and log offsets remain resident after a commit."""

    def __init__(self, directory):
        directory.mkdir(parents=True, exist_ok=True)
        path = self.path = directory / 'database.log'
        if not path.exists():
            path.touch()
        self.writer = path.open('r+b', buffering=256 * 1024)
        self.reader = path.open('rb', buffering=256 * 1024)
        self.tables = {}
        self.pending = {}
        self.row_cache = OrderedDict()
        self.cache_bytes = 0
        self.transaction = False
        self.committed_end = 0
        self.recover()

    def recover(self):
        while True:
            offset = self.reader.tell()
            line = self.reader.readline()
            if not line.endswith(b'\n'):
                break
            record = json.loads(line)
            if record is None:
                self.publish()
                self.committed_end = self.reader.tell()
            else:
                table, key, value = record
                self.pending.setdefault(table, {})[key] = offset if value is not None else None
        self.rollback()

    def publish(self):
        for table, changes in self.pending.items():
            rows = self.tables.setdefault(table, {})
            for key, offset in changes.items():
                if offset is None:
                    rows.pop(key, None)
                else:
                    rows[key] = offset
        self.pending.clear()

    def rollback(self):
        self.writer.seek(self.committed_end)
        self.writer.truncate()
        self.row_cache.clear()
        self.cache_bytes = 0
        self.refresh_reader()
        self.pending.clear()
        self.transaction = False

    def commit(self):
        if self.pending:
            self.writer.write(b'null\n')
            self.writer.flush()
            os.fsync(self.writer.fileno())
            self.committed_end = self.writer.tell()
            self.publish()
        self.transaction = False

    def stage(self, table, rows):
        start = self.writer.tell()
        changes = {}
        try:
            for key, value in rows:
                offset = self.writer.tell()
                self.writer.write((encode((table, key, value)) + '\n').encode('utf-8'))
                changes[key] = offset if value is not None else None
        except (SQLError, ValueError):
            self.writer.seek(start)
            self.writer.truncate()
            self.row_cache.clear()
            self.cache_bytes = 0
            self.refresh_reader()
            raise
        if changes:
            self.writer.flush()
            self.refresh_reader()
            self.pending.setdefault(table, {}).update(changes)
            if not self.transaction:
                self.commit()
        return len(changes)

    def refresh_reader(self):
        self.reader.close()
        self.reader = self.path.open('rb', buffering=256 * 1024)

    def offset(self, table, key):
        return self.pending.get(table, {}).get(key, self.tables.get(table, {}).get(key))

    def read(self, offset):
        if offset in self.row_cache:
            self.row_cache.move_to_end(offset)
            return self.row_cache[offset][0]
        self.reader.seek(offset)
        value = json.loads(self.reader.readline())[2]
        if isinstance(value, list):
            # Bound the cache by the decoded size, including vector elements.
            size = 160 + sys.getsizeof(value) + sum(sys.getsizeof(cell) for cell in value)
            size += sum(sum(map(sys.getsizeof, cell)) for cell in value if isinstance(cell, list))
            budget = 64 * 1024 * 1024
            if size <= budget:
                while self.cache_bytes + size > budget:
                    _, (_, old_size) = self.row_cache.popitem(last=False)
                    self.cache_bytes -= old_size
                self.row_cache[offset] = (value, size)
                self.cache_bytes += size
        return value

    def rows(self, table, key=None):
        if key is not None:
            offset = self.offset(table, key)
            if offset is not None:
                yield key, self.read(offset)
            return
        pending = self.pending.get(table, {})
        for primary, offset in self.tables.get(table, {}).items():
            if primary is not None and primary not in pending:
                yield primary, self.read(offset)
        for primary, offset in pending.items():
            if primary is not None and offset is not None:
                yield primary, self.read(offset)

    def close(self):
        self.rollback()
        self.reader.close()
        self.writer.close()


TOKEN = re.compile(r"\s+|[A-Za-z_][A-Za-z_0-9]*|`(?:``|[^`])+`|'(?:''|[^'])*'|[+-]?\d+|<=|>=|[=<>(),;*]", re.ASCII)
IDENTIFIER = re.compile(r'[a-z_][a-z_0-9]*', re.ASCII)
INTEGER = re.compile(r'[+-]?\d+', re.ASCII)
COMPARE = {'=': operator.eq, '<': operator.lt, '<=': operator.le, '>': operator.gt, '>=': operator.ge}


class Tokens:
    def __init__(self, sql):
        self.items = []
        self.pos = 0
        position = 0
        while position < len(sql):
            match = TOKEN.match(sql, position)
            require(match is not None, 'Invalid SQL token', 'syntax_error')
            token = match[0]
            position = match.end()
            if not token.isspace():
                self.items.append(token if token[0] in "'`" else token.lower())

    def peek(self):
        return self.items[self.pos] if self.pos < len(self.items) else ''

    def take(self, expected=None):
        token = self.peek()
        require(token and (expected is None or token == expected), f'Expected {expected or "token"}', 'syntax_error')
        self.pos += 1
        return token

    def accept(self, expected):
        if self.peek() == expected:
            self.pos += 1
            return True
        return False

    def name(self):
        token = self.take()
        if token.startswith('`'):
            return token[1:-1].replace('``', '`').lower()
        require(IDENTIFIER.fullmatch(token), 'Expected identifier', 'syntax_error')
        return token

    def literal(self):
        token = self.take()
        if token.startswith("'"):
            return token[1:-1].replace("''", "'")
        require(INTEGER.fullmatch(token), 'Expected integer or quoted string', 'syntax_error')
        return int(token)

    def finish(self):
        self.accept(';')
        require(not self.peek(), 'Unexpected SQL suffix', 'syntax_error')


def vector(value, dimension):
    try:
        parts = json.loads(value)
        if not isinstance(parts, list) or len(parts) != dimension or any(type(x) not in (float, int) for x in parts):
            raise ValueError()
        converted = list(struct.unpack(f'<{dimension}f', struct.pack(f'<{dimension}f', *parts)))
        if not all(map(math.isfinite, converted)):
            raise ValueError()
        return converted
    except (ValueError, TypeError, OverflowError, struct.error, RecursionError):
        raise SQLError('invalid_vector', 'Expected a fixed-length finite float32 vector') from None


def validate(value, column):
    _, kind, size = column
    if kind == 'bigint':
        require(type(value) is int and -(1 << 63) <= value < 1 << 63, 'BIGINT outside signed 64-bit range')
    elif kind == 'varchar':
        require(isinstance(value, str) and len(value) <= size, 'VARCHAR value exceeds its Unicode length')
    else:
        value = vector(value, size)
    return value


class Database:
    def __init__(self, directory):
        self.log = Log(directory)
        self.indexes = {}
        for table in self.log.tables:
            schema = self.schema(table)
            if 'index' in schema:
                self.index(table, schema)

    def index(self, table, schema):
        if table not in self.indexes:
            column = next(i for i, c in enumerate(schema['columns']) if c[1] == 'vector')
            entries = sorted((row[column][0], key) for key, row in self.log.rows(table))
            self.indexes[table] = entries
        return self.indexes[table]

    def stage(self, table, records):
        count = self.log.stage(table, records)
        if count:
            self.indexes.pop(table, None)
        return count

    def execute(self, sql, mode):
        require(mode in ('exact', 'ann'), 'Invalid search mode')
        t = Tokens(sql)
        verb = t.take()
        if verb in ('begin', 'commit', 'rollback'):
            t.finish()
            if verb == 'begin':
                require(not self.log.transaction, 'A transaction is already open')
                self.log.transaction = True
            else:
                require(self.log.transaction, 'No transaction is open')
                if verb == 'rollback':
                    for table in self.log.pending:
                        self.indexes.pop(table, None)
                (self.log.commit if verb == 'commit' else self.log.rollback)()
            return {}
        if verb == 'create':
            return self.create(t)
        if verb == 'select':
            return self.select(t, mode)
        if verb in ('insert', 'update', 'delete'):
            return self.modify(verb, t)
        raise SQLError('unsupported', f'Unsupported statement: {verb}')

    def schema(self, name):
        offset = self.log.offset(name, None)
        require(offset is not None, f'Unknown table {name}', 'unknown_table')
        return self.log.read(offset)

    @staticmethod
    def column(schema, name):
        for index, column in enumerate(schema['columns']):
            if column[0] == name:
                return index
        raise SQLError('unknown_column', f'Unknown column {name}')

    def create(self, t):
        kind = t.take()
        if kind == 'vector':
            t.take('index')
            index = t.name()
            t.take('on')
            name = t.name()
            t.take('(')
            column = t.name()
            t.take(')')
            t.finish()
            schema = self.schema(name)
            i = self.column(schema, column)
            require(schema['columns'][i][1] == 'vector' and 'index' not in schema, 'Invalid vector index definition')
            schema['index'] = index
        elif kind == 'table':
            name = t.name()
            require(self.log.offset(name, None) is None, 'Table already exists')
            t.take('(')
            columns, primary = [], None
            while True:
                if t.accept('primary'):
                    t.take('key')
                    t.take('(')
                    require(primary is None, 'Only one primary key is supported')
                    primary = t.name()
                    t.take(')')
                else:
                    field, datatype = t.name(), t.take()
                    require(datatype in ('bigint', 'varchar', 'vector'), 'Unsupported column type')
                    size = 0
                    if datatype != 'bigint':
                        t.take('(')
                        size = t.literal()
                        t.take(')')
                        require(type(size) is int and size > 0, 'Column size must be positive')
                    if t.accept('primary'):
                        t.take('key')
                        require(primary is None, 'Only one primary key is supported')
                        primary = field
                    columns.append((field, datatype, size))
                if not t.accept(','):
                    break
            t.take(')')
            t.finish()
            names = [c[0] for c in columns]
            require(len(set(names)) == len(names) and primary in names, 'Invalid table columns or primary key')
            key = names.index(primary)
            require(columns[key][1] == 'bigint' and sum(c[1] == 'vector' for c in columns) <= 1, 'Invalid primary key or vector count')
            schema = {'columns': columns, 'key': key}
        else:
            raise SQLError('unsupported', 'Unsupported CREATE statement')
        self.stage(name, [(None, schema)])
        return {}

    def conditions(self, schema, t):
        conditions = []
        if t.accept('where'):
            while True:
                index = self.column(schema, t.name())
                comparison = t.take()
                column = schema['columns'][index]
                require(comparison in COMPARE and column[1] != 'vector', 'Invalid comparison')
                value = t.literal()
                if column[1] == 'varchar':
                    require(isinstance(value, str), 'Expected a string comparison')
                else:
                    value = validate(value, column)
                conditions.append((index, comparison, value))
                if not t.accept('and'):
                    break
        return conditions

    def matching(self, name, schema, conditions):
        key = next((value for index, cmp, value in conditions if index == schema['key'] and cmp == '='), None)
        return ((key, row) for key, row in self.log.rows(name, key) if matches(row, conditions))

    def modify(self, verb, t):
        if verb == 'insert':
            t.take('into')
        elif verb == 'delete':
            t.take('from')
        name = t.name()
        schema = self.schema(name)
        columns = schema['columns']
        if verb == 'insert':
            order = list(range(len(columns)))
            if t.accept('('):
                order = [self.column(schema, t.name())]
                while t.accept(','):
                    order.append(self.column(schema, t.name()))
                t.take(')')
                require(len(order) == len(columns) and len(set(order)) == len(columns), 'INSERT must supply each column')
            t.take('values')

            def records():
                seen = set()
                while True:
                    t.take('(')
                    row = [None] * len(columns)
                    for position, index in enumerate(order):
                        if position:
                            t.take(',')
                        row[index] = validate(t.literal(), columns[index])
                    t.take(')')
                    key = row[schema['key']]
                    require(key not in seen and self.log.offset(name, key) is None, 'Duplicate primary key', 'duplicate_key')
                    seen.add(key)
                    yield key, row
                    if not t.accept(','):
                        break
                t.finish()

            return {'affected_rows': self.stage(name, records())}

        updates = {}
        if verb == 'update':
            t.take('set')
            while True:
                index = self.column(schema, t.name())
                require(index != schema['key'], 'Updating the primary key is unsupported', 'unsupported')
                t.take('=')
                updates[index] = validate(t.literal(), columns[index])
                if not t.accept(','):
                    break
        conditions = self.conditions(schema, t)
        t.finish()

        def mutations():
            for key, row in self.matching(name, schema, conditions):
                row = list(row)
                for index, value in updates.items():
                    row[index] = value
                yield key, row if verb == 'update' else None

        return {'affected_rows': self.stage(name, mutations())}

    def select(self, t, mode):
        projections = []
        if not t.accept('*'):
            while True:
                projections.append(self.expression(t, allow_alias=True))
                if not t.accept(','):
                    break
        t.take('from')
        name = t.name()
        schema = self.schema(name)
        conditions = self.conditions(schema, t)
        ordering, descending, limit = None, False, None
        if t.accept('order'):
            t.take('by')
            ordering = self.expression(t)
            descending = t.accept('desc')
            if not descending:
                t.accept('asc')
        if t.accept('limit'):
            limit = t.literal()
            require(type(limit) is int and limit >= 0, 'Invalid LIMIT')
        t.finish()
        if not projections:
            projections = [(column[0], None, column[0]) for column in schema['columns']]
        output = [label for _, _, label in projections]

        def bind(expr):
            column, probe, _ = expr
            index = self.column(schema, column)
            if probe is None:
                return operator.itemgetter(index)
            _, kind, size = schema['columns'][index]
            require(kind == 'vector', 'L2_DISTANCE requires a vector column')
            query = vector(probe, size)
            return lambda row: math.dist(row[index], query)

        projectors = [bind(expr) for expr in projections]
        source = self.matching(name, schema, conditions)
        if ordering:
            rank_expr = ordering
            if ordering[1] is None and ordering[0] in output:
                rank_expr = projections[output.index(ordering[0])]
                order_value = projectors[output.index(ordering[0])]
            else:
                order_value = bind(ordering)
            if rank_expr[1] is not None and limit is not None and not descending and 'index' in schema:
                column = self.column(schema, rank_expr[0])
                probe = vector(rank_expr[1], schema['columns'][column][2])
                source = self.indexed_topk(name, schema, conditions, probe, order_value, limit, mode)
            elif descending:
                key_function = lambda item: (order_value(item[1]), -item[0])
                source = heapq.nlargest(limit, source, key=key_function) if limit is not None else sorted(source, key=key_function, reverse=True)
            else:
                key_function = lambda item: (order_value(item[1]), item[0])
                source = heapq.nsmallest(limit, source, key=key_function) if limit is not None else sorted(source, key=key_function)
        elif limit is not None:
            source = islice(source, limit)
        return {'columns': output, 'rows': [[value(row) for value in projectors] for _, row in source]}

    def indexed_topk(self, name, schema, conditions, probe, distance, limit, mode):
        if limit == 0:
            return []
        entries = self.index(name, schema)
        right = bisect.bisect_left(entries, (probe[0], -(1 << 63)))
        left = right - 1
        best = []
        # The coordinate gap is a lower bound on Euclidean distance. In ANN
        # mode only, permit a 0.1% distance approximation on larger indexes.
        factor = 1.001 if mode == 'ann' and len(entries) > 256 else 1.0
        while left >= 0 or right < len(entries):
            left_gap = probe[0] - entries[left][0] if left >= 0 else math.inf
            right_gap = entries[right][0] - probe[0] if right < len(entries) else math.inf
            if len(best) == limit and min(left_gap, right_gap) * factor > -best[0][0]:
                break
            if left_gap <= right_gap:
                _, key = entries[left]
                left -= 1
            else:
                _, key = entries[right]
                right += 1
            row = self.log.read(self.log.offset(name, key))
            if matches(row, conditions):
                item = (-distance(row), -key, row)
                if len(best) < limit:
                    heapq.heappush(best, item)
                elif item[:2] > best[0][:2]:
                    heapq.heapreplace(best, item)
        return [(-key, row) for _, key, row in sorted(best, reverse=True)]

    @staticmethod
    def expression(t, allow_alias=False):
        name = t.name()
        probe, label = None, name
        if name == 'l2_distance' and t.accept('('):
            name = t.name()
            t.take(',')
            probe = t.literal()
            t.take(')')
            label = 'L2_DISTANCE'
        if allow_alias and t.accept('as'):
            label = t.name()
        return name, probe, label


def matches(row, conditions):
    return all(COMPARE[comparison](row[index], value) for index, comparison, value in conditions)


def main():
    database = Database(Path(sys.argv[1] if len(sys.argv) == 2 else '/data'))
    print('{"type":"ready"}', flush=True)
    try:
        for line in sys.stdin:
            identifier = None
            try:
                request = json.loads(line)
                identifier = request['id']
                if request['op'] == 'shutdown':
                    database.log.rollback()
                    print(encode({'id': identifier, 'ok': True}), flush=True)
                    break
                require(request['op'] == 'execute', 'Unknown request operation')
                result = database.execute(request['sql'], request.get('search_mode', 'exact'))
                response = {'id': identifier, 'ok': True, **result}
            except SQLError as exc:
                response = {'id': identifier, 'ok': False, 'error': {'code': exc.code, 'message': str(exc)}}
            except (KeyError, TypeError, ValueError) as exc:
                response = {'id': identifier, 'ok': False, 'error': {'code': 'invalid_argument', 'message': str(exc)}}
            print(encode(response), flush=True)
    finally:
        database.log.close()


if __name__ == '__main__':
    main()
