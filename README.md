# MiniVectorDB

单进程数据库通过标准输入和输出处理 JSONL。支持题包里的 SQL 子集、显式事务、持久化和向量检索。Docker 构建时编译本仓库的向量扫描代码，运行时使用 Python 标准库和该共享库。本地直接运行 Python 源码时可使用纯 Python 扫描。

```bash
docker build --platform linux/amd64 -t minivectordb:round2 .
python3 protocol_check/run.py --image minivectordb:round2 --output protocol-results/public
python3 tests/crash_recovery.py
python3 tests/vector_search.py
```

Dockerfile 以 Python 3.11 启动 `/app/minivectordb.py`，默认使用 `/data`。程序先输出
`{"type":"ready"}`，之后逐行读取请求，返回同一个 `id`。失败请求返回
`ok:false` 和错误码。支持 BIGINT 主键、VARCHAR、VECTOR、基础增删改查、
条件过滤和显式事务。传输格式和重启用法见 [STDIO_PROTOCOL.md](STDIO_PROTOCOL.md)。

从空卷运行 `docker run --rm -i -v minivectordb-data:/data minivectordb:round2`，
等到 ready 后逐行发送以下示例。退出后用同一条命令重启，重发 ID 9 的查询，预期仍为 `[[1,0]]`。

```jsonl
{"id":1,"op":"execute","sql":"CREATE TABLE items (id BIGINT PRIMARY KEY, v VECTOR(2))"}
{"id":2,"op":"execute","sql":"BEGIN"}
{"id":3,"op":"execute","sql":"INSERT INTO items VALUES (1, '[3,4]')"}
{"id":4,"op":"execute","sql":"COMMIT"}
{"id":5,"op":"execute","sql":"CREATE VECTOR INDEX idx ON items(v)"}
{"id":6,"op":"execute","sql":"BEGIN"}
{"id":7,"op":"execute","sql":"DELETE FROM items WHERE id = 1"}
{"id":8,"op":"execute","sql":"ROLLBACK"}
{"id":9,"op":"execute","sql":"SELECT id, L2_DISTANCE(v, '[3,4]') FROM items ORDER BY L2_DISTANCE(v, '[3,4]') LIMIT 1","search_mode":"exact"}
{"id":10,"op":"shutdown"}
```

`/data/database.log` 按行写入表结构、记录和删除操作。新写入的向量以小端 float32 字节编码到 JSON 记录内，旧日志中的 JSON 数组仍可读取。提交写入独立标记并调用 `fsync`，随后才确认成功。恢复只发布完整提交并截掉尾部未提交记录。单条语句失败时截回语句起点，保留当前事务内之前成功的修改。

常驻记录位置由主键到文件偏移的映射维护。行内容按需从日志读取，解码缓存估算上限 64 MiB。日志会保留旧版本，当前没有压缩或回收机制。结果排序带 LIMIT 时使用有界堆。没有 LIMIT 的完整查询结果需要一次性生成。

向量先转换成 float32，距离按存储值算欧氏距离，过滤先于排序。无过滤或单列等值 TopK 可以扫描紧凑的向量缓存。等值索引按列值查找候选位置。原生扫描以 double 累加平方距离并为误差边界内的候选保留余量，最终仍用 `math.dist` 排序并以主键处理距离并列。支持 AVX2 和 FMA 的 CPU 使用向量指令，其余 CPU 使用标量实现。若查询投影只用到主键和向量，结果能直接从缓存构造。向量缓存和等值索引分别受 64 MiB 和 16 MiB 的预算约束，写入或回滚会使相关缓存失效。较大数据按需扫描持久化日志。

其他过滤条件可以使用一个按首维坐标排序的有界索引，依靠坐标差剪枝。exact 模式保留精确排序和主键并列规则，ann 模式在这条剪枝路径上允许 0.1% 的距离近似。高维数据上的首维下界可能较弱。

用官方公开题包运行 Docker 版协议回归，并用本地测试对照独立计算的 TopK，覆盖浮点边界、距离并列、事务内索引失效和强制终止恢复。公开回归与本地负载用于验证候选，正式成绩以冻结提交的线上评测为准。
