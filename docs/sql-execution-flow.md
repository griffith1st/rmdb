# RMDB：一条 SQL 的完整函数流程图

> 本文按**层次分类**画出从客户端发出 SQL 到结果回包的完整函数调用链。
> 所有节点都对应仓库当前 main 分支的真实代码，标了函数名与文件。
> 图形使用 Mermaid 书写，GitHub 会原生渲染；本地可用支持 Mermaid 的编辑器（VS Code + Markdown Preview Mermaid）查看。
> 文字主线见 [面试问答](interview-qa.md)，深挖专题见 [RMDB 面试项目深度准备](RMDB面试项目深度准备.md)。

## 图索引

| # | 分类 | 图类型 | 回答什么问题 |
| --- | --- | --- | --- |
| 1 | 总览 | 时序图 | 一条 SQL 依次经过哪些模块？ |
| 2 | 网络与解析层 | 流程图 | SQL 文本怎么变成 AST？并发怎么处理？ |
| 3 | 语义分析层 | 流程图 | AST 怎么变成带类型的 Query？ |
| 4 | 优化与计划层 | 流程图 | Query 怎么变成 Plan 树？索引怎么选？ |
| 5 | Portal 翻译层 | 流程图 | Plan 树怎么变成算子树？ |
| 6 | 执行层 | 流程图 | 算子树怎么被驱动、结果怎么产出？ |
| 7 | 存储层 | 流程图 | 一条记录怎么从磁盘到内存再回到执行器？ |
| 8 | 缓冲池 | 状态图 | 帧在生命周期里经历哪些状态？ |
| 9 | 事务与锁 | 状态图 | 事务状态怎么迁移？锁何时加、何时放？ |
| 10 | 异常路径 | 流程图 | 出错了会走哪条路？ |

---

## 图 1：总览时序图

```mermaid
sequenceDiagram
    autonumber
    participant C as 客户端 rmdb_client
    participant S as client_handler 线程
    participant P as flex/bison 解析器
    participant A as Analyze 语义分析
    participant O as Optimizer / Planner
    participant PT as Portal
    participant Q as QlManager
    participant BP as BufferPoolManager
    participant T as TransactionManager

    C->>S: send SQL 加 0x00 结尾
    S->>S: receive_request 按 0x00 拆分
    S->>S: 构造 Context
    S->>T: SetTransaction 调 begin
    T-->>S: Transaction 事务号
    S->>P: yy_scan_string 加 yyparse
    P-->>S: ast::parse_tree
    S->>A: do_analyze(parse_tree)
    A-->>S: Query
    S->>O: plan_query(query, context)
    O-->>S: Plan 树
    S->>PT: start(plan, context)
    PT->>PT: convert_plan_executor 递归转换
    PT-->>S: PortalStmt
    S->>PT: run(stmt, ql_manager)
    PT->>Q: select_from 或 run_dml 或 run_mutli_query
    Q->>BP: fetch_page 加 unpin_page
    BP-->>Q: Page 指针
    Q-->>S: 结果写入 data_send
    S->>T: commit 隐式事务
    T->>T: 写 Commit 日志并落盘 释放全部锁
    S->>C: send 响应
```

---

## 图 2：网络与解析层（rmdb.cpp）

```mermaid
flowchart TD
    A["客户端 readline 读到一行"] --> B["write sockfd 发送 SQL 加 0x00"]
    B --> C["服务端 accept 新连接"]
    C --> D["起一个线程跑 client_handler(fd)"]
    D --> E{"pending 缓冲区里<br/>能找到 0x00 吗"}
    E -->|否| F["recv 一块追加到 pending"]
    F --> E
    E -->|是| G["切出一条完整 SQL<br/>剩余留在 pending"]
    G --> H{"是 exit 或 crash 吗"}
    H -->|是| I["跳出循环 或 exit(1)"]
    H -->|否| J["构造 Context<br/>lock_mgr log_mgr data_send offset"]
    J --> K["SetTransaction 取事务号"]
    K --> L{"事务存在且状态为<br/>GROWING 或 SHRINKING 吗"}
    L -->|是| M["沿用现有事务 显式事务"]
    L -->|否| N["txn_manager begin<br/>set_txn_mode(false) 隐式事务"]
    M --> O["判断 AST 是否为 DDL 语句"]
    N --> O
    O --> P{"是 DDL 吗"}
    P -->|是| Q["catalog_write.lock 独占目录锁"]
    P -->|否| R["catalog_read.lock 共享目录锁"]
    Q --> S["buffer_mutex 串行化解析"]
    R --> S
    S --> T["yy_scan_string 加 yyparse"]
    T --> U{"yyparse 返回 0 吗"}
    U -->|否| V["抛 InternalError 语法错误"]
    U -->|是| W["得到 ast::parse_tree"]
    W --> X["buf.reset 并解锁解析器锁"]
```

**要点**：TCP 是字节流，一条 SQL 可能被拆包或两条挤在一个包里，所以用 0x00 当分隔符做**拆包/粘包处理**；
flex/bison 是全局状态不可重入，用 buffer_mutex 只锁住解析这一段，之后立刻放锁。

---

## 图 3：语义分析层（analyze/analyze.cpp）

```mermaid
flowchart TD
    A["Analyze::do_analyze(parse_tree)"] --> B{"AST 根节点类型"}
    B -->|SelectStmt| C["校验每张表存在 is_table"]
    C --> C1["get_all_cols 收集所有列元数据"]
    C1 --> C2["check_column 补全 tab_name<br/>检测歧义列与不存在列"]
    C2 --> C3["get_clause 生成 Condition 列表"]
    C3 --> C4["check_clause 检查 lhs 与 rhs 类型"]
    C4 --> C5["coerce_value_to_col 字面量按列类型提升"]
    B -->|UpdateStmt| D["逐条解析 set_clauses"]
    D --> D1["coerce_value_to_col 强制转换 rhs"]
    D1 --> D2["get_clause 加 check_clause"]
    B -->|DeleteStmt| E["get_clause 加 check_clause"]
    B -->|InsertStmt| G["convert_sv_value 转换每个字面量"]
    C5 --> H["query->parse = parse_tree"]
    D2 --> H
    E --> H
    G --> H
    H --> I["返回 Query"]
```

**要点**：这一层的产出是 Query，它把**文本层面的名字**（表名、列名、字面量）绑定到**元数据层面的对象**
（TabMeta、ColMeta、带类型和字节长度的 Value）。到这里为止，所有"这个名字存不存在""类型对不对"的问题都应当已经暴露。

---

## 图 4：优化与计划生成层（optimizer/）

```mermaid
flowchart TD
    A["Optimizer::plan_query(query, context)"] --> B{"parse 节点类型"}
    B -->|"Help ShowTables DescTable<br/>TxnBegin TxnCommit TxnAbort TxnRollback"| C["直接构造 OtherPlan"]
    B -->|其他语句| D["Planner::do_planner(query, context)"]
    D --> E{"语句类型"}
    E -->|SELECT| F["generate_select_plan"]
    E -->|INSERT| G["DMLPlan 标签 T_Insert<br/>值来自 query->values"]
    E -->|UPDATE 或 DELETE| H["get_index_cols 决定扫描方式"]
    E -->|"CREATE 或 DROP TABLE INDEX"| I["DDLPlan"]
    H --> H1{"索引前缀列能否<br/>全部匹配等值条件"}
    H1 -->|是| H2["ScanPlan 标签 T_IndexScan"]
    H1 -->|否| H3["ScanPlan 标签 T_SeqScan"]
    H2 --> H4["DMLPlan 标签 T_Update 或 T_Delete"]
    H3 --> H4
    F --> F1["logical_optimization<br/>当前为空实现 直接返回"]
    F1 --> F2["physical_optimization"]
    F2 --> F3["make_one_rel"]
    F3 --> F4["pop_conds 把单表可解条件<br/>下推进该表的 ScanPlan"]
    F4 --> F5{"表的数量是 1 吗"}
    F5 -->|是| F6["直接返回该 ScanPlan"]
    F5 -->|否| F7["按连接条件自底向上建 JoinPlan<br/>标签 T_NestLoop"]
    F7 --> F8["push_conds 把剩余条件下推到 JoinPlan"]
    F6 --> F9["generate_sort_plan"]
    F8 --> F9
    F9 --> F10{"有 ORDER BY 或 LIMIT 吗"}
    F10 -->|是| F11["外层套 SortPlan"]
    F10 -->|否| F12["保持原计划"]
    F11 --> F13["最外层包 ProjectionPlan<br/>聚合查询另带 aggregate_limit"]
    F12 --> F13
    F13 --> F14["DMLPlan 标签 T_select"]
```

**要点**：索引选择规则很保守 —— 要求索引**前缀列全部有等值条件**才走索引扫描，
不重排 where 条件顺序，也没有基于代价的比较。聚合查询的 LIMIT 不会截断聚合输入，而是作用在聚合结果上。

---

## 图 5：Portal 翻译层（portal.h）

```mermaid
flowchart TD
    subgraph S1["Portal::start —— Plan 树的类型分派"]
        A["Portal::start(plan, context)"] --> B{"plan 的实际类型"}
        B -->|OtherPlan| C["PortalStmt 标签 PORTAL_CMD_UTILITY"]
        B -->|DDLPlan| D["PortalStmt 标签 PORTAL_MULTI_QUERY"]
        B -->|DMLPlan| E{"x 的 tag"}
        E -->|T_select| F["取 subplan_ 转成 ProjectionPlan<br/>再交给 convert_plan_executor"]
        E -->|T_Update| G["先把子扫描完整跑一遍<br/>收集所有命中 Rid"]
        E -->|T_Delete| H["同样先收集 Rid"]
        E -->|T_Insert| I["直接构造 InsertExecutor"]
        G --> G1["构造 UpdateExecutor<br/>传 tab_name set_clauses conds rids"]
        H --> H1["构造 DeleteExecutor<br/>传 tab_name conds rids"]
        F --> J["PortalStmt 标签 PORTAL_ONE_SELECT<br/>携带 sel_cols 和 aggregate_limit"]
    end

    subgraph S2["Portal::convert_plan_executor —— Plan 递归翻译成 Executor"]
        K["convert_plan_executor(plan)"] --> L{"plan 的实际类型"}
        L -->|ProjectionPlan| M["ProjectionExecutor"]
        L -->|"ScanPlan 标签 T_SeqScan"| N["SeqScanExecutor"]
        L -->|"ScanPlan 标签 T_IndexScan"| O["IndexScanExecutor"]
        L -->|JoinPlan| P["NestedLoopJoinExecutor<br/>先递归转换 left_ 和 right_"]
        L -->|SortPlan| Q["SortExecutor"]
    end
```

**要点**：UPDATE 和 DELETE 有个特殊处理 —— **先把子扫描完整跑一遍收集 Rid，再构造修改算子**，
避免边扫边改导致扫描器看到自己的修改。这是很典型的"读集合与写集合必须分离"。

---

## 图 6：执行层（execution/execution_manager.cpp）

```mermaid
flowchart TD
    A["Portal::run(stmt, ql_manager, txn_id, context)"] --> B{"portal 的 tag"}
    B -->|PORTAL_ONE_SELECT| C["QlManager::select_from"]
    B -->|PORTAL_DML_WITHOUT_SELECT| D["QlManager::run_dml"]
    B -->|PORTAL_MULTI_QUERY| E["QlManager::run_mutli_query"]
    B -->|PORTAL_CMD_UTILITY| G["QlManager::run_cmd_utility"]

    D --> D1["exec->Next 只调用一次"]
    E --> E1["SmManager 的 create_table drop_table<br/>create_index drop_index"]
    G --> G1["help show tables desc<br/>begin 置 txn_mode 为 true<br/>commit abort rollback"]

    C --> C1{"sel_cols 首个是聚合函数吗"}
    C1 -->|是| C2["聚合分支"]
    C2 --> C3["遍历全表 逐行累加<br/>COUNT SUM MIN MAX"]
    C3 --> C4["aggregate_limit 为 0 则不输出<br/>否则输出一行并打印记录数"]
    C1 -->|否| C5["先打印表头 同时写入 output.txt"]
    C5 --> C6["Volcano 循环"]
    C6 --> C7{"is_end 为假吗"}
    C7 -->|是| C8["Next 取一条 RmRecord"]
    C8 --> C9["对每列按 col.offset 取值<br/>value_to_string 格式化"]
    C9 --> C10["RecordPrinter 写入 data_send_<br/>同时 append 到 output.txt"]
    C10 --> C11["nextTuple 推进迭代器"]
    C11 --> C7
    C7 -->|否| C12["打印分隔线并输出记录条数"]
```

---

## 图 7：算子树与存储层调用链

```mermaid
flowchart TD
    A["执行的起点<br/>AbstractExecutor 接口"] --> B["beginTuple 定位到第一条"]
    B --> C["is_end 判断是否结束"]
    C --> D["Next 取当前记录"]
    D --> E["nextTuple 推进"]

    B --> F["SeqScanExecutor::beginTuple"]
    F --> F1["lock_shared_on_table 加表级共享锁"]
    F1 --> F2["构造 RmScan 从首页开始"]
    F2 --> F3["advance_to_match 逐槽位找匹配记录"]
    F3 --> F4["eval_conditions 过滤 where 条件"]

    D --> G["RmFileHandle::get_record(rid)"]
    G --> H["fetch_page_handle 校验 page_no 范围"]
    H --> I["BufferPoolManager::fetch_page"]
    I --> J{"page_table_ 命中吗"}
    J -->|命中| K["pin_count 加一<br/>replacer pin 移出淘汰候选"]
    J -->|未命中| L["find_victim_page"]
    L --> L1{"free_list_ 非空吗"}
    L1 -->|是| L2["取一个从未用过的空帧"]
    L1 -->|否| L3["replacer victim 取 LRU 队尾"]
    L2 --> M["update_page"]
    L3 --> M
    M --> M1["老页脏则 write_page 回写磁盘"]
    M1 --> M2["reset_memory 清零并改 id_"]
    M2 --> M3["disk_manager read_page 读入 4KB"]
    M3 --> K
    K --> N["返回 Page 指针"]

    H --> O["RmPageHandle 按页内布局切分<br/>page_hdr bitmap slots"]
    O --> P["get_slot 按 slot_no 定位记录"]
    P --> Q["深拷贝出 RmRecord"]
    Q --> R["unpin_page 归还帧"]
    R --> D
```

**要点**：get_record 里的深拷贝是**缓冲池与执行层的分界线**。拷贝完成之后立刻 unpin，
记录的生命周期就与帧无关了 —— 帧可以被淘汰换页，手里的 RmRecord 依然有效。

---

## 图 8：缓冲池中帧的状态机

```mermaid
stateDiagram-v2
    [*] --> Free: 构造时 65536 个帧全部进 free_list_
    Free --> Pinned: fetch_page 取空帧<br/>pin_count = 1
    Pinned --> Pinned: 再次 fetch_page 命中<br/>pin_count 加一
    Pinned --> Evictable: unpin_page 使 pin_count 归零<br/>replacer unpin 进入候选
    Evictable --> Pinned: fetch_page 命中<br/>replacer pin 摘出候选
    Evictable --> Evicting: victim 选中 LRU 队尾
    Evicting --> Pinned: update_page 加 read_page<br/>pin_count = 1
    Evicting --> Free: delete_page 或 close_file<br/>帧归还 free_list_
    Pinned --> Free: delete_page 且 pin_count 为 0
    note right of Evictable
        链表里的帧一定是 pin_count == 0
        排序依据是最后一次 unpin 的时刻
    end note
    note right of Evicting
        is_dirty 为真则先 write_page
        再 reset_memory 换成新页
    end note
```

---

## 图 9：事务状态迁移与加锁时机

```mermaid
stateDiagram-v2
    [*] --> DEFAULT: new Transaction 分配事务号
    DEFAULT --> GROWING: TransactionManager::begin
    GROWING --> GROWING: 继续加锁
    GROWING --> SHRINKING: 第一次 unlock 之后
    SHRINKING --> SHRINKING: 只允许放锁
    GROWING --> COMMITTED: commit
    SHRINKING --> COMMITTED: commit
    GROWING --> ABORTED: abort 或锁冲突
    SHRINKING --> ABORTED: abort
    note right of GROWING
        两阶段封锁的增长阶段
        此阶段禁止放锁
    end note
    note right of SHRINKING
        收缩阶段
        再请求新锁会抛
        LOCK_ON_SHIRINKING
    end note
```

各算子的加锁点：

```mermaid
flowchart LR
    A["SeqScanExecutor::beginTuple"] --> A1["lock_shared_on_table 表级共享锁"]
    B["IndexScanExecutor::beginTuple"] --> B1["lock_shared_on_table 表级共享锁"]
    C["InsertExecutor::Next"] --> C1["lock_exclusive_on_table 表级排他锁"]
    D["UpdateExecutor::Next"] --> D1["lock_exclusive_on_table 表级排他锁"]
    E["DeleteExecutor::Next"] --> E1["lock_exclusive_on_table 表级排他锁"]
    G["SmManager 的 DDL"] --> G1["lock_exclusive_on_table 表级排他锁"]
    H["行级锁 API<br/>lock_shared_on_record<br/>lock_exclusive_on_record"] --> H1["已实现但无任何调用点"]
    I["意向锁 API<br/>lock_IS_on_table<br/>lock_IX_on_table"] --> I1["已实现但无任何调用点"]
```

---

## 图 10：异常路径与收尾

```mermaid
flowchart TD
    A["client_handler 的 try 块"] --> B{"抛出了什么"}
    B -->|TransactionAbortException| C["响应写 abort"]
    C --> C1["txn_manager abort"]
    C1 --> C2["逆序遍历 write_set 反向补偿"]
    C2 --> C3["INSERT 删记录 DELETE 回插 UPDATE 写回旧值"]
    C3 --> C4["清空 write_set 状态置 ABORTED"]
    C4 --> C5["按 lock_set 释放全部锁"]
    C5 --> C6["output.txt 追加 abort"]
    B -->|std::exception| D["响应写 Error 加消息"]
    D --> D1["中止整个当前事务"]
    D1 --> D2["output.txt 追加 failure"]
    B -->|无异常| E["正常路径"]
    C6 --> F["隐式提交判定的检查点"]
    D2 --> F
    E --> F
    F --> G{"txn_mode 为 false 且<br/>状态不是 COMMITTED 或 ABORTED"}
    G -->|是| H["commit 先写日志落盘再放锁"]
    G -->|否| I["跳过提交"]
    H --> J["状态已终结则 delete 事务对象并复位 txn_id"]
    I --> J
    J --> K["send 响应给客户端"]
    K --> L{"send 完整吗"}
    L -->|否| M["退出循环 结束连接"]
    L -->|是| N["回到 receive_request 等下一条 SQL"]
    M --> O["连接收尾<br/>有未完成事务则 abort 并 delete"]
```

**要点**：**错误即中止整个事务**是有意的语义选择 —— 一条语句失败就放弃整个事务，而不是只放弃这条语句，
避免留下半成品状态。另外隐式事务的**提交发生在回包之前**，客户端收到成功时数据已经持久化。

---

## 附：分层函数清单

| 层 | 入口函数 | 文件 | 产出 |
| --- | --- | --- | --- |
| 网络 | receive_request / client_handler | rmdb.cpp | 一条完整 SQL 字符串 |
| 解析 | yy_scan_string / yyparse | parser/lex.l parser/yacc.y | ast::parse_tree |
| 语义 | Analyze::do_analyze | analyze/analyze.cpp | Query |
| 优化 | Optimizer::plan_query | optimizer/optimizer.h | Plan 树 |
| 计划 | Planner::do_planner | optimizer/planner.cpp | ProjectionPlan 或 DMLPlan 或 DDLPlan |
| 翻译 | Portal::start / convert_plan_executor | portal.h | PortalStmt 加算子树 |
| 驱动 | Portal::run | portal.h | 分派到 QlManager |
| 执行 | QlManager::select_from / run_dml | execution/execution_manager.cpp | data_send_ 缓冲区 |
| 算子 | AbstractExecutor::beginTuple / is_end / Next / nextTuple | execution/executor_*.h | RmRecord |
| 记录 | RmFileHandle::get_record 等 | record/rm_file_handle.cpp | RmRecord 深拷贝 |
| 缓冲 | BufferPoolManager::fetch_page / unpin_page | storage/buffer_pool_manager.cpp | Page 指针 |
| 磁盘 | DiskManager::read_page / write_page | storage/disk_manager.cpp | 4KB 裸字节 |
| 事务 | TransactionManager::begin / commit / abort | transaction/transaction_manager.cpp | 状态迁移与回滚 |
| 封锁 | LockManager::lock / unlock | transaction/concurrency/lock_manager.cpp | 锁授予或抛异常 |
| 日志 | LogManager::add_log_to_buffer / flush_log_to_disk | recovery/log_manager.cpp | WAL 落盘 |
