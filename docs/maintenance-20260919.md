# RMDB 修复与验证记录（2026-09-19）

本次从已有 GitHub `main` 的 `7a9fe1d4fc24c0bc6bbc7b4db067ce3ed19923bc` 创建独立工作树，核对原题后补修实际缺陷，并增加可重复运行的回归与 GitHub Actions。本文随修复代码一同提交；包含本文的 Git 提交可定位交付版本。

## 版本与归属

- `E:\Code\rmdb.tar\rmdb` 是较完整的课程实现 A，包含 11 题的主要功能路径，不是空白框架。
- `E:\BaiduNetdiskDownload\rmdb` 是较早实现 B，主要补全存储与基础查询，含用户原有未提交修改。
- 实际框架基线 T 位于 `E:\Code\rmdb.tar\rmdb.tar`。完整来源、哈希、113 个源码文件的比较和逐题证据见[题目对照](requirements-audit.md)。
- 本轮交付目录为 `E:\Code\rmdb-audit-20260918`；采用已有 GitHub 较完整版本并修复，未覆盖 A/B 的生产代码。B 中另保存了一份审计说明。

框架已有的接口、网络和解析基础、计划调度、部分 DDL/INSERT 与单测，不记为本轮新增。原有 BIGINT、DATETIME、vector 唯一索引、聚合、多列排序、块连接、锁和事务基础，也不记为本轮重新实现。源码变化不能单独证明个人作者归属。

## 与原题的最终对应

| 题目 | 交付实现与本轮处理 | 验证范围及边界 |
| --- | --- | --- |
| 1 存储 | 文件页读写、LRU 缓冲、定长记录 CRUD；修复 pin 泄漏、RID 恢复插入、空闲链、文件描述符复用、并发偏移与内存所有权 | 原有单测与 16 个存储回归；未实现磁盘页空间回收 |
| 2 查询执行 | DDL/DML、过滤投影和连接；补上原题示例 `score=score+5`，修复多行/多索引异常一致性 | 真实 TCP SQL 回归；表达式为同类型数值列加/减常量，非通用表达式引擎 |
| 3 BIGINT | 保留 64 位 CRUD 和边界检查，补齐带符号词法与算术溢出检查 | 验证极值、越界拒绝和失败后的数据一致性 |
| 4 DATETIME | 保留严格格式、日期合法性、比较与输出 | 覆盖闰年、非法日期和查询，不扩展时间运算 |
| 5 唯一索引 | 单列/复合索引 DDL、范围扫描与维护；SHOW INDEX 返回客户端；批量 UPDATE 先检查所有最终键 | 38 个索引结果断言；另有万行单列性能测量。仍是内存有序 vector，非完整磁盘 B+ 树 |
| 6 聚合 | SUM、MIN/MAX、COUNT；修复 LIMIT 被错误用于聚合输入 | COUNT 三行 LIMIT 1 得到 3，LIMIT 0 不输出结果；单个聚合及 AS 别名，无 GROUP BY |
| 7 排序/LIMIT | 保留多列 ASC/DESC、稳定排序和行数限制；非法/超大输入返回错误 | 全量物化排序；不承诺外部排序或 Top-N |
| 8 块连接 | 保留约 8 MiB 左侧分块的嵌套循环连接 | 本轮验证连接、排序、过滤组合；未执行官方大规模跨块性能测试 |
| 9 事务 | BEGIN/COMMIT/ABORT、自动提交、堆与索引撤销；修复断线和错误回滚、唯一键位移撤销 | 语句报错中止整个当前事务；DDL 仅支持自动提交，不提供保存点 |
| 10 并发 | 保留原题要求的表级 2PL + no-wait；补目录并发保护及 DDL 表锁 | 验证读者兼容、写者冲突、DDL 不破坏其他事务；未宣称官方全部并发场景通过 |
| 11 恢复 | 强制日志先于记录修改持久化；修复重启编号、残缺尾部、已中止脏页、RID 复用、同名表重建与空闲链 | 10 个恢复回归及存储子进程崩溃用例；保留完整 WAL 重建，不是 ARIES |

## 初始缺陷 D01–D12 的处置

编号对应[初始审计](requirements-audit.md)第 4 节。这里区分已有实现、本轮修复和保留边界。

| 编号 | 最终处置 | 证据 |
| --- | --- | --- |
| D01 B 的空事务接口 | 交付基线已有完整 begin/commit/abort；没有把它算作本轮新增 | 全部 SQL 集成测试实际启动服务器并建表 |
| D02 B 的空闲页过早摘除 | 交付基线已有正常插入路径；本轮补显式 RID 插入时的非链首摘除和恢复重建 | `ExplicitInsertUnlinksAFullPageFromInsideTheFreeList` 及两项恢复空闲链测试 |
| D03 UPDATE 缺算术 | 扩展 lexer/grammar/AST、语义检查及执行；SET 表达式读取原行，检查 INT/BIGINT/FLOAT 溢出 | `test_update_arithmetic_uses_original_row_values`、`test_update_arithmetic_overflow_is_atomic` |
| D04 多索引失败污染 | 在修改前物化整条 UPDATE 并检查所有最终唯一键；允许 1,2 → 2,3 这样的合法位移 | `test_second_unique_index_failure_preserves_first_index`、`test_unique_key_shift_and_rollback` |
| D05 多行错误不回滚 | 修改前注册日志/写集；SQL 错误统一中止整个事务 | `test_multirow_unique_failure_rolls_back_earlier_rows`，显式事务语义/语法错误回归 |
| D06 聚合/LIMIT 次序 | LIMIT 用于聚合结果，不再裁剪聚合输入 | 7 个查询回归中的 4 个聚合测试方法；原基线 4 个方法失败，修复后通过 |
| D07 INSERT 违反 WAL 次序 | 记录页保持 pinned，在槽修改前回调写入并同步日志、登记撤销信息；UPDATE/DELETE 同样先写日志 | 两项 INSERT 回调测试与恢复场景；日志短写/中断/同步测试 |
| D08 重启编号重复 | 扫描日志最大 LSN/事务 ID，LSN 同时考虑表创建边界 | `test_transaction_and_log_ids_continue_after_restart`、`test_schema_boundary_reserves_lsn_after_losing_buffered_begins` |
| D09 提交顺序和同步 | `fdatasync` 完成后才解锁；隐式提交后才发送成功响应 | 日志同步包装测试、真实提交/重启回归及代码顺序核查 |
| D10 ABORT 与恢复 | 按当前表创建边界后的完整历史重建每槽最终已提交状态；排除 ABORT/未提交修改，最后重建索引 | ABORT 脏页、历史 loser 与后续 RID 复用、索引位移回滚等恢复回归 |
| D11 超大 LIMIT 异常 | 捕获标准异常，返回明确错误并撤销当前事务，连接可继续使用 | `test_out_of_range_limit_and_char_length_leave_server_usable` |
| D12 非完整 B+ 树 | 保留既有 vector 方案，明确标注为尚未实现的结构性能力 | 功能结果与本地加速测量不作为树节点分裂/合并/持久化的证明 |

另外修复并覆盖：`is_record` 的 pin 泄漏和非法 RID；恢复插入缺页；关闭文件后缓存污染复用的 fd；`RmRecord` 自赋值与反序列化泄漏；并发页 I/O 共享 seek 偏移；新页头发布次序；DROP/CREATE 同名表重启后旧行复活；显式事务中的不可撤销 DDL；TCP 半包/合包；SELECT 锁冲突返回未终止的 `abort` 导致客户端超时；SIGINT 时未完成事务没有中止日志；连接 fd 传参和线程回收。

复现时观察到的部分旧行为包括：三行 COUNT LIMIT 1 返回 1、断线后锁持续保留、并发页 I/O 出现 84 次数据不匹配、ASan/LSan 报告 102 次分配共 1312 字节泄漏、恢复后的空闲链自环导致 `No free slot in record page`。网络的四个新增用例均先在对应修复前失败，再在修复后通过；SELECT 锁冲突场景曾收到 `abort\n-` 而非 `abort\n\0`。部分复现原始输出保存在 [SQL 修复前证据](evidence/sql-regressions-before.txt)；其他分项过程日志留在本地忽略的构建目录。

## 最终验证

环境：WSL2 Ubuntu 24.04、GCC 13.3、C++17、flex 2.6.4、bison 3.8.2、Python 3.12.3；Debug 构建。具体二进制 SHA-256 与测试输出见 [验证证据](evidence/final-validation.txt) 和 [性能数据](index-benchmark.json)。仍有框架 signed/unsigned 比较等编译警告，不宣称零警告。

在交付目录执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure
cmake -S rmdb_client -B build-client
cmake --build build-client -j2
python3 tests/benchmark_index.py build/bin/rmdb --output docs/index-benchmark.json
git diff --check
```

最终 CTest 7/7 通过，失败 0：原有 GoogleTest 6 个、存储 16 个、SQL 13 个、恢复 10 个、查询 7 个、网络/关闭 4 个，共 56 个具体测试用例，另有解析器测试程序。存储分项另在 ASan/LeakSanitizer 下通过 16/16，用于确认此前内存泄漏已消除。客户端独立构建成功。逐项输出见 [CTest 详细记录](evidence/ctest-details.txt)。

测试均创建自己的临时数据库。服务端脚本使用端口 8765 和进程间文件锁，CTest 串行运行这些项目；只终止测试自身启动的进程。新增 GitHub Actions 会在 push/pull_request 上构建、运行 CTest 和构建客户端。

性能脚本用 10,000 行固定数据，每批 20 个等值和 10 个范围查询，预热后对建索引前后各测 3 批，并验证总计 240 个查询结果。最终批次中位耗时为无索引 0.483965 秒、有索引 0.150053 秒，比值 **31.00%**；完整 [JSON](index-benchmark.json) 包含逐批耗时、工作负载和环境。该测量仅检验本地单列索引；官方规则要求有索引耗时不超过无索引的 70%，本地达标不能替代官方评分或复合索引性能结论。

## 仍保留的边界

1. 索引没有真正的磁盘 B+ 树；插入/删除 vector 的移动成本、内存占用与开库重建成本仍存在。
2. 恢复扫描完整 WAL，时间和内存随历史增长；无检查点、页 LSN、CLR、日志压缩或校验和。旧版本已经产生重复事务 ID 的日志无法可靠自动消歧。
3. 已验证的是进程退出/崩溃及特定落盘次序，不是断电、磁盘损坏或任意文件系统故障。元数据使用临时文件、同步和重命名，但多文件 DDL 不构成完整断电原子事务。
4. `.epoch` 是同名表重建的恢复边界，备份时必须与表文件、`db.meta`、`db.log` 一起保留。
5. 未连接希冀评分平台，未取得官方完整测试集。本记录说明已实现内容、已修复缺陷及实际本地结果，不宣布“官方 11 题全部通过”。
