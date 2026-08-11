# RMDB 项目详细学习方案

更新时间：2026-08-11
学习源码基准：`E:\BaiduNetdiskDownload\Anything\git_compare\rmdb`
原始证据目录：`E:\Code\rmdb.tar\rmdb`
已有分题测试脚本：`E:\Code\rmdb.tar`

## 1. 学习目标与真实归属口径

这份方案不是把实验报告再背一遍，而是把项目训练到以下程度：

1. 能从一条 SQL 画出 `Parser -> Analyze -> Planner -> Portal -> Executor -> Record/Index -> BufferPool -> Disk` 的真实调用链。
2. 能解释每道题修改了哪些状态、必须保持哪些不变量、失败时怎样回滚。
3. 能在源码中定位结论，能自己写最小测试暴露边界问题。
4. 面试时只讲当前代码真正实现的能力，不把框架代码、生成代码或 AI 后续维护算作自己从零实现。

### 1.1 建议掌握层级

| 层级 | 题目 | 学习目标 | 面试口径 |
|---|---|---|---|
| A：核心主讲 | 题目 1、2 | 必须能脱离源码画图、写伪代码、解释关键分支，并能重做核心函数 | “基于课程框架重点完成并调试了存储管理、记录管理和基础查询执行链路” |
| B：功能补全 | 题目 3 至 8 | 必须能指出功能横跨的层次，解释实现取舍，并补写边界测试 | “补全了类型、索引使用、聚合、排序和块连接等功能” |
| C：机制理解与集成验证 | 题目 9 至 11 | 必须能沿写集、锁表、日志解释状态变化，并复现实验脚本 | “参与补全并重点调试、验证了事务、并发控制和基础恢复流程” |

### 1.2 必须保留的项目边界

- `src/parser/lex.yy.*` 和 `src/parser/yacc.tab.*` 是 Flex/Bison 生成物。应解释 `lex.l`、`yacc.y`，不要把生成文件当作手写成果。
- `Planner::logical_optimization()` 当前仍为空实现。可以讲计划生成、谓词分配和索引选择，不讲完整的代价优化器。
- 当前索引主体是进程内的有序 `vector` 模型，并在打开数据库时从基表重建。`split`、`coalesce`、`redistribute`、`adjust_root` 等标准磁盘 B+ 树路径仍是空或简化实现。
- 当前恢复是基础物理日志加 committed redo、active undo。它不是完整 ARIES：没有 checkpoint、CLR、脏页表，也没有按 `prev_lsn` 链执行 undo。
- 已通过本地测试不等于通过全部课程隐藏测试。所有表述都要区分“源码已有”“本地已验证”和“课程平台已评分”。

## 2. 固定学习方式

### 2.1 每题必须留下的五项产物

每完成一题，在 `notes/` 下保留一个 `tNN_主题.md`，内容固定为：

```text
1. 一张数据结构/调用链图
2. 一张“输入、状态变化、输出、失败路径”表
3. 一段不超过 30 行的核心伪代码
4. 一组正常、边界、失败测试及实际结果
5. 五个面试问题的口头答案
```

每题至少做一次“闭卷复述”：关掉源码，用 10 分钟画图；再打开源码逐项纠错。看懂不计完成，能够预测测试结果才计完成。

### 2.2 上班和本地电脑的分工

默认每周约 10 小时：上班 5 次、每次 45 分钟静态阅读；本地电脑 2 次、每次约 3 小时动手。

上班时只做：

- 用 VSCode 搜函数定义和引用，记录调用者、输入、状态、输出、异常。
- 每次只给 Gemini 30 至 80 行已经定位的代码，要求引用文件和函数，不让它泛讲教材。
- 画调用链、状态机、页布局、锁兼容矩阵和日志格式。
- 写测试预期，但不声称测试已经运行。

本地电脑只做：

- 编译、单元测试、启动服务端和客户端。
- 用断点或临时日志验证静态推断。
- 在学习分支重写小函数、制造边界场景、恢复实现并跑回归。
- 保存命令、输入、实际输出和结论。

可重复使用的 Gemini 提示词：

```text
只依据下面的 RMDB 源码回答。按“调用者、输入状态、关键分支、状态变化、输出、失败路径、不变量”解释。
每个结论引用文件名和函数名。先指出当前实现，再指出与教材标准实现的差别；不要把空函数推测成已实现。
我正在学习题目 N：[题目名]。
[粘贴 30-80 行源码]
```

### 2.3 基线构建

项目依赖 POSIX socket、pthread、unistd 和 readline，优先在 Linux/WSL 环境构建。先只验证最新学习副本，不直接改原始证据目录。

```bash
cd /mnt/e/BaiduNetdiskDownload/Anything/git_compare/rmdb
cmake -S . -B build
cmake --build build -j2
./build/bin/unit_test
ctest --test-dir build --output-on-failure
```

启动服务端和客户端：

```bash
./build/bin/rmdb /tmp/rmdb-study-db
cmake -S rmdb_client -B build-client
cmake --build build-client -j2
./build-client/rmdb_client -h 127.0.0.1 -p 8765
```

基线验收：记录编译器版本、构建命令、6 个单元测试的结果，以及一次 `create table -> insert -> select` 冒烟测试。后续每题都从全新的数据库目录开始，避免旧元数据和日志干扰。

## 3. 题目一：存储管理

建议时间：12 小时。可信层级：A，必须主讲。

### 3.1 最终应掌握

- 区分磁盘页、缓冲帧、`PageId(fd, page_no)`、`frame_id` 和 `Rid(page_no, slot_no)`。
- 解释 page table、free list、LRU、pin count、dirty flag 各自解决什么问题。
- 解释记录页的 `RmPageHdr + bitmap + slots` 布局，以及空闲页链表如何变化。
- 能手写 `fetch_page`、`unpin_page`、`insert_record` 和 `RmScan::next` 的伪代码。

### 3.2 源码阅读顺序

1. `src/common/config.h`：`PAGE_SIZE`、缓冲池大小和类型别名。
2. `src/storage/page.h`：`PageId`、`Page`、pin、dirty、page LSN。
3. `src/storage/disk_manager.cpp`：`read_page`、`write_page`、`allocate_page` 和文件生命周期。
4. `src/replacer/lru_replacer.cpp`：`victim`、`pin`、`unpin`、`Size`。
5. `src/storage/buffer_pool_manager.cpp`：按 `find_victim_page -> update_page -> fetch/new/unpin/flush/delete` 阅读。
6. `src/record/rm_defs.h`、`bitmap.h`：文件头、页头、记录和位图布局。
7. `src/record/rm_file_handle.cpp`：记录增删改查和空闲页链表。
8. `src/record/rm_scan.cpp`：跨页、跳过洞、结束条件。
9. `src/unit_test.cpp`：先看测试断言，再回到实现解释为什么通过。

### 3.3 分次执行

上班静态任务：

1. 画出 `PageId -> page_table_ -> frame_id -> pages_[frame_id]` 映射，标出页命中和未命中的两条路径。
2. 手推一个三帧例子：访问 A、B、C，unpin A/B/C，再访问 A，最后装入 D；写出 LRU 顺序和 pin count。
3. 用一张表记录 `fetch_page` 每个分支对 `page_table_`、LRU、pin、dirty 的影响。
4. 计算一页可容纳的记录数，说明 bitmap 大小和 slot 地址如何由记录长度决定。
5. 手推“空页 -> 未满 -> 满 -> 删除一条 -> 再次可插入”的 `first_free_page_no` 变化。

本地动手任务：

1. 在学习分支把 `BufferPoolManager::fetch_page` 只保留函数签名，闭卷重写后与原实现比较。
2. 给 `unit_test.cpp` 增加“所有帧均被 pin 时 `fetch/new_page` 失败”的测试。
3. 增加脏页淘汰测试：写入 A、标脏、迫使淘汰、重新 fetch，验证数据来自磁盘。
4. 增加重复 `unpin` 测试，确认 pin count 不会降为负数。
5. 增加记录页满后删除一条再插入的测试，验证新 RID 复用空 slot，扫描不返回已删除记录。
6. 增加空表、单页多洞、跨页多洞三种 `RmScan` 测试。

### 3.4 过关标准

- 能在白板上解释“为什么被 pin 的页不能进入 LRU”。
- 能指出脏页在 `update_page` 淘汰旧身份之前写回，随后删除旧 page table 映射。
- 能解释为什么删除记录时只有“原来是满页”才重新加入空闲页链表。
- 能从任意 RID 算出所在页和 slot，并说明无效 RID 如何报错。
- 全部原有单元测试和新增测试通过，且没有靠扩大缓冲池掩盖问题。

面试表述：重点完成页面读写、LRU 替换、缓冲池换入换出、定长记录和扫描器；重点处理 pin/dirty 状态以及 bitmap 与空闲页链表一致性。

## 4. 题目二：查询执行

建议时间：14 小时。可信层级：A，必须主讲。

### 4.1 最终应掌握

- 讲清 `SQL -> AST -> Query -> Plan -> Executor Tree -> output` 全链路。
- 区分语法错误、语义错误、计划选择和运行期错误。
- 解释 Volcano/迭代器接口：`beginTuple`、`nextTuple`、`Next`、`is_end`。
- 解释 DDL、DML、DQL 为什么经过不同的 Portal 分支。

### 4.2 源码阅读顺序

1. `src/rmdb.cpp`：从 `yyparse` 跟到 `Analyze::do_analyze`、`plan_query`、`Portal::start/run`。
2. `src/parser/ast.h`、`lex.l`、`yacc.y`：只看源文件，不先钻生成物。
3. `src/analyze/analyze.cpp`：列补全、歧义列、条件类型检查和值转换。
4. `src/optimizer/plan.h`、`planner.cpp`：`ScanPlan`、`JoinPlan`、`ProjectionPlan` 和 DML plan。
5. `src/portal.h`：Plan 到执行器树的映射，以及 UPDATE/DELETE 先收集 RID 的原因。
6. `src/execution/executor_abstract.h`、`src/execution/executor_seq_scan.h`、`src/execution/executor_projection.h`、`src/execution/executor_nestedloop_join.h`。
7. `src/execution/executor_insert.h`、`executor_update.h`、`executor_delete.h`。
8. `src/execution/execution_manager.cpp`：执行、格式化和写入 `output.txt`。
9. `src/system/sm_manager.cpp`、`sm_meta.h`：表和列元数据持久化。

### 4.3 必须追踪的四条 SQL

对每条 SQL 写一张表，逐层记录对象类型、关键字段、所选计划、执行器和底层访问：

```sql
create table student(id int, name char(8), score float);
insert into student values(1, 'alice', 90.0);
select name from student where id = 1;
update student set score = 95.0 where id = 1;
```

再追踪一条连接：

```sql
select s.name, c.score
from student s, course c
where s.id = c.sid;
```

当前语法未必支持表别名；实际运行时按项目语法改为真实表名，但静态分析仍要说明连接条件如何被分配给 JoinPlan。

### 4.4 分次执行

上班静态任务：

1. 为四条 SQL 分别画 AST、Query 和 Plan，不允许只写类名，必须填关键字段。
2. 对照 `Portal::convert_plan_executor` 画最终执行器树。
3. 比较 SeqScan、Projection、Join、Insert、Update、Delete 的 `Next()` 契约。
4. 列出 `TableNotFound`、`ColumnNotFound`、`AmbiguousColumn`、`IncompatibleType` 在哪一层产生。
5. 解释 `logical_optimization()` 为空时，当前 Planner 仍然做了哪些物理选择。

本地动手任务：

1. 给 `Analyze::do_analyze`、`Planner::do_planner`、`Portal::convert_plan_executor` 临时加一行结构化 trace，验证静态调用链后撤销 trace。
2. 写一个 SQL 回归文件覆盖 create/show/desc/drop、insert/select/update/delete 和两表连接。
3. 加入失败用例：不存在的表、歧义列、值数量错误、类型错误。
4. 在 SeqScan 中为条件为真、为假、空条件三条路径设置断点，观察 RID 如何推进。
5. 解释并验证 UPDATE/DELETE 为什么先扫描收集 RID，再执行修改，避免边扫描边改变访问结构。

### 4.5 过关标准

- 10 分钟内从 `src/rmdb.cpp` 入口画出完整请求链。
- 能回答“SELECT 为什么不直接访问磁盘”“Query 与 Plan 有何区别”“Projection 为什么在最上层”。
- 能预测五类失败 SQL 在哪一层终止，实际运行与预测一致。
- 能闭卷写出 SeqScan 和 Projection 的迭代逻辑。

面试表述：补全语义分析、计划组装以及顺序扫描、投影、更新、删除和连接执行器，使基础 DDL/DML/DQL 贯通；当前项目没有完整代价优化器。

## 5. 题目三：BIGINT 类型

建议时间：5 小时。可信层级：B。

### 5.1 贯穿路径

按以下顺序找出 BIGINT 的每个落点，缺一个就不算完整类型支持：

1. `src/parser/lex.l`、`yacc.y`、`ast.h`：关键字、类型和整数字面量。
2. `src/defs.h`、`src/common/common.h`：`TYPE_BIGINT`、`Value::set_bigint`、8 字节 raw。
3. `src/analyze/analyze.cpp`：`strtoll` 范围检查，以及 INT 到 BIGINT 的列类型转换。
4. `src/optimizer/planner.h`：AST 类型映射到列类型。
5. `src/execution/executor_insert.h`、`executor_update.h`：序列化到记录。
6. `src/execution/executor_utils.h`：按 `int64_t` 比较。
7. `src/execution/execution_manager.cpp`：十进制输出。
8. `src/index/ix_index_handle.h`：索引 key 的 BIGINT 比较。

### 5.2 动手任务与测试

1. 画一张“文本字面量 -> `long long` -> Value -> raw 8 字节 -> record -> 输出”的转换图。
2. 手算 `INT_MAX`、`INT_MAX+1` 在 `convert_sv_value` 中分别得到什么 Value 类型，以及插入 BIGINT 列时怎样转换。
3. 测试 `-9223372036854775808`、`9223372036854775807`、两侧越界值、0 和负数。
4. 测试 insert、update、where 六种比较、order by、唯一索引键。
5. 检查越界失败后表中没有半条记录，索引也没有残留键。

过关标准：能解释为什么“语法识别 BIGINT”只完成了不到一半，以及为什么 raw 长度、比较、输出和索引都必须同步修改。

面试表述：将 BIGINT 作为 8 字节基础类型贯穿语法、值系统、记录编码、比较、输出和索引，并在字面量解析阶段做 `int64_t` 越界检查。

## 6. 题目四：DATETIME 类型

建议时间：5 小时。可信层级：B。

### 6.1 核心模型

当前实现把 `YYYY-MM-DD HH:MM:SS` 编码为 `YYYYMMDDHHMMSS` 形式的 `int64_t`。这种编码的数值顺序与时间先后顺序一致，便于直接比较和建立索引，但它不是 Unix 时间戳。

源码顺序：

1. `src/common/datetime_utils.h`：格式、闰年、月天数、编码、解码。
2. `src/parser/lex.l`、`yacc.y`、`ast.h`：DATETIME 列类型。
3. `src/analyze/analyze.cpp`：字符串常量到 DATETIME Value。
4. `src/execution/executor_insert.h`、`executor_update.h`：写入 8 字节记录。
5. `src/execution/executor_utils.h`：比较。
6. `src/execution/execution_manager.cpp`：格式化输出。
7. `src/execution/executor_index_scan.h`：DATETIME 范围上下界。

### 6.2 动手任务与测试矩阵

| 类别 | 输入 | 预期 |
|---|---|---|
| 边界合法 | `1000-01-01 00:00:00`、`9999-12-31 23:59:59` | 成功 |
| 闰年 | `2000-02-29`、`1900-02-29`、`2024-02-29` | 成功、失败、成功 |
| 月日 | 月 00/13，4 月 31 日，日期 00 | 失败 |
| 时间 | 23:59:59、24:00:00、分钟/秒 60 | 仅第一条成功 |
| 格式 | 少一位、用 `/`、缺空格、含非数字 | 失败 |
| 操作 | insert/update/delete/六种比较/order/index range | 结果与时间先后相符 |

本地再做一次性质验证：随机生成一批合法时间，比较字符串排序、编码整数排序和 SQL `ORDER BY` 输出是否一致。

过关标准：能闭卷写出闰年规则、合法性检查顺序和编码公式；能说明为什么不能只用字符串存储，以及当前方案有哪些限制。

面试表述：增加严格格式和日历合法性校验，将时间编码为可排序的 8 字节整数，并贯通增删改查、比较、排序、索引和格式化输出。

## 7. 题目五：唯一索引

建议时间：10 小时。可信层级：B，但必须主动说明当前索引边界。

### 7.1 先理解当前真实实现

当前代码同时存在 B+ 树风格的文件头、节点结构和接口，但主要功能实际由 `src/index/ix_index_handle.cpp` 中的全局有序条目表完成：

- `g_index_entries[fd]` 保存有序 `(key, Rid)`。
- `insert_entry` 用 `lower_bound` 插入并拒绝重复 key。
- `get_value` 和 `get_range` 完成点查及范围查。
- 打开数据库时，`SmManager::open_db` 扫描基表重建进程内索引条目。
- `find_leaf_page`、`split`、`insert_into_parent`、`coalesce`、`redistribute`、`adjust_root` 尚未形成标准磁盘 B+ 树流程。

因此本题先掌握“唯一索引功能链”，不要把学习重点误写成已经完成的 B+ 树分裂合并。

### 7.2 源码阅读顺序

1. `src/system/sm_meta.h`：`IndexMeta`、联合列顺序、总 key 长度。
2. `src/execution/index_utils.h`：按列顺序拼接定长联合 key。
3. `src/index/ix_defs.h`、`ix_index_handle.h`：文件头、节点和公开接口。
4. `src/index/ix_index_handle.cpp`：有序条目模型、唯一性、点查、范围查和空接口。
5. `src/index/ix_manager.h`：索引文件创建、打开、关闭。
6. `src/system/sm_manager.cpp`：`rebuild_index_entries`、create/drop/show/open。
7. `src/optimizer/planner.cpp`：`get_index_cols` 的最左前缀和范围条件选择。
8. `src/execution/executor_index_scan.h`：lower/upper key 构造及残余条件过滤。
9. Insert/Update/Delete 执行器：基表和索引的同步顺序。
10. `src/unit_test.cpp` 的 `IndexScanTest`：扫描推进和句柄生命周期。

### 7.3 必做实验

1. 单列唯一索引：建索引前有重复值应失败；建成后重复 insert 和重复 update 应失败。
2. 联合索引 `(a,b)`：验证 `a=...`、`a=... and b=...` 可选索引，只有 `b=...` 不满足最左前缀。
3. 范围边界：分别验证 `>`、`>=`、`<`、`<=`，以及闭区间两端。
4. DML 一致性：insert、delete、更新非索引列、更新索引列后分别查询。
5. 事务一致性：运行 `E:\Code\rmdb.tar\txn_index.sql`，abort 后旧 key 恢复、新 key 消失。
6. 重启一致性：重启服务端后，索引由基表重建，点查和范围查仍正确。
7. 计划验证：临时打印 `ScanPlan` tag，确认可用前缀走 `T_IndexScan`，其余走 `T_SeqScan`。

### 7.4 过关标准

- 能画出 `where -> Planner 选索引 -> IndexScan 构造边界 -> Rid -> 基表记录 -> 残余条件`。
- 能解释联合 key 为什么必须按索引定义顺序拼接。
- 能说明写操作失败时，为什么必须避免基表成功而索引只更新一半。
- 能准确回答当前索引数据实际存在哪里、重启后怎样恢复。
- 能指出完整磁盘 B+ 树还缺哪些函数，而不把接口声明当成实现。

面试表述：实现了功能型单列/联合唯一索引、最左前缀选择、范围扫描和 DML/事务下的同步维护；当前版本使用进程内有序条目并从基表重建，标准磁盘 B+ 树分裂合并仍是简化项。

## 8. 题目六：聚合函数

建议时间：5 小时。可信层级：B。

### 8.1 源码链路

1. `src/defs.h`：`AGG_COUNT/MAX/MIN/SUM`。
2. `src/parser/ast.h`、`yacc.y`：聚合表达式、`COUNT(*)`、别名。
3. `src/analyze/analyze.cpp`：列解析及 SUM 的数值类型限制。
4. `src/execution/executor_projection.h`：聚合列的输出元数据和 offset。
5. `src/execution/execution_manager.cpp`：`AggState`、扫描、累加和单行输出。

当前聚合在 `QlManager::select_from` 中集中执行，不是独立 AggregateExecutor。当前无 NULL 语义，因此 `COUNT(col)` 与 `COUNT(*)` 的差别有限；`is_aggregate` 还依赖第一列是聚合表达式，不应声称支持完整 SQL 聚合语义、GROUP BY 或任意聚合/普通列混用。

### 8.2 动手任务

1. 手推空表、一行、多行时每个 `AggState` 的状态变化。
2. 测试 `COUNT(*)`、`COUNT(col)`、`SUM(int)`、`SUM(float)`、`MAX/MIN` 和别名。
3. 测试空表的 COUNT、SUM、MAX、MIN 输出，并记录当前实际语义。
4. 测试 `SUM(char)` 失败，MAX/MIN 对 int、float、char、BIGINT、DATETIME 的行为。
5. 测试带 WHERE 条件的聚合，确认先过滤再聚合。
6. 阅读并记录混合 `select id, count(*)` 的当前行为，不把未支持行为写进成果。

过关标准：能解释聚合为什么必须消费全部输入后才输出一行，能画出 COUNT/SUM/MAX/MIN 四类状态机，并能指出当前实现与独立聚合算子的差别。

面试表述：扩展聚合语法和类型检查，在执行阶段维护聚合状态，支持 COUNT、SUM、MAX、MIN 和别名；当前不包含 GROUP BY 和完整 NULL 语义。

## 9. 题目七：ORDER BY 与 LIMIT

建议时间：5 小时。可信层级：B。

### 9.1 源码阅读顺序

1. `src/parser/ast.h`、`lex.l`、`yacc.y`：多列顺序、ASC/DESC、LIMIT。
2. `src/analyze/analyze.cpp`：查询列和条件；再注意排序列的解析实际在 Planner。
3. `src/optimizer/planner.cpp`：`generate_sort_plan`、歧义列和不存在列检查。
4. `src/optimizer/plan.h`：`SortPlan` 保存字段、方向和 limit。
5. `src/portal.h`：SortPlan 转为 SortExecutor。
6. `src/execution/execution_sort.h`：物化全部输入、`stable_sort`、多关键字比较、最后截断。

### 9.2 必做实验

1. 单列 ASC/DESC，多列不同方向，相同 key 下的稳定性。
2. int、float、char、BIGINT、DATETIME 五种类型排序。
3. `LIMIT 0`、1、等于行数、大于行数，以及只有 LIMIT 没有 ORDER BY。
4. 验证 LIMIT 在排序后截断，而不是先截断再排序。
5. 测试不存在排序列、两表同名歧义列、使用 `table.column` 消歧义。
6. 用 1 万行观察内存占用，说明当前是内存全量排序，复杂度约为 `O(n log n)`，不是外排或 Top-N 堆。

过关标准：能解释稳定排序、多列短路比较和 LIMIT 的执行位置；能指出当前 SortExecutor 的内存上界风险。

面试表述：增加多列 ASC/DESC 和 LIMIT，Planner 生成 SortPlan，SortExecutor 物化并稳定排序后截断；当前实现是内存排序，不是外部归并排序。

## 10. 题目八：块嵌套循环连接 BNLJ

建议时间：8 小时。可信层级：B。

### 10.1 当前算法

`NestedLoopJoinExecutor` 使用 8 MiB `JOIN_BUFFER_BYTES`：

- `load_left_block` 按左记录长度装入一块记录。
- 对每个左块完整扫描右输入，并把左右记录拼接后复用 `eval_conditions`。
- 无连接条件且右输入能装入 8 MiB 时走 `right_cache_` 的笛卡尔积快速路径。
- 条件连接支持等值和非等值，但仍是嵌套循环，不是 Hash Join。

源码顺序：`src/optimizer/planner.cpp` 的 JoinPlan 生成 -> `src/portal.h` 的执行器树 -> `src/execution/executor_nestedloop_join.h` -> 两侧 ScanExecutor -> `src/execution/executor_utils.h` 条件判断。

### 10.2 手推和测试

1. 用左表 5 行、每块 2 行、右表 3 行，逐次写出 `left_idx_`、右记录、输出和重新扫描时机。
2. 运行 `E:\Code\rmdb.tar\bnlj_test.sql`，覆盖等值和非等值连接。
3. 运行 `bnlj_count.sql`，确认结果跨越一个 join buffer 仍能完整迭代。
4. 增加空左表、空右表、无匹配、全部匹配、重复 key 和笛卡尔积测试。
5. 将学习分支的 `JOIN_BUFFER_BYTES` 临时改小到只能放 1 至 2 行，更容易观察换块；验证后恢复。
6. 记录理论扫描次数：普通 NLJ 与 BNLJ 在左表页数、右表页数和缓冲块数不同情况下的 I/O 差别。

### 10.3 过关标准

- 能闭卷写出 `load_left_block` 和 `advance_to_match` 的伪代码。
- 能解释右表为什么要对每个左块重新 `beginTuple()`。
- 能解释拼接后右侧列 offset 为什么要加左记录长度。
- 能准确区分当前 BNLJ、普通 NLJ 和 Hash Join。

面试表述：将连接执行改为固定内存预算的块嵌套循环，分块缓存左输入、逐轮扫描右输入，并复用统一条件比较支持等值和非等值连接。

## 11. 题目九：事务控制语句

建议时间：8 小时。可信层级：C，先做到可解释和可验证。

### 11.1 源码阅读顺序

1. `src/parser/lex.l`、`yacc.y`、`ast.h`：BEGIN、COMMIT、ABORT。
2. `src/transaction/txn_defs.h`：事务状态和三类 `WriteRecord`。
3. `src/transaction/transaction.h`：txn id、模式、写集、锁集、`prev_lsn`。
4. `src/rmdb.cpp`：`SetTransaction`、单条 SQL 自动事务和显式事务跨请求复用。
5. `src/execution/execution_manager.cpp`：事务命令分发。
6. Insert/Update/Delete 执行器：何时追加写集，保存了什么 undo 信息。
7. `src/transaction/transaction_manager.cpp`：begin、commit、abort 和反向回滚。
8. `src/execution/index_utils.h`：回滚时索引 key 的重建。

### 11.2 必须掌握的 undo 表

| 正向操作 | 写集保存 | ABORT 反向操作 |
|---|---|---|
| INSERT | 表名、RID | 删除该 RID，并删除其索引项 |
| DELETE | 表名、RID、旧记录 | 按原 RID 插回旧记录，并重建索引项 |
| UPDATE | 表名、RID、旧记录 | 删除当前索引项，恢复旧记录，再插回旧索引项 |

写集必须逆序处理。例如事务内先 INSERT 再 UPDATE，同一记录应先撤销 UPDATE，再撤销 INSERT。

### 11.3 必做实验

1. 运行 `txn_sample.sql` 和 `txn_commit_abort.sql`。
2. 运行 `txn_index.sql`，验证 abort 后基表与唯一索引同时恢复。
3. 运行 `txn_datetime.sql`，验证类型扩展、索引和事务可以组合。
4. 增加同一 RID 在一个事务内 `insert -> update -> delete -> abort` 的测试。
5. 增加重复 abort、空事务 commit、SQL 出错后显式事务状态的观察测试。
6. 在 `TransactionManager::abort` 设置断点，逐个记录写集长度、回滚顺序、基表值和索引键。

### 11.4 过关标准

- 能画出 DEFAULT/GROWING/SHRINKING/COMMITTED/ABORTED 状态及触发位置。
- 能说明自动事务与显式事务在 `SetTransaction` 和 `txn_mode` 上的差别。
- 能解释 commit 为什么清理写集，abort 为什么反向消费写集。
- 能预测三条 DML 组合在 abort 后的最终表和索引状态。

面试表述：补全显式事务生命周期和基于写集的回滚，支持插入、删除、更新的逆操作，并在回滚时同步维护唯一索引。

## 12. 题目十：并发控制

建议时间：10 小时。可信层级：C。

### 12.1 当前协议

当前运行路径使用严格的表级 S/X 锁加 no-wait 冲突处理：

- SeqScan 和 IndexScan 在开始扫描时申请表级 S 锁。
- Insert/Update/Delete 申请表级 X 锁。
- 锁冲突时立即抛 `TransactionAbortException`，不进入等待，因此不会形成等待环。
- 锁在 commit/abort 时统一释放。表级锁并发度低，但覆盖范围扫描和插入，能直接防幻读。
- LockManager 还声明 IS、IX、SIX 和记录锁接口，但主执行路径主要使用表级 S/X。

### 12.2 源码阅读顺序

1. `src/transaction/txn_defs.h`：`LockMode`、`LockDataId`、事务状态和 abort reason。
2. `src/transaction/concurrency/lock_manager.h`：锁表和请求队列结构。
3. `src/transaction/concurrency/lock_manager.cpp`：兼容性矩阵、已有锁、升级、no-wait、unlock。
4. SeqScan、IndexScan、Insert、Update、Delete 中的加锁位置。
5. `transaction_manager.cpp`：commit/abort 释放锁。
6. `rmdb.cpp`：捕获冲突异常、输出 `abort`、触发事务回滚。

### 12.3 手推矩阵与并发实验

先闭卷写出 IS/IX/S/SIX/X 兼容矩阵，再逐行对照 `compatible()`。重点检查函数参数是 requested/granted，避免把矩阵方向看反。

必须运行：

```bash
cd /mnt/e/Code/rmdb.tar
bash run_concurrency_test.sh
bash run_concurrency_read.sh
```

Windows 原始脚本位置是 `E:\Code\rmdb.tar\run_concurrency_test.sh` 和 `E:\Code\rmdb.tar\run_concurrency_read.sh`。若脚本路径与构建目录不一致，保留 Python 场景逻辑，修正服务端路径后再运行。

再补四组双连接测试：

1. S/S：两个事务同时 SELECT，应兼容。
2. X/S：事务 1 UPDATE 未提交，事务 2 SELECT，应立即 abort，不能脏读。
3. S/X：事务 1 范围 SELECT，事务 2 INSERT，应 abort，避免幻读。
4. X/X：两个事务更新同一表，后申请者 abort，避免丢失更新。

每组都记录时间线：连接、事务 id、持锁对象、锁模式、冲突点、abort 后数据值、锁是否释放。

### 12.4 过关标准

- 能用冲突图解释 no-wait 为什么预防死锁，以及它牺牲了什么。
- 能解释表级 S/X 为什么能防幻读，但并发度为何低。
- 能定位一次冲突从执行器加锁到 `rmdb.cpp` 回滚的完整异常链。
- 能区分两阶段封锁、严格两阶段封锁和 no-wait；不把它们当作同一个概念。
- 能说明记录锁和意向锁接口存在，但当前主要业务路径没有充分发挥多粒度并发。

面试表述：实现/补全锁兼容检查、事务锁集和 no-wait 冲突中止；执行路径采用严格表级 S/X 锁，直接防止脏读、不可重复读、幻读和丢失更新，代价是并发度较低。

## 13. 题目十一：故障恢复

建议时间：12 小时。可信层级：C，必须明确是基础恢复，不是完整 ARIES。

### 13.1 源码阅读顺序

1. `src/recovery/log_defs.h`：日志头字段偏移和缓冲区大小。
2. `src/recovery/log_manager.h`：BEGIN/COMMIT/ABORT/INSERT/DELETE/UPDATE 的序列化布局。
3. `src/recovery/log_manager.cpp`：LSN 分配、日志缓冲区、刷盘和 `persist_lsn`。
4. 三个 DML 执行器：日志包含的 RID、新值/旧值、`prev_lsn` 和刷盘位置。
5. `src/storage/disk_manager.cpp`：`read_log`、`write_log`。
6. `src/recovery/log_recovery.cpp`：辅助 redo/undo、`analyze`、`redo`、`undo`。
7. `src/rmdb.cpp` 主函数：打开数据库后、监听端口前执行恢复。
8. `src/storage/page.h` 和 BufferPoolManager：检查 page LSN 是否真正参与 WAL 判定。

### 13.2 当前恢复流程

```text
启动
  -> analyze 顺序读取日志，得到 committed / aborted / active 事务集合
  -> redo 顺序重放 committed 事务的 INSERT/DELETE/UPDATE
  -> undo 逆序撤销 active 事务的 INSERT/DELETE/UPDATE
  -> flush 所有表数据页
  -> 启动服务
```

三类物理日志必须能同时回答：

- INSERT：redo 怎样按 RID 插入，undo 怎样删除。
- DELETE：redo 怎样删除，undo 怎样恢复旧记录。
- UPDATE：redo 怎样写新值，undo 怎样写旧值。

恢复辅助函数还同步删除和插入索引项，防止基表恢复后索引仍指向旧 key。

### 13.3 必做崩溃实验

运行已有两阶段脚本：

```bash
cd /mnt/e/Code/rmdb.tar
bash run_recovery_test.sh
bash run_recovery_mix.sh
```

Windows 原始脚本位置是 `E:\Code\rmdb.tar\run_recovery_test.sh` 和 `E:\Code\rmdb.tar\run_recovery_mix.sh`。

场景一预期：事务 1 已提交的 `(1,1)` 保留，事务 2 未提交的 `(2,2)` 被撤销。
场景二预期：已提交的两表数据保留；未提交的 update/delete/insert 被撤销；恢复后索引范围查询、连接和排序仍可用。

再自行补充崩溃点矩阵：

| 崩溃点 | 要验证的结果 |
|---|---|
| BEGIN 后、首条写前 | 无数据变化 |
| INSERT 日志刷盘后、COMMIT 前 | 重启后撤销插入 |
| COMMIT 日志刷盘后 | 重启后保留写入 |
| 一个事务内 UPDATE 多次后 | active 事务按逆序恢复最初值 |
| 索引列 UPDATE 后、COMMIT 前 | 重启后旧 key 可查，新 key 消失 |
| 混合 INSERT/DELETE/UPDATE 后 | 基表、索引和查询执行同时一致 |

每次实验保留：崩溃前 SQL、日志记录顺序、事务集合、redo/undo 动作、重启后查询结果。

### 13.4 必须能指出的限制

- 当前 redo 只重放 committed 事务，undo 逆序扫描整个日志处理 active 事务。
- 虽然日志保存 `prev_lsn`，当前 undo 没有沿事务日志链跳转。
- 没有 checkpoint、Dirty Page Table、Transaction Table、CLR 或 repeated-history ARIES 流程。
- `Page` 虽有 page LSN 字段，BufferPoolManager 当前没有在刷脏页前用 `persist_lsn` 做完整 WAL 检查。
- 索引没有独立物理日志，而是在基表恢复动作中同步维护或启动时重建。

### 13.5 过关标准

- 能从十六进制/字段布局解释一条 UPDATE 日志如何序列化和反序列化。
- 能给定一段日志，闭卷算出 committed、aborted、active 集合以及最终数据。
- 能解释“日志已刷盘”与“数据页已刷盘”的顺序要求。
- 能说明当前实现为什么是基础 WAL/redo/undo，而不是完整 ARIES。
- 两组已有脚本及至少三组自制崩溃点测试得到预测结果。

面试表述：补全物理日志缓冲、事务日志记录和启动恢复，按事务状态对已提交修改 redo、对未完成修改逆序 undo，并同步维护索引；实现属于基础 WAL 恢复，未实现完整 ARIES/checkpoint。

## 14. 十周执行日程

总量约 105 小时。若每周只能投入 7 小时，保持顺序不变，把每一周拆成一周半，不要压缩题目 1、2、5、10、11 的动手时间。

| 周次 | 主题 | 上班静态任务 | 本地电脑任务 | 周末验收 |
|---|---|---|---|---|
| 第 1 周 | 基线 + 题目 1 上半 | 项目地图、Page/PageId/frame/LRU | 构建、原测试、重写 LRU 和 fetch trace | 手画一次缓存未命中 |
| 第 2 周 | 题目 1 下半 | 记录页、bitmap、空闲页链 | 记录洞、跨页扫描、脏页淘汰测试 | 闭卷讲完题目 1 |
| 第 3 周 | 题目 2 上半 | rmdb/analyze/planner/portal 调用链 | trace 四条 SQL、语义失败用例 | 画 AST/Query/Plan/Executor |
| 第 4 周 | 题目 2 下半 + 题目 3 | 六类执行器契约、BIGINT 落点 | DDL/DML/DQL 回归、BIGINT 边界矩阵 | 闭卷讲 SELECT 与 UPDATE |
| 第 5 周 | 题目 4 + 题目 5 上半 | DATETIME 编码、IndexMeta/key | 时间性质测试、索引创建和唯一冲突 | 讲联合 key 与重建 |
| 第 6 周 | 题目 5 下半 | Planner 最左前缀、范围边界 | IndexScan/DML/重启/事务测试 | 明确索引实现边界 |
| 第 7 周 | 题目 6 + 题目 7 | 聚合状态、SortPlan/SortExecutor | 空表聚合、多类型排序、LIMIT | 讲当前 SQL 语义限制 |
| 第 8 周 | 题目 8 + 题目 9 | BNLJ 状态机、事务写集 | 小缓冲分块、四个事务脚本 | 手推 join 与 abort |
| 第 9 周 | 题目 10 | 锁矩阵、异常链、隔离异常 | 双客户端四组并发测试 | 闭卷解释 no-wait/2PL |
| 第 10 周 | 题目 11 + 总复盘 | 日志布局、三阶段恢复、限制 | 两组恢复脚本、自制崩溃点 | 30 分钟完整项目答辩 |

每天固定节奏：

```text
上班 45 分钟：5 分钟定问题 -> 25 分钟读 30-80 行源码 -> 10 分钟画状态 -> 5 分钟写未解问题
本地 3 小时：20 分钟写预期 -> 100 分钟调试/测试 -> 40 分钟记录证据 -> 20 分钟闭卷复述
```

## 15. 最终综合验收

### 15.1 30 分钟项目答辩顺序

1. 2 分钟：课程框架和个人负责边界。
2. 5 分钟：一条 SELECT 的全链路。
3. 5 分钟：页、缓冲池、记录和 RID。
4. 4 分钟：类型、聚合、排序和 BNLJ。
5. 4 分钟：唯一索引功能链及简化边界。
6. 4 分钟：写集回滚和表级 no-wait 锁。
7. 4 分钟：物理日志 redo/undo 及非 ARIES 边界。
8. 2 分钟：测试证据、一个真实 bug 和一个后续改进。

### 15.2 最终必须能回答的十五问

1. 页与帧有什么区别，PageId 为什么还要带 fd？
2. pin count 何时增加和减少，忘记 unpin 会怎样？
3. 脏页淘汰前必须做什么？
4. 一条记录如何由 RID 找到具体字节？
5. AST、Query、Plan、Executor 分别保存什么？
6. Planner 何时选择 IndexScan？
7. BIGINT 和 DATETIME 为什么都用 8 字节但语义不同？
8. 当前聚合为什么只输出一行，空表怎样处理？
9. LIMIT 为什么必须放在排序之后？
10. BNLJ 相对 NLJ 减少了哪一部分重复扫描？
11. 当前唯一索引为什么不等于完整磁盘 B+ 树？
12. abort 为什么必须逆序处理写集？
13. no-wait 为什么不会死锁，代价是什么？
14. 表级 S/X 如何避免幻读？
15. 当前恢复与 ARIES 的关键差别是什么？

### 15.3 达标判定

只有同时满足以下条件，项目才算真正掌握：

- 题目 1、2 可闭卷画图和写伪代码。
- 题目 3 至 8 每题至少一组边界/失败测试，实际结果与预测一致。
- 题目 9 至 11 的已有脚本能复现，能逐步解释状态变化。
- 能准确说出索引、优化器和恢复的简化边界。
- 随机打开任一关键函数，3 分钟内说明调用者、输入、修改的状态、输出和异常。
- 简历与面试表述不超过源码证据，不把后续 AI 文档和索引维护提交归入原始个人实现。

## 16. 学习记录模板

```markdown
# 题目 N：主题

## 今日问题

## 调用入口
- 文件：
- 函数：
- 调用者：

## 核心状态
| 状态 | 修改前 | 修改条件 | 修改后 | 不变量 |
|---|---|---|---|---|

## 成功路径

## 失败/回滚路径

## 测试
| 输入 | 运行前预测 | 实际结果 | 差异原因 |
|---|---|---|---|

## 闭卷复述
- 一句话：
- 三分钟版：
- 仍不确定：
```
