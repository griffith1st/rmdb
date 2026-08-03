# RMDB 项目掌握与 Gemini 协作指南

这份文档服务于“只用公司现有 VSCode 和插件”的学习场景。目标不是先把环境装满，而是先能沿着一条 SQL 请求讲清楚数据如何经过解析、计划、执行、页缓存、磁盘、索引和事务。

## 1. 你要掌握到什么程度

达到下面的验收标准，就可以把项目讲完整：

1. 能解释 `SELECT`、`INSERT`、`UPDATE`、`DELETE` 从客户端到执行器的调用链。
2. 能画出 `BufferPoolManager`、`DiskManager`、`RmFileHandle`、`IxIndexHandle` 的关系，并说明页、帧、RID 的区别。
3. 能解释 Planner 何时选择顺序扫描，何时选择索引扫描，以及 Nested Loop Join 和 Sort Executor 的位置。
4. 能说明事务状态、锁、写日志、提交、回滚、崩溃恢复之间的关系。
5. 能从现有单元测试出发，设计一个能暴露边界问题的新测试，而不是只复述类名。

## 2. 一条 SQL 的真实链路

```text
rmdb_client/main.cpp
        |
        | TCP request
        v
src/rmdb.cpp
  yyparse / AST
        v
src/analyze/analyze.cpp       语义检查、列和条件补全
        v
src/optimizer/planner.cpp     逻辑计划、物理计划、索引选择
        v
src/portal.h                  Plan -> Executor
        v
src/execution/*               SeqScan / IndexScan / Join / Sort / DML
        v
src/system/*                  数据库、表、元数据管理
        +--> src/record/*       定长记录、RID、记录扫描
        +--> src/index/*        B+Tree 风格索引和范围扫描
        +--> src/storage/*      Page、DiskManager、BufferPoolManager
        +--> src/transaction/*  事务状态、锁和提交/回滚
        +--> src/recovery/*     WAL 日志、redo、undo
```

服务端入口是 `src/rmdb.cpp`。它创建各类 Manager，监听 TCP `8765`，每条请求都会建立 `Context`，完成解析、分析、计划和执行；单条 SQL 执行后会自动提交，显式事务则由 `TransactionManager` 维护。

## 3. 文件阅读顺序

### 第一遍：只看主干

按下面顺序打开文件，先不钻进每个细节：

1. `src/rmdb.cpp`：全局 Manager、连接线程、请求循环、恢复入口。
2. `rmdb_client/main.cpp`：客户端如何连接、发送 SQL、接收结果。
3. `src/portal.h`：计划如何转换为执行器。
4. `src/analyze/analyze.cpp`：AST 如何变成带有表列和条件的 Query。
5. `src/optimizer/planner.cpp`、`src/optimizer/plan.h`：计划类型和索引选择。
6. `src/execution/execution_manager.cpp`、`executor_*.h`：执行器的生命周期和输出。

第一遍结束时，你应该能回答：`SELECT` 为什么不会直接访问磁盘？

### 第二遍：看数据落盘

1. `src/storage/page.h`：页和页 ID。
2. `src/storage/disk_manager.*`：数据库文件、页读写和日志文件。
3. `src/storage/buffer_pool_manager.*`：页调入、置换、pin count、dirty page 和 flush。
4. `src/record/rm_defs.h`、`rm_file_handle.*`、`rm_scan.*`：记录布局和 RID 定位。
5. `src/system/sm_meta.h`、`sm_manager.*`：表、列、索引元数据如何持久化。

第二遍结束时，你应该能解释“记录更新后为什么既要改数据页，也要维护索引”。

### 第三遍：看索引、并发和恢复

1. `src/index/ix_defs.h`：索引页头、节点和键值布局。
2. `src/index/ix_index_handle.*`、`ix_scan.*`：查找、插入、分裂、删除和范围扫描。
3. `src/transaction/transaction.h`、`transaction_manager.*`：事务状态和写集合。
4. `src/transaction/concurrency/lock_manager.*`：表锁、记录锁和兼容性。
5. `src/recovery/log_manager.*`、`log_recovery.*`：日志记录、分析、redo、undo。

## 4. 没有 WSL、Docker 时的学习路径

### Level 0：只用 VSCode，始终可执行

- 使用 VSCode 的 `Go to Definition`、`Find All References`、调用层级和全文搜索。
- 从 `src/rmdb.cpp` 的 `SetTransaction` 开始，逐个跟进 `analyze->do_analyze`、`optimizer->plan_query`、`portal->start`、`portal->run`。
- 每读完一个模块，写一张“输入、核心状态、输出、失败路径”四列表格。
- 用 Gemini 解释你已经定位到的 30 到 80 行代码，要求它引用文件路径和函数名，不要让它凭项目名猜实现。

### Level 1：只使用公司已有命令

在 VSCode 的 PowerShell 终端检查，不安装新软件：

```powershell
cmake --version
g++ --version
cl
```

如果已有 CMake 和编译器，可以尝试：

```powershell
cmake -S . -B build
cmake --build build
```

项目服务端使用 POSIX socket、`pthread`、`unistd` 和 `readline`。如果公司 Windows 环境没有对应兼容层，停止在静态阅读，不要把时间花在绕过审批安装工具上；这不影响你掌握架构和面试表达。

### Level 2：已有可运行构建时

```powershell
.\build\bin\rmdb.exe .\study-db
.\build-client\rmdb_client.exe -h 127.0.0.1 -p 8765
```

Linux 风格的 `./build/bin/rmdb` 只适用于相应构建环境。运行时先用一个新的数据库目录，避免覆盖已有实验数据。

## 5. 练习任务

### 练习 A：追踪一条查询

选择：

```sql
select id, name from student where id = 1;
```

在代码中记录：

1. AST 节点类型和条件保存在哪里。
2. `Analyze` 如何补全列元数据。
3. `Planner` 如何决定 `T_SeqScan` 或 `T_IndexScan`。
4. `ProjectionExecutor` 从下层 executor 取出什么。
5. 结果如何写入 `Context::data_send`。

### 练习 B：解释一次缓存未命中

围绕 `BufferPoolManager::fetch_page` 写出状态变化：页不在内存、寻找 victim、必要时 flush、读盘、更新 page table、返回 Page。再说明 pin count 为什么会阻止页面被置换。

### 练习 C：从测试反推设计

阅读 `src/unit_test.cpp` 的 buffer pool 和并发测试，分别回答：测试前置状态是什么、测试改变了什么、断言保护了哪条不变量、失败时最可能是哪一层出错。

### 练习 D：设计一个回归测试

任选一个边界：重复释放页面、索引分裂后查找、删除最后一条记录、事务 abort 后索引恢复。先写“预期不变量”，再写测试，不要先写实现。

## 6. Gemini 提示词模板

### 代码讲解

```text
你是 C++ 数据库内核导师。只依据我随后粘贴的代码回答，不要猜测仓库中未提供的实现。
请按“调用者 -> 输入状态 -> 核心分支 -> 输出状态 -> 失败路径”解释。
每个结论都标注文件路径、函数名和大致代码位置；发现信息不足时列出需要补充的文件。
项目目标：解释 RMDB 的 [模块名]。
代码：
[粘贴 30-80 行]
```

### 追踪 SQL

```text
请追踪 RMDB 中这条 SQL 的完整路径：[SQL]。
按 parser、Analyze、Planner、Portal、Executor、Record/Index、BufferPool、Disk 的顺序输出表格。
对每一层写：输入对象、关键函数、可能的错误、我下一步应打开的文件。
不要把 PostgreSQL 的实现当成 RMDB 的实现。
```

### 面试模拟

```text
请面试我 RMDB 的 [BufferPool/索引/事务/恢复]。
一次只问一个问题，先等待我的回答；回答后按“正确点、遗漏点、代码证据、追问”点评。
如果我的说法与代码不一致，请引用具体函数纠正，不要用泛泛的数据库教材答案替代。
```

## 7. 掌握记录模板

每完成一个模块，在 `notes/`（不提交也可以）写下：

```text
模块：
入口文件和函数：
解决的问题：
输入和输出：
关键不变量：
失败/回滚路径：
我能用一句话解释：
仍然不懂的一个点：
```

最后的验收不是“看过所有文件”，而是能在白板上画出一条 SQL 请求，并在任意一个箭头处打开源码证明自己的说法。
