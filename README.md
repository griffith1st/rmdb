# RMDB

基于人大 RMDB 教学框架扩展的 C++17 关系数据库课程项目。支持页式存储、LRU 缓冲池、定长记录、SQL 查询与连接、BIGINT/DATETIME、功能型唯一索引、聚合排序、表级两阶段封锁与 no-wait、事务回滚及基于完整 WAL 的进程崩溃恢复。

实现边界：索引采用内存有序 vector，启动时由堆表重建；没有实现完整磁盘 B+ 树。恢复不是 ARIES，没有检查点或 CLR。本文和测试通过记录不代表官方 11 题评分全部通过。

- [原题、原框架与你已有实现的逐题对照](docs/requirements-audit.md)
- [本轮修复、验证和遗留边界](docs/maintenance-20260919.md)
- [恢复算法与失败场景](docs/recovery-fixes.md)
- [原有归属记录](RMDB_TASKS_AND_ATTRIBUTION.md)（历史文档；以新审计修正后的结论为准）

## Build the server

运行环境：Linux 或 WSL2 Ubuntu；依赖 C++17、CMake、flex、bison、readline 和 Python 3。Ubuntu 安装命令：

```bash
sudo apt-get install build-essential cmake flex bison libreadline-dev python3
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
./build/bin/rmdb /tmp/rmdb-study-db
```

The server listens on TCP port `8765`.

## Build the client

```bash
cmake -S rmdb_client -B build-client
cmake --build build-client -j2
./build-client/rmdb_client -h 127.0.0.1 -p 8765
```

## Tests

```bash
ctest --test-dir build --output-on-failure
```

CTest 执行原有单测，以及存储、SQL、查询、恢复、网络与关闭回归，共 7 个测试项（56 个具体用例及解析器测试）。服务端集成测试通过 TCP 操作独立临时数据库，并串行占用端口 8765；执行前应关闭自己运行的 RMDB 服务。测试结束会清理其创建的数据，不使用项目中的历史数据库。

支持的扩展示例：

```sql
create table scores (id int, score int);
insert into scores values (1, 85);
create index scores(id);
update scores set score=score+5 where id=1;
select sum(score) as total from scores limit 1;
begin;
update scores set score=0;
rollback;
```

`UPDATE` 支持常量赋值及同类型数值列加/减常量；同一条语句的表达式读取原行，溢出或唯一键冲突会报错。SQL 错误中止整个当前事务。DDL 仅支持自动提交模式，修改已有表结构时遵守表锁。聚合遵循课程语法，使用单个聚合表达式和 `AS` 别名；不支持 GROUP BY。表同名重建会保存 `.epoch` 文件，请把它与 `db.meta`、`db.log` 和表文件一起保留。
