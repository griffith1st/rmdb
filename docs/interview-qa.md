# RMDB 面试问答准备

> 本文基于仓库当前 main 分支的真实代码撰写，所有结论都能用文件与行号复核。
> 写作原则：只讲代码里真实存在的东西——不把框架自带的说成自己写的，也不把「没实现」包装成「已实现」。
> 配套记录见 [修复与验证记录](maintenance-20260919.md) 与 [题目对照](requirements-audit.md)。
> 往深处追问见 [RMDB 面试项目深度准备](RMDB面试项目深度准备.md)，调用链图见 [SQL 执行流程图](sql-execution-flow.md)。

## 目录

- [0. 项目自述模板](#0-项目自述模板)
- [1. 一条 SQL 语句怎么跑通](#1-一条-sql-语句怎么跑通)
- [2. TransactionManager 在做什么](#2-transactionmanager-在做什么)
- [3. 缓冲池与 LRU 淘汰](#3-缓冲池与-lru-淘汰)
- [4. 「你在项目里发现过什么问题」](#4-你在项目里发现过什么问题)
- [5. 能力边界：必须主动说清楚的清单](#5-能力边界必须主动说清楚的清单)
- [6. 如果再给我两周](#6-如果再给我两周)

---

## 0. 项目自述模板

（先说系统，再说自己。下面第一段是事实描述，第二段的方括号由你按真实分工填写。）

RMDB 是一个教学用关系型数据库，C++17 实现，代码量约 1.1 万行，走的是教科书式分层架构：
网络层单连接单线程收发 SQL，解析层用 flex/bison 产出 AST，语义层做表/列/类型校验并生成 Query，
优化层生成 Plan 树，Portal 把 Plan 翻译成 Volcano 迭代器算子树，执行层拉取记录，
存储层是「磁盘管理器 → 缓冲池 → 记录/索引管理器」三级。

系统支持建表删表、增删改查、单列与复合唯一索引、聚合（SUM/MIN/MAX/COUNT）、多列排序与 LIMIT、
块嵌套循环连接，以及 BEGIN/COMMIT/ABORT 显式事务、表级两阶段封锁和基于 WAL 的崩溃恢复。

> **[待填]** 我在其中负责/主要参与的是：______（例如：缓冲池与 LRU 替换、唯一索引维护、WAL 恢复流程）。
> 建议按「我改了哪个文件、解决了什么可复现的问题、用什么测试证明」三段式讲，不要笼统说「我做了存储引擎」。

**为什么这样开头**：面试官最反感的是把课程框架的既有代码说成自己的成果。
先把「系统是什么」和「我做了什么」切开，可信度立刻不一样，后面被追问也不容易翻车。

---

## 1. 一条 SQL 语句怎么跑通

### 30 秒版（先给这个，面试官有兴趣再展开）

> 一条 SQL 从客户端经 TCP 进来后，服务端为每个连接起一个线程。这条语句依次走五步：
> flex/bison 解析成 AST → 语义分析把 AST 变成带类型的 Query → 优化器生成 Plan 树 →
> Portal 把 Plan 翻译成算子树 → QlManager 用 Volcano 模型的 beginTuple / is_end / nextTuple 循环拉取结果，
> 结果写进响应缓冲区并回传客户端。如果是隐式事务，**提交发生在回给客户端之前**。
> 全程靠四种数据结构串起来：AST → Query → Plan → AbstractExecutor，
> 一个 Context 贯穿始终携带事务、锁、日志和输出缓冲。

### 展开：十个阶段

下面每条都给了「文件:行号」，可以现场打开指给面试官看。

**① 接入**　rmdb.cpp:107 主循环 receive_request(fd, pending, request)。
因为 TCP 是字节流，一条 SQL 可能被拆成两个包、也可能两条 SQL 挤在一个包里，
所以这里用 0x00 字节作分隔符把收到的字节攒进 pending，凑齐一条才返回（rmdb.cpp:61-80）。
这是很多人会漏掉的真实工程问题。

**② 建上下文 + 开事务**　rmdb.cpp:125-127：

    Context statement_context(lock_manager.get(), log_manager.get(), nullptr, data_send, &offset);
    SetTransaction(&txn_id, context);

Context 只有五个字段（common/context.h:32-37）：锁管理器、日志管理器、当前事务、响应缓冲区、缓冲区长度。
SetTransaction（rmdb.cpp:83-91）判断当前是显式事务还是单条语句的隐式事务：
若事务不存在或已 COMMITTED/ABORTED，就 begin() 一个新事务并 set_txn_mode(false)。

**③ 解析**　rmdb.cpp:131-135。注意这里持有一把 buffer_mutex：

    std::unique_lock<std::mutex> parser_guard(buffer_mutex);
    std::unique_ptr<yy_buffer_state, decltype(&yy_delete_buffer)> buf(yy_scan_string(data_recv), yy_delete_buffer);
    if (yyparse() != 0) throw InternalError("SQL syntax error");

**flex/bison 生成的是全局状态、不可重入**，必须串行化；
buf 用 unique_ptr 加自定义删除器托管，保证异常路径也不泄漏。

**④ 语义分析**　rmdb.cpp:143 调用 analyze->do_analyze(ast::parse_tree) →
analyze/analyze.cpp 按 AST 节点类型分派：校验表存在、check_column 补全列归属并检测歧义列/不存在列、
get_clause 生成 Condition、check_clause 做类型检查并把字面量按列类型提升（INT → BIGINT/FLOAT）。

**⑤ 优化**　rmdb.cpp:146 调用 optimizer->plan_query(query, context)。
optimizer/optimizer.h 先分流：help / show / desc / begin / commit / abort 直接造 OtherPlan，其余交给 Planner::do_planner。
SELECT 的路径是 logical_optimization（**目前是空实现，直接返回**，optimizer/planner.cpp:145-151）→
physical_optimization → make_one_rel 生成扫描/连接计划 → generate_sort_plan 处理 ORDER BY / LIMIT →
最外层包一层 ProjectionPlan。

**⑥ Plan → 算子树**　rmdb.cpp:147 调用 portal->start(plan, context)。
portal.h 的 convert_plan_executor 递归把 ProjectionPlan / ScanPlan / JoinPlan / SortPlan
翻译成 ProjectionExecutor / SeqScanExecutor / IndexScanExecutor / NestedLoopJoinExecutor / SortExecutor。

**⑦ 执行**　rmdb.cpp:148 调用 portal->run(...)，按 PortalStmt 的 tag 分派到 QlManager：
SELECT → select_from，DML → run_dml，DDL → run_mutli_query，工具语句 → run_cmd_utility。

**⑧ 拉取记录**　execution/execution_manager.cpp 里是教科书式的 Volcano 循环：

    for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
        auto tuple = executorTreeRoot->Next();
        ...
    }

**⑨ 落盘**　算子树最底层的 SeqScanExecutor::beginTuple 建 RmScan，逐页用 Bitmap 找有效槽位，
BufferPoolManager::fetch_page 取帧，用完 unpin_page。修改走 RmFileHandle 的 insert / update / delete_record，
把帧标记为脏，由缓冲池在淘汰时回写。

**⑩ 提交并回包**　rmdb.cpp:179-200：

    // Persist an implicit commit before the client receives success.
    if (context->txn_->get_txn_mode() == false && ...) {
        txn_manager->commit(context->txn_, context->log_mgr_);
    }
    ... 发送响应

**顺序很关键**：先提交（提交日志 fdatasync 落盘）再回包，客户端收到「成功」时数据已经持久化。

### 高频追问

**Q：为什么语义分析和计划生成要分两层？合成一层不是更简单？**

分层让「正确性」和「效率」解耦。语义分析是一次性、必须做的检查（表存不存在、列歧义、类型是否可比），
错了就直接报错，与怎么执行无关；优化器则是可替换的策略——同一个 Query 可以生成顺序扫描或索引扫描两种计划。
如果把类型检查塞进优化器，换个优化规则就得重做一遍检查。
这也解释了为什么 logical_optimization 现在是空的：它是留给「规则」的位置，
删掉它不影响正确性，但保留它就保留了扩展点。

**Q：Volcano 模型（迭代器模型）好在哪？**

三个优点：**接口统一**（所有算子只暴露 beginTuple / is_end / nextTuple / Next，加新算子不改上层）、
**流式处理**（不需要把中间结果全物化，LIMIT 1 可以只读一行就停）、
**组合自由**（算子树的形状就是执行计划，改计划等于改树结构）。
代价是每行数据要穿过整棵树、虚函数调用开销大——所以真实数据库会在火山模型之上再做向量化或编译执行。

**Q：为什么解析器要加锁？**

flex/bison 生成的 yyparse 依赖全局状态，不是可重入的（除非用 reentrant 版本）。
本项目一个客户端一个线程，多连接并发解析会互相踩状态，所以用 buffer_mutex 把「解析」这一段串行化。
注意锁的粒度：**只锁住解析**，buf.reset() 和 parser_guard.unlock() 之后立刻放锁（rmdb.cpp:144-145），
优化和执行阶段是并发的。

**Q：读和 DDL 并发怎么办？**

用一把 catalog_mutex（std::shared_mutex）：DDL 语句（CreateTable / DropTable / CreateIndex / DropIndex）
取**写锁**，其余语句取**读锁**（rmdb.cpp:129-142）。
因为 DDL 会改目录元数据（表结构、索引定义），读锁保证查询期间目录不被改，
写锁之间互斥保证两次 DDL 不打架。这是典型的读写锁用法：读多写少，且读之间无冲突。

**Q：报错之后连接还能用吗？**

能，但**整个当前事务会被中止**（rmdb.cpp:160-174）：任何 std::exception 都会被转成 Error: ... 回给客户端，
同时调用 txn_manager->abort() 回滚。
这是有意的语义选择——一条语句失败就放弃整个事务，而不是只放弃这条语句，避免留下半成品状态。

---

## 2. TransactionManager 在做什么

### 30 秒版

> 它是事务的**生命周期编排者**，不是执行者。它不锁数据、不写日志内容、不改数据页，
> 只负责把 Transaction / LockManager / LogManager / 存储层在正确时机串起来。
> 对外只有三个方法：begin 发事务号并写 Begin 日志、commit 清 write_set 并集中放锁、abort 逆序回滚数据。
> 另外它维护一张全局事务表 txn_map。

### 核心数据结构

    static std::unordered_map<txn_id_t, Transaction *> txn_map;  // 全局事务表
    std::atomic<txn_id_t>    next_txn_id_{0};      // 事务号分发
    std::atomic<timestamp_t> next_timestamp_{0};   // 开始时间戳分发
    std::mutex latch_;                             // 只保护 txn_map

get_transaction(txn_id) 里有一句很关键的断言：

    assert(res->get_thread_id() == std::this_thread::get_id());

**事务与线程强绑定**——因为服务端是「一个客户端连接一个线程」，一个事务只允许创建它的那个线程操作。
这条断言把「事务对象被别的线程误用」这类并发 bug 变成快速失败。

### 三件事

**begin**　发号 → set_state(GROWING)（进入 2PL 增长阶段）→ 写 Begin 日志并把它接到 prev_lsn 链上 →
登记进 txn_map。prev_lsn 是把同一事务的日志串成链表的关键，崩溃恢复靠它回放。

**commit**　先清空 write_set（**只删对象、不改数据**，因为提交了修改本来就要保留；
此时 write_set 的作用只是「undo 日志的内存版」）→ 写 Commit 日志 → 置 COMMITTED →
遍历 lock_set **集中放锁** → 刷日志 → 从 txn_map 移除。

**abort**　逆序遍历 write_set 做反向补偿 → 写 Abort 日志 → 置 ABORTED → 放锁 → 刷日志 → 移除。

### 最容易忽略的一点：write_set 是执行器生产的

TransactionManager 只是**消费者**。真正往 write_set 里塞记录的是执行器自己：

    // execution/executor_insert.h
    context_->txn_->append_write_record(new WriteRecord(WType::INSERT_TUPLE, tab_name_, rid_));
    // execution/executor_update.h —— 改之前先把旧值存下来
    context_->txn_->append_write_record(new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rid, old_rec));
    // execution/executor_delete.h
    context_->txn_->append_write_record(new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, *rec));

WriteRecord 的结构就是一份 undo 日志（transaction/txn_defs.h）：
INSERT 只记「类型 + 表 + rid」，DELETE / UPDATE 还要带上**旧记录的值**。
所以「改数据前先记旧值」这个纪律，是**执行器**在遵守，不是事务管理器在保证——
这一点想清楚，事务回滚的链路才算真的懂了。

### 高频追问

**Q：为什么回滚必须逆序？**

因为同一个数据项可能被改多次。顺序执行 a=1 → a=2 → a=3，若顺着从头回滚，
第二条 undo 记录恢复的是 a=1，会把最终的 a=3 覆盖成错的中间态。
逆序回滚等价于「把操作栈弹空」，每一步都把状态退回上一个已知点，任意时刻中断都是自洽的。
代码上就是 write_set->back() 加 pop_back()。

**Q：回滚时为什么不写日志？**

运行时的 abort 是**就地撤销**（直接把旧值写回数据页），这条路径不需要日志——因为数据没坏，只是要撤销。
而崩溃恢复时的 undo 才需要日志，那时进程已经没了，只能靠 WAL 重建。
所以恢复流程是 RecoveryManager 的另一套逻辑，与运行时的 abort 职责分开。

**Q：UPDATE 回滚为什么要动索引？**

因为索引项和数据页必须一致。逐行 undo 的思路是：先删掉新值对应的索引项，写回旧记录，再按旧值重建索引项；
顺序不能乱，否则中途崩溃会留下「索引指向新值、数据是旧值」的不一致。
本项目现在的做法更保守：回滚时先恢复整个堆，再对涉及的每张表**整体重建索引**
（transaction/transaction_manager.cpp 的 rebuild_indexes），
这样能正确处理 1,2 → 2,3 这种唯一键整体位移，避免逐行 undo 时互相冲突。

**Q：事务提交和放锁的先后有意义吗？**

有。本项目是「提交日志落盘后再放锁」，保证别的线程通过释放的锁看到数据时，这次提交已经持久化了。
反过来说，如果先放锁再刷日志，其他人可能读到「尚未持久化的提交」，
一旦此时宕机就会出现「别人见过、重启后却没了」的丢失更新。

**Q：锁是谁加的？**

LockManager（transaction/concurrency/lock_manager.cpp），事务管理器只负责在结束时代为释放。
实际加锁点全在执行器里：SeqScan / IndexScan 取**表级共享锁**，
Insert / Update / Delete 取**表级排他锁**（见第 5 节「能力边界」）。

---

## 3. 缓冲池与 LRU 淘汰

### 30 秒版

> 缓冲池分三层：DiskManager 管裸磁盘 I/O，BufferPoolManager 管帧（frame）的分配与换入换出，
> Replacer 提供淘汰策略。核心是四个容器：帧数组、页号到帧号的哈希表、空闲帧链表、替换器。
> 每页用 pin_count 表示「正被使用，不许淘汰」，用 is_dirty 表示「改过，淘汰时要回写」。
> LRU 用「双向链表 + 哈希表存迭代器」实现，三个操作全 O(1)。

### 数据结构

    size_t pool_size_;                                        // 帧数 = BUFFER_POOL_SIZE = 65536
    Page  *pages_;                                            // 帧数组，一次 new Page[65536]
    std::unordered_map<PageId, frame_id_t, PageIdHash> page_table_;  // 页号 → 帧号
    std::list<frame_id_t> free_list_;                         // 从未用过的空闲帧
    Replacer *replacer_;
    std::mutex latch_;

Page 本体（storage/page.h）是内嵌定长数组：

    PageId id_;              // { fd, page_no }
    char   data_[PAGE_SIZE]; // 4096 字节，实际数据
    bool   is_dirty_;
    int    pin_count_;

因为是内嵌数组，new Page[65536] 是一整块连续内存，约 256 MB（65536 × 4 KB）。
页用 (fd, page_no) 标识，**用文件描述符而不是表名**——同一个物理文件被打开两次会算成不同的页。

### fetch_page：最核心的路径

    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {          // 命中
        Page *page = &pages_[it->second];
        page->pin_count_++;
        replacer_->pin(it->second);         // 从淘汰候选中摘掉
        return page;
    }
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) return nullptr;   // 无可用帧 → 返回空
    Page *page = &pages_[frame_id];
    update_page(page, page_id, frame_id);   // 回写老脏页 + 重置帧 + 改映射
    disk_manager_->read_page(...);          // 从磁盘读入
    page->pin_count_ = 1;
    replacer_->pin(frame_id);

find_victim_page 有**两级来源**：优先 free_list_（冷启动时前 65536 次缺页直接拿空帧，零淘汰代价），
空链表才问 replacer_->victim()。

**写回式（write-back）策略**体现在 update_page：脏页只在**被淘汰时**才落盘，正常修改只改内存并置脏。

### unpin_page：归还使用权

    page->is_dirty_ = page->is_dirty_ || is_dirty;   // 或运算：脏标记只增不减
    page->pin_count_--;
    if (page->pin_count_ == 0)
        replacer_->unpin(it->second);                // 只有归零才进入淘汰候选

上层调用约定很清晰：读路径 get_record 传 false，写路径 insert / update / delete_record 传 true。

### LRU 的实现（重点）

    std::list<frame_id_t> LRUlist_;                                            // 双向链表
    std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> LRUhash_;  // 帧号 → 链表迭代器

**这是「list + hash 索引」的经典组合，三个操作全部 O(1)。**
如果不存迭代器，pin 就得用 LRUlist_.remove(frame_id) 做 O(n) 线性扫描——
这是这个实现最核心的技巧。

**方向约定（容易记反）**：

    front (MRU，最近被 unpin)  ← ... →  back (LRU，最久未被 unpin)

- unpin 用 push_front，插到头部，表示「我刚被释放，是最新的」
- victim 取 LRUlist_.back()，队尾就是最久没人用的

三个成员函数：

    bool victim(frame_id_t *frame_id) {
        if (LRUlist_.empty()) return false;
        *frame_id = LRUlist_.back();
        LRUhash_.erase(*frame_id);
        LRUlist_.pop_back();
        return true;
    }
    void pin(frame_id_t frame_id) {          // 帧被占用 → 移出候选
        auto it = LRUhash_.find(frame_id);
        if (it == LRUhash_.end()) return;
        LRUlist_.erase(it->second);           // 用迭代器 O(1) 删除
        LRUhash_.erase(it);
    }
    void unpin(frame_id_t frame_id) {         // 帧空闲 → 加入候选
        if (LRUhash_.count(frame_id) != 0 || LRUlist_.size() >= max_size_) return;  // 幂等
        LRUlist_.push_front(frame_id);
        LRUhash_[frame_id] = LRUlist_.begin();
    }

### 高频追问

**Q：这个 LRU 是按「访问时刻」排序的吗？**

**不是，是按「unpin 时刻」。** 因为一个帧只要被 fetch_page 命中就会 replacer_->pin() 移出链表，
所以**链表里的帧必定是 pin_count_ == 0 的**；它被再次访问、再次 unpin 时才挪到队头。
这等价于「最后一次 unpin 时间」的 LRU 近似，是 CMU 15-445 / BusTub 的标准做法——
好处是省掉了每次访问都调整链表的开销。
面试时能主动指出这个细节，比背「LRU 是最近最少使用」有价值得多。

**Q：pin_count 的作用是什么？**

它是「引用计数 + 淘汰许可」。pin_count > 0 意味着有代码正拿着这个 Page* 读写，
此时淘汰它会让人读到已经被换走的帧，所以 pin / unpin 就是在维护这个许可。
配套的纪律是：**拿到 Page* 必须配对 unpin_page**——漏一次就是永久 pin 泄漏，
这个帧再也不会被淘汰，缓冲池有效容量越来越小。
项目里 is_record() 曾经就漏了 unpin，是一类典型 bug。

**Q：LRU 有什么问题？**

最大的问题是**顺序扫描污染**。全表扫描时每个页用完立刻 unpin 进队头，
会把整个缓冲池的有效工作集冲干净，之后点查全部 miss。
工业上的对策是 LRU-K（看最近 K 次访问，一次性访问的页不进入淘汰队列）、
CLOCK（用引用位近似，省链表开销）、或分段 LRU（把「新页」和「热页」分开放）。
本项目目前是朴素 LRU，这是可以主动说出来的一条改进点。

**Q：缓冲池的并发度如何？**

这是本项目**当前最大的性能短板**：fetch_page 里的 std::scoped_lock 从查表一直持有到
read_page 结束，也就是**整个磁盘 I/O 都在全局锁内**，65536 帧的大池子并发度实际被压成 1。
改进方向是把锁做细（分桶锁或每帧锁），并让 I/O 在锁外完成——
DiskManager 已经改用 pread / pwrite（无共享文件偏移），具备了这个条件。

---

## 4. 「你在项目里发现过什么问题」

这道题几乎必问。**回答的关键是：讲一个「你发现 → 定位 → 修复 → 验证」的完整闭环，而不是罗列名词。**
下面三条素材都满足「可用行号复核」。

### 素材 A：REPLACER_TYPE 的比较写反了（逻辑错误）

storage/buffer_pool_manager.h 的构造函数里：

    if (REPLACER_TYPE.compare("LRU"))
        replacer_ = new LRUReplacer(pool_size_);
    else if (REPLACER_TYPE.compare("CLOCK"))
        replacer_ = new LRUReplacer(pool_size_);
    else { replacer_ = new LRUReplacer(pool_size_); }

**问题**：std::string::compare 在相等时返回 0，而 if (0) 是假。
所以当 REPLACER_TYPE 取值为 "LRU"（common/config.h 的实际值）时，第一个分支**不成立**，
代码走进了 else if——判断完全反了。

**为什么之前没暴露**：三个分支都 new LRUReplacer，结果碰巧一样。
这正是这类 bug 危险的地方：**它不影响当前行为，只让「策略选择」这个功能变成摆设**。
一旦有人实现了 CLOCKReplacer 并改配置，会发现配置根本不生效，
而且很难往「比较函数写反」这个方向想。

**修复**：把 compare(...) 换成 ==，语义显式化。

**怎么讲这个故事**：强调「我从『这段代码想表达什么』出发，而不是从『有没有崩』出发」——
能优雅地发现这种静默失效，比修一个崩溃更能体现 code review 能力。

### 素材 B：索引层缺少空指针检查（内部不一致）

index/ix_index_handle.cpp：

    IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
        Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
        return new IxNodeHandle(file_hdr_, page);      // page 可能是 nullptr
    }

**为什么这是真问题**：BufferPoolManager 的 fetch_page / new_page 在**没有可用帧时返回 nullptr**
（这是有意的 API 契约，单元测试里就断言了这一点）。
项目里其他地方都处理了：RmFileHandle 的 fetch_page_handle 和 create_new_page_handle 判空后抛 InternalError，
就连索引层自己的**消费者** release_node_handle 也写了 if (node.page == nullptr) return;
**唯独这两个生产者不判**——消费者在防御一个生产者不阻止的空值，这是明显的内部不一致。

**诚实的补充**（面试时一定要说，否则被追问会很被动）：
这两个函数目前**没有被任何代码调用**——索引的实际实现是内存里的有序 std::vector（g_index_entries），
IxNodeHandle 那套 B+ 树是框架留下的骨架。
所以这是**潜在的防御性缺陷，不是线上崩溃**。修它是为了让「生产者和消费者的契约一致」，不是救火。

> **面试技巧**：主动交代「这个函数其实没被调用」，比被面试官发现你在夸大严重性要好得多。
> 你展示的是「我知道这段代码在系统里的真实位置」，这恰恰是有经验的工程师和新手的区别。

### 素材 C（方法论）：一次被我自己推翻的结论

这一条很适合回答「**你怎么保证自己的判断是对的**」。

前期我基于一份**较早的项目副本**读代码，得出了三个结论：
磁盘 I/O 用 lseek + read 存在共享偏移问题、索引层会段错误、disk_manager 没有重试逻辑。
后来对当前 main 分支逐条复核时发现：**DiskManager 已经改成了带 EINTR 与短读写重试的 pread / pwrite**——
第一个结论完全不成立，是基于过期代码的误判。

**我的处理方法**：（1）把所有结论逐条回到真源重新验证，而不是「看起来对就写进文档」；
（2）用**按行号加内容核对**而不是靠记忆；（3）对自己已经说出口的结论主动更正。
**教训**：代码审查的第一原则是**确认你读的是哪个版本**。
在有多个副本、多个分支的项目里，「我读过这个文件」和「我读的是这个分支的这个文件的这一版」是两件事。

---

## 5. 能力边界：必须主动说清楚的清单

面试官最怕候选人把没做的东西说成做了。**主动划边界反而加分**，因为这说明你真的理解自己的系统。
以下都是本仓库当前的**真实状态**：

| 能力 | 真实状态 |
| --- | --- |
| **索引结构** | 是**内存中的有序 std::vector**（g_index_entries），**不是磁盘 B+ 树**。IxNodeHandle 的 B+ 树是框架骨架、未被调用。插入/删除有 vector 搬移成本，内存占用随数据量增长，开库时重建。 |
| **逻辑优化** | Planner::logical_optimization **是空实现**（optimizer/planner.cpp:145-151），只做物理优化。谓词下推是 pop_conds / push_conds 在物理计划阶段做的，不是独立的逻辑优化规则。 |
| **索引选择规则** | 很保守：要求**索引前缀列全部有等值条件**才走索引扫描（Planner::get_index_cols）。范围条件只吃最左一列，不等于条件不算，也不会重排 where 条件的顺序。没有基于代价的优化（CBO），没有统计信息。 |
| **连接算法** | 只有**块嵌套循环连接**（约 8 MiB 左侧分块）。没有哈希连接、排序合并连接。 |
| **并发控制** | **表级两阶段封锁 + no-wait**。行级锁 API（lock_shared_on_record / lock_exclusive_on_record）和意向锁 API（lock_IS_on_table / lock_IX_on_table）**都实现了但没有任何调用点**——多粒度锁目前退化成表级 2PL，这会限制并发性能。 |
| **隔离级别** | 声明为 SERIALIZABLE，靠表级 2PL 实现。没有 MVCC。 |
| **恢复** | 是**完整 WAL 重放**，**不是 ARIES**：没有检查点、页 LSN、CLR、日志压缩或校验和。恢复时间随日志历史线性增长。 |
| **聚合** | 支持 SUM / MIN / MAX / COUNT 和 AS 别名，**没有 GROUP BY**。 |
| **DDL 与事务** | DDL 只支持自动提交，**不支持在显式事务里回滚**，也没有保存点（savepoint）。 |
| **磁盘空间** | DiskManager::deallocate_page 是空实现，**不回收页号空间**。 |
| **缓冲池并发** | 全局锁覆盖磁盘 I/O，并发度实际为 1（见第 3 节）。 |

**怎么用这张表**：不要等面试官问出来才承认。
讲到某个模块时主动补一句「这里我做的是 X，还没做 Y，因为 Z」，然后用「如果要做我会怎么做」接住——见下一节。

---

## 6. 如果再给我两周

面试官问「这个项目还有什么不足」时，**给出可执行的改进路径**，比说「时间不够」好一百倍。

1. **把索引换成真的 B+ 树**：这是最大的一块。现状是内存 vector，
   要做的是页式节点、分裂/合并、持久化与崩溃一致性。收益不只是性能——
   它是「存储引擎」这个方向最硬的基本功，也是能撑起一整场面试的深度话题。

2. **细化缓冲池的锁**：把 BufferPoolManager 的全局 latch_ 拆成分桶锁或每帧锁，
   配合已经就位的 pread / pwrite 让磁盘 I/O 移出临界区。
   这件事**投入小、收益直接**（并发度从 1 提到接近核数），适合作为第一个动手项。

3. **把替换策略真正做成可插拔**：修好 REPLACER_TYPE 的判断之后，
   实现一个 CLOCKReplacer，然后做 A/B 对比实验——这正好能回答「LRU 有什么问题」那个追问，
   并且用数据说话：顺序扫描负载下 CLOCK 的命中率不一定比 LRU 差，但开销更低。

4. **实现基于代价的优化（CBO）**：给表和列加统计信息（行数、不同值个数、直方图），
   让 get_index_cols 从「能不能用索引」升级为「用索引划不划算」。
   小表走索引反而更慢，现在的规则一定会选错。

5. **给恢复加检查点**：当前每次重启都要重放全部 WAL。
   加一个 fuzzy checkpoint 就能把恢复时间从「正比于日志长度」降到「正比于活跃事务」。

6. **补行级锁**：让 executor_*_scan.h 里那些已经写好但没人调的 lock_shared_on_record
   真正用起来，配合意向锁实现层次化的多粒度封锁。

---

## 附：现场演示建议

如果允许带电脑，准备三条命令，**在真实数据上跑**比讲十页 PPT 有效：

1. 建库、建表、建索引、插入、查询 —— 走通主链路
2. SHOW INDEX / DESC table —— 证明目录元数据是活的
3. 跑一遍 ctest，展示 7 个测试套件、56 个用例全绿

重点演示**索引加速比**（仓库里有现成的基准数据，见 [index-benchmark.json](index-benchmark.json)：
1 万行数据下，无索引 0.484s vs 有索引 0.150s）。
**有数字的性能结论**永远比「我做了索引优化」有说服力。
