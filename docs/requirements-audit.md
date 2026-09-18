# RMDB 题目、框架与本地实现对照

核查日期：2026-09-18。本文先记录本轮修改前的事实，避免把补修后的功能倒算成原有成果。2026-09-19 的最终修复状态、测试证据和遗留边界见[维护记录](maintenance-20260919.md)；“存在实现”只代表源码可见，不代表已通过希冀平台测试。

## 1. 先确认版本，避免比较错对象

用户给出的两个目录并非“完整项目与原封不动框架”的简单对应：

| 标识 | 本轮实际读取的来源 | 初始状态 |
| --- | --- | --- |
| A：较完整实现 | `E:\Code\rmdb.tar\rmdb` | 已有 BIGINT、DATETIME、唯一索引、聚合、多列排序、块连接、事务、锁与恢复实现；不能当作原始框架 |
| B：较早实现 | `E:\BaiduNetdiskDownload\rmdb` | Git HEAD 为 `c6b964a`（snapshot），工作区有用户未提交内容；主要补了存储与基础查询，题目 3–7、9–11 仍有缺项或空实现 |
| T：框架基线 | `E:\Code\rmdb.tar\rmdb.tar` 中的 `rmdb/` | 保留存储、执行、索引、事务、恢复的待填函数，可用于确认框架原本提供什么 |
| C：历史 GitHub 副本 | `E:\BaiduNetdiskDownload\Anything\git_compare\rmdb` | 旧证据索引记录的是此副本、HEAD `7a9fe1d`，不能把该副本的源码行号或测试数量直接套给 A/B |

T 的 SHA-256：`0e062cb68b50c5d67892e97ec0b850d4d08f7e78509da5d69c2fee99e76b9662`。这里把它称为“本地框架基线”，未借此证明它与课程远端发放包完全相同。

对 T 中 113 个 `src/` 文件逐个比较，并统一 CRLF/LF 后，初始 B 有 92 个相同、17 个不同、4 个缺失；A 有 69 个相同、44 个不同、0 个缺失。生成的 parser 文件也计入此统计，故它不是个人手写文件数量或贡献比例。A 另有 `common/datetime_utils.h`、`execution/executor_utils.h`、`execution/index_utils.h` 等新增辅助文件。

目录差异只能证明代码新增或变化。未经 Git 历史、分工记录确认，不能据此将全部新增实现认定为某个人独立完成。

## 2. 需求依据与证据层级

主要依据是 A/B 均附带的《测试说明文档.pdf》，共 16 页。本轮对全部页面进行了渲染检查；PDF 的中文字体映射使直接提取文字部分乱码，因此题目标题及判定规则以渲染页面为准。其正文从题目二开始，题目一由框架接口和课程指导补足。

额外找到并读取了 `C:\Users\32259\Downloads\2026三小数据库系统实现课设指导.pptx`：第 3–5 页说明存储管理，第 6–7 页说明查询执行，第 9 页说明显式事务，第 10 页明确要求表级锁、两阶段封锁和 no-wait，第 11 页明确 WAL、UNDO/REDO。`C:\Users\32259\Downloads\《数据库系统实现》.pptx` 第 4 页明确该课设是在 RMDB 框架上填空式实现，第 5 页列出 Linux/C++17/CMake/flex/bison/readline 环境，第 6 页要求用同一份代码验收所有题目。

证据优先级：原测试要求和课程指导 → 本地框架包 → 本轮读取的实际源码 → 可复现运行结果。`实验报告.md`、旧学习文档和已有构建目录仅作为线索，不能替代源码或本轮运行。

下文代码定位采用 `A: src/文件:行号`、`B: src/文件:行号`；行号对应本轮开始时读取的源文件，后续修改可能移动行号，函数名可作为稳定入口。

## 3. 11 题逐项对照

| 题目与要求 | T 原框架已经提供 | B 初始状态 | A 初始状态与真实边界 |
| --- | --- | --- | --- |
| 1. 存储管理：文件创建/打开/关闭/删除、页读写，缓冲池和替换，记录 CRUD 与扫描 | Page/Rid/bitmap/文件头/页头定义、接口和流程提示；页号分配、目录操作、日志文件读写、部分 RmManager 辅助操作；五个基础 GoogleTest | 已填 DiskManager、BufferPoolManager、LRU、RmFileHandle、RmScan；仍存在空闲页链提前摘除等缺陷，不能只按 TODO 注释判断未实现 | 已实现页读写、页表与 pin/dirty 管理、LRU、位图槽与空闲页链。`deallocate_page` 仍为空，不能宣称磁盘页空间回收；`write` 成功不等于掉电持久化 |
| 2. 查询执行：DDL、插入/更新/删除、投影/条件查询、笛卡尔积与连接、语义检查和浮点输出 | TCP 服务与客户端、flex/bison/AST 基础语法、计划类型和计划组装、Portal 调度、基础 InsertExecutor、元数据结构、创建数据库/创建表/展示元数据部分逻辑 | 已有 Analyze、顺扫、投影、基本嵌套循环、更新/删除、开关数据库/删表；`SetTransaction` 调用空 `begin` 后解引用，完整服务路径受阻 | 基本执行链和类型检查已实现；但 PDF 第 3 页的 `SET score = score + 5` 在初始 A/B 均不支持，语法仍是 `colName '=' value`。完整成本优化器不在现有实现中 |
| 3. BIGINT：字段增删改查、合法性和溢出检查（PDF 第 5–6 页） | 只有 INT/FLOAT/STRING；整数 token 使用原基础表示 | 无 BIGINT 类型、词法/语法与执行路径 | 新增 64 位类型、原值编码和输出；整数文本以 `strtoll` 检查越界，并区分 INT/BIGINT。需用边界及非法字面量回归证明 |
| 4. DATETIME：字段 CRUD、比较；拒绝错误格式、非法年月日/时分秒（PDF 第 6–7 页） | 无时间类型 | 无 DATETIME 类型和校验 | 新增 DATETIME，按 `YYYY-MM-DD HH:MM:SS` 严格解析；年份 1000–9999、闰年和每月天数校验，以十进制日期时间编码存 8 字节并格式化输出 |
| 5. 唯一索引：建立/删除/展示，单列/复合等值及范围查询，DML 保持唯一性，实际加速（PDF 第 7–11 页） | B+ 树节点/文件格式、IxManager、IxScan、比较函数及待填接口；Planner 已有基础索引计划分支；InsertExecutor 原本已有遍历索引的框架代码 | `create_index`/`drop_index` 空；节点查找/分裂/插入/删除为桩；IndexScan 无实现 | 有索引 DDL、唯一键、单列/复合范围、回表过滤和 DML 维护；核心条目是进程内有序 vector，开库从表重建。树查找/分裂/合并接口仍为简化桩，因此只能称“功能型唯一索引”，不能称完整磁盘 B+ 树。初始 UPDATE 多索引失败仅补偿当前索引，存在原子性缺口 |
| 6. 聚合：SUM、MAX、MIN、COUNT(col)、COUNT(*)，条件过滤，AS 名称和输出精度（PDF 第 11–13 页） | 无聚合语法和求值分支 | 无实现 | 新增聚合语法、语义和输出层计算；SUM 仅接受 INT/FLOAT，未承诺 BIGINT SUM；没有 GROUP BY。聚合与 LIMIT 的组合顺序需要补修/验证，不能由独立聚合示例推导组合正确 |
| 7. ORDER BY 与 LIMIT：单列/多列、ASC/DESC、默认升序、行数限制（PDF 第 13–14 页） | 单列 OrderBy AST 和 SortPlan、SortExecutor 空接口 | 排序执行器仍为空；无多列和 LIMIT | 新增多列排序方向列表、`stable_sort`、LIMIT；实现先全量物化再截断，不是 Top-N 或外部排序。超大/负 LIMIT 输入需要明确处理 |
| 8. 块嵌套循环连接：等值和不等值连接（PDF 第 14 页） | NestedLoopJoinExecutor 空接口及 JoinPlan | 已实现逐条左记录重扫右侧的基础 NLJ，不能称块连接 | 左输入按约 8 MiB 块装载；跨块重扫右输入；无连接条件时可缓存小右表。存在等值/不等值过滤；跨块、空表和重启迭代需用测试验证 |
| 9. 事务控制：BEGIN/COMMIT/ABORT，自动提交，插入/删除/更新的撤销，含索引/无索引（PDF 第 14–15 页） | 事务 AST/命令分发、状态/写集/锁集结构，TxnManager 接口及提示；服务器的自动事务调用框架 | `begin` 返回 nullptr，commit/abort 空；DML 不完整记录撤销信息 | 已实现开始/提交/逆序写集回滚、DML 写集、表与索引撤销；普通 RMDBError 初始处理只输出 failure，不能推导任意失败均自动撤销。客户端响应与隐式提交顺序也需补修 |
| 10. 并发：表级 2PL + no-wait，阻止脏写/脏读/丢失更新/不可重复读/幻读，允许并发读（PDF 第 15 页；指导第 10 页） | 锁类型、锁表/队列/异常定义、锁管理接口 | 所有加锁/解锁接口直接返回 true，没有真实互斥语义 | 有兼容判断、锁集、SHRINKING 限制、冲突立即抛中止异常；SQL 主要使用表 S/X 锁。表级锁符合本地课设指导，不能把没有行锁/MVCC 本身记为违反题目；高并发性能和间隙锁属另外的能力 |
| 11. 故障恢复：WAL、已提交 REDO/未提交 UNDO、单/多客户端和索引一致性、大数据恢复（PDF 第 16 页；指导第 11 页） | 日志头、BEGIN/INSERT 等部分日志结构、日志磁盘读写、Recovery 三阶段空接口及启动调用 | `add_log_to_buffer` 返回 INVALID_LSN，flush/analyze/redo/undo 空 | 有 BEGIN/COMMIT/ABORT/INSERT/DELETE/UPDATE 日志、缓冲/刷日志、全日志分类与 REDO/UNDO。初始版本仍有 INSERT 改页后记日志、编号重启从 0、撤销落盘与 ABORT 顺序等保证缺口；不是完整 ARIES，不能称任意故障恢复已正确 |

### 源码核查入口

| 能力 | 初始源码定位 |
| --- | --- |
| 存储和空闲页链 | `B: src/storage/buffer_pool_manager.cpp:61`，`B: src/record/rm_file_handle.cpp:36`；`A: src/storage/buffer_pool_manager.cpp:34`，`A: src/record/rm_file_handle.cpp:21` |
| LRU | `B: src/replacer/lru_replacer.cpp:22`；`A: src/replacer/lru_replacer.cpp:10` |
| SQL 语义和字面量 | `A: src/analyze/analyze.cpp:39`、`:198`；`B: src/analyze/analyze.cpp:18` |
| 更新表达式缺口 | `A: src/parser/yacc.y:337`；`B: src/parser/yacc.y:320` 的 `setClause` |
| 时间校验 | `A: src/common/datetime_utils.h:33` 的 `parse_datetime_to_int64` |
| 功能索引与树算法边界 | `A: src/index/ix_index_handle.cpp:11` 的 `g_index_entries`、`:127` 的 `find_leaf_page`、`:172` 的 `split`、`:184` 的 `insert_entry` |
| 索引 DDL 与重建 | `A: src/system/sm_manager.cpp:101` 的 `open_db`、`:207` 的 `show_index`、`:298` 的 `create_index`；`B: src/system/sm_manager.cpp:228` 仍是空函数 |
| 索引异常补偿 | `A: src/execution/executor_update.h:38` 的 `Next` |
| 聚合与格式 | `A: src/execution/execution_manager.cpp:182` 的 `is_aggregate` 分支；`:141` 的 `select_from` |
| 排序与 LIMIT | `A: src/execution/execution_sort.h:36`；`A: src/parser/yacc.y:423` |
| 两种连接实现 | `A: src/execution/executor_nestedloop_join.h:32` 的块大小与 `load_left_block`；`B: src/execution/executor_nestedloop_join.h:67` 的逐行 `advance` |
| 事务生命周期 | `A: src/transaction/transaction_manager.cpp:35`、`:52`、`:81`；B 同名文件 `:23`、`:38`、`:53` 为空桩 |
| 锁及 no-wait | `A: src/transaction/concurrency/lock_manager.cpp:51`；B 同名文件 `:20` 起为直接返回 true 的接口 |
| 日志和恢复 | `A: src/recovery/log_manager.cpp:19`、`:39`；`A: src/recovery/log_recovery.cpp:126`、`:177`、`:192` |
| 服务端自动提交和异常 | A/B 初始 `src/rmdb.cpp` 的 `SetTransaction`、`client_handler`；普通错误与事务中止异常是不同处理分支 |

## 4. 已确认的缺陷与尚待运行的风险

这些是初始版本记录，不等同于最终交付状态。修复后应保留条目，并以实际测试结果标注关闭，不能直接删除缺陷历史。

| 编号 | 版本 | 源码已经能确认的现象 | 后果/最小验证方向 |
| --- | --- | --- | --- |
| D01 | B | `TransactionManager::begin` 返回 nullptr；`SetTransaction` 紧接着取 transaction_id | 普通 SQL 服务路径存在空指针解引用；以真实服务器启动和第一条建表验证 |
| D02 | B | `RmFileHandle::create_page_handle` 每次取得非满页便从空闲链摘下，只有满页路径才按插入后更新 | 未满页被过早丢出空闲链，产生异常页增长/空间浪费；用小记录连续插入检查页数和复用 |
| D03 | A/B | 初始 `setClause` 只允许字面量 | 题目二展示的 `score = score + 5` 无法解析；需补语法、AST、语义检查与执行，验证多行与负数 |
| D04 | A | UPDATE 遍历多个索引，失败时只恢复当前索引；本行写集在最后追加 | 前一个索引可能残留新键而基表还是旧行；复现两个唯一索引、第二索引冲突，检查索引/顺扫/回滚/重启 |
| D05 | A | 普通 RMDBError 输出 failure 后继续；本行/本语句失败不统一撤销 | 多行 UPDATE 后一行冲突时先前修改可能残留；需要明确语句/事务错误语义并回归 |
| D06 | A | 聚合在 QlManager 输出阶段计算，排序/LIMIT 已在输入执行器完成 | COUNT/SUM 配 LIMIT 可能聚合被截断的输入；验证三行 COUNT(*) LIMIT 1，以及 LIMIT 0 |
| D07 | A | INSERT 先改记录再追加日志；缓冲池淘汰不检查 pageLSN 与已持久化 LSN | 不能证明严格 WAL；并发换页可能扩大改页与写日志之间的窗口，需要按真实日志顺序补修 |
| D08 | A | `next_txn_id_`、`global_lsn_` 从 0 开始，初始恢复未接续日志最大值 | 连续重启时新旧事务标识可能混淆；需提交→重启→未提交崩溃→再次恢复的回归 |
| D09 | A | 初始提交在解锁后 flush；自动提交在客户端响应之后；磁盘 write 无 fsync/fdatasync | 响应/可见性与持久化边界不一致；进程崩溃与系统断电保证需分开说明 |
| D10 | A | ABORT 记录将事务移出待撤销集合；撤销数据页的可靠保存缺少完整保证 | 若 ABORT 已写而撤销页尚未保存，重启可能跳过撤销；需验证混合已提交/已中止/未完成事务 |
| D11 | A | `std::stoi` 直接解析 LIMIT，普通语句异常处理主要捕获 RMDBError | 超大整数 LIMIT 可抛标准异常；应有输入拒绝和后续连接可继续使用的回归 |
| D12 | A | 索引节点分裂、合并、根调整等仍为简化实现 | 功能索引可用与完整 B+ 树是两个结论；如保持 vector 设计，应明确复杂度、内存和重建成本 |

旧证据索引还提出复合索引相同列多条件的边界组合，应继续作为回归用例：`a = 3 AND a >= 3`、条件交换、`a >= 3 AND a > 3`，以及 `(a,b)` 索引上的前缀等值加第二列范围。本文仅列测试方向，不将未执行场景写成已复现故障。

## 5. 测试要求并不等于已经通过

| 题目 | 题目文档列出的测试组/判定 |
| --- | --- |
| 1 | 框架源码含 `LRUReplacerTest.SampleTest`、`BufferPoolManagerTest.SampleTest`、`BufferPoolManagerConcurrencyTest.ConcurrencyTest`、`StorageTest.SimpleTest`、`RecordManagerTest.SimpleTest` |
| 2 | `basic_query_test1` 至 `basic_query_test5`：DDL、插入/查询、更新、删除、连接与浮点精度 |
| 3 | `storage_test6`：BIGINT CRUD 与合法性 |
| 4 | `storage_test1`、`storage_test2`：DATETIME CRUD 与合法性 |
| 5 | `storage_test3` 至 `storage_test5`；`judge_whether_use_index_on_single_attribute`、`judge_whether_use_index_on_multiple_attributes` |
| 6 | `aggregate_test1` 至 `aggregate_test3`：SUM、MAX/MIN、COUNT；浮点保留六位、整数无小数、别名与 SQL 一致 |
| 7 | `order_by_test`：单字段、多字段、LIMIT、升/降序 |
| 8 | `join_test_1`、`join_test_2`：等值/不等值连接 |
| 9 | `commit_test`、`abort_test`、`commit_index_test`、`abort_index_test` |
| 10 | `concurrency_read_test`、`dirty_write_test`、`dirty_read_test`、`lost_update_test`、`unrepeatable_read_test`、`unrepeatable_read_test_hard`、`phantom_read_test_1` 至 `_4` |
| 11 | `crash_recovery_single_thread_test`、`crash_recovery_single_thread_test_2`、`crash_recovery_index_test`、`crash_recovery_multi_thread_test`、`crash_recovery_large_data_test` |

题目五 PDF 第 11 页明确：先记录无索引查询耗时 time_a，再建立索引测 time_b；`time_b / time_a <= 70%` 才认为实际使用索引。若不满足，索引查询/维护相关测试会被判零分。因此只返回正确结果、只看到 Planner 选择索引，均不足以证明该题按评分规则通过。本地自建性能测试也不能替代官方评分环境。

本轮审计发现 A/B 的 `src/unit_test.cpp` 都只有五个上述 GoogleTest；旧副本 C 的“六个测试”多了索引回归，不能挪用为 A/B 的运行记录。原始包含测试定义并不意味着作者新增了这些测试。

本审计子任务仅进行了源码差异、PDF 渲染和需求核查，没有运行长测试，也没有访问评分平台。最终维护记录应写明所测代码提交、环境、命令、用例数量、退出状态和仍未覆盖的官方测试。

## 6. 实验报告需要纠正的口径

初始 B 的 `实验报告.md` 描述了题目 1–11 的完整功能，却与 B 中的大量空桩不符，且引用了部分不存在的代码。下列表述不能直接沿用：

| 报告表述 | 源码支持的准确表述 |
| --- | --- |
| “从零构建 RMDB” | 基于人大 RMDB 教学框架补全/扩展；框架已有网络、解析基础、数据结构、计划/调度、部分 DDL 与 INSERT 等 |
| “B+ 树索引已实现” | B 初始索引未完成；A 使用有序 vector 完成功能索引，树结构操作仍有简化桩 |
| “STRING 变长字符串” | 表列是 `CHAR(n)`，按列长存入定长记录；不能由 C++ std::string 推导数据库存储是变长布局 |
| “DiskManager::read_page 从 BufferPoolManager 获取页面” | 真实实现直接通过文件偏移及 read 读取；正确依赖方向是缓冲池调用磁盘管理器 |
| “11 题均完成/通过” | 应分清 B 已有内容、A 已有内容、本轮整合修复、尚未验证的题目测试；必须附运行证据 |
| “完整事务原子性、WAL、任意崩溃恢复” | 初始 A 仅有基础路径，必须完成异常补偿、日志先行、重启编号等补修，并说明故障模型和已验证范围 |

## 7. 本轮补修状态的记录约定

此文锁定的是初始审计结论。后续整合应优先保留两个目录的原始差异和已有用户修改，以实际复现的故障驱动修复。最终维护记录至少应关联 D01–D12：哪些由采用已有较完整实现解决、哪些是本轮新增修复、哪些仅澄清实现边界，以及每项验证命令和结果。

在主维护任务填入这些证据以前，本文不宣布任何缺陷已经关闭，也不宣布官方 11 题已全部通过。

## 8. 本轮补修与验证追加记录

最终工作目录选为 `E:\Code\rmdb-audit-20260918`，由现有 GitHub 仓库创建独立 worktree；A/B 均保留作为原始输入。以下是 2026-09-19 已实际完成的分项证据，其他维护人员负责的项目以最终维护记录为准。

| 项目 | 本轮处理 | 验证结果 |
| --- | --- | --- |
| D06：聚合与 LIMIT | Planner 不再将聚合查询的 LIMIT 下推到聚合输入；ProjectionPlan/Portal 把限制传至 QlManager，在聚合结果输出时应用。LIMIT 0 输出零行，LIMIT 1 或更大保留该查询的一行完整聚合结果 | 基线三行 `COUNT(*) LIMIT 1` 实际得到 1，修复后为 3；空输入和非空输入的 `LIMIT 0` 均无结果行 |
| 重复/矛盾索引范围与复合前缀 | 新增真实服务器 SQL 回归；对单列/复合索引分别在建索引前后核对手工期望值；覆盖条件交换、严格/非严格端点、矛盾条件、INT 最小值、非前缀谓词、CHAR/FLOAT 复合键 | 38 个索引结果断言在基线与本轮构建均通过；这批场景未复现结果缺陷，因此未改索引范围生产代码。它不代表所有范围组合或题目五性能评分已通过 |
| 聚合语法边界 | 本轮聚合修复保留既有语法：单个聚合表达式并使用 AS；不新增 GROUP BY 或同一 SELECT 多个聚合表达式 | 最终聚合用例均使用已有合法语法；ORDER BY 仍解析基础列，不宣称支持聚合别名排序 |
| D01–D05、D07–D11 | 由其他补修分项继续维护 | 此追加记录不替其他分项宣告关闭 |
| D12：磁盘 B+ 树 | 保留功能型唯一索引实现和边界说明 | 当前回归验证功能结果，不证明实现了分裂/合并/根调整或磁盘树持久化 |

本分项新增 `tests/test_query_regressions.py`，共 7 个 unittest 方法。测试通过 `tests/sql_harness.py` 在临时数据库启动真实 RMDB，并以 `/tmp/rmdb-test-port-8765.lock` 串行使用端口。

实际执行环境为 WSL Ubuntu-24.04，命令如下（在工作树根目录执行）：

```bash
# 修改生产代码前：7 个测试方法中 4 个聚合方法失败，3 个索引方法通过。
# LIMIT 0 使用 subTest，因此原始输出记录 failures=6。
python3 tests/test_query_regressions.py build/bin/rmdb-baseline

cmake -S . -B build-query
cmake --build build-query --target rmdb -j4
python3 tests/test_query_regressions.py build-query/bin/rmdb
# Ran 7 tests in 1.393s / OK / exit 0
```

独立构建成功；编译仍可见框架中的 signed/unsigned 比较等警告，没有把“编译成功”写成“零警告”。最终脚本在上述绿测试前将一个超出现有语法的多聚合 SELECT 改成两个合法单聚合 SELECT；已复现的 COUNT/LIMIT 1 和 LIMIT 0 断言保留。测试日志位于本地忽略目录 `build-query/query-results.log`，合并后的全量测试应由主维护任务再次统一记录。
