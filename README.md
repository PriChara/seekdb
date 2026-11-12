# MiniVectorDB

单进程数据库通过标准输入和输出处理 JSONL。支持题包里的 SQL 子集、显式事务、持久化和向量检索。运行时只使用 Python 标准库。

```bash
docker build --platform linux/amd64 -t minivectordb:round2 .
python3 protocol_check/run.py --image minivectordb:round2 --output protocol-results/public
python3 tests/crash_recovery.py
python3 tests/vector_search.py
```

`/data/database.log` 按行写入表结构、记录和删除操作。提交写入独立标记并调用 `fsync`，随后才确认成功。恢复只发布完整提交并截掉尾部未提交记录。单条语句失败时截回语句起点，保留当前事务内之前成功的修改。

常驻记录位置由主键到文件偏移的映射维护。行内容按需从日志读取，解码缓存估算上限 64 MiB。日志会保留旧版本，当前没有压缩或回收机制。结果排序带 LIMIT 时使用有界堆。没有 LIMIT 的完整查询结果需要一次性生成。

向量先转换成 float32，距离按存储值算欧氏距离，过滤先于排序。向量索引按首维坐标排序，用坐标差作为距离下界。exact 保留精确排序和主键并列规则。较大索引上的 ann 允许 0.1% 的距离近似。索引从持久化记录重建，写入后失效，下一次查询依据当前事务视图重建。高维数据上的首维下界可能较弱，仍需正式规模压测。

本地在离线 Linux 容器中通过 20 个公开用例和双进程强制终止恢复测试。另以固定随机种子对比了 24 次独立 TopK 计算。Docker Hub 连接超时时，本地测试用 `--build-arg PYTHON_IMAGE=team-seekdb-dev:2026` 复用已有的 Linux Python 镜像。默认基础镜像和正式 29 个用例仍待评测验证。
