# RMDB 题目、代码归属与完成度说明

更新时间：2026-08-05

这份文档把课程实验报告、原始 RMDB 目录和当前 GitHub 仓库放在一起对照。它用于复习、面试准备和后续补全，不把 Git 提交账号直接当作代码作者证明。

## 1. 证据与归属口径

### 1.1 原始项目

原始目录为 `E:\Code\rmdb.tar\rmdb`。上传仓库的初始提交 `b6df74d` 基本整体导入了该目录中的：

- `src/` 源码
- `rmdb_client/`
- `deps/googletest/`
- CMake、License、课程 PDF 和项目图片

因此，题目一至题目十一的主体代码在 AI 整理仓库之前已经存在。

### 1.2 框架代码与课程实现

实验报告明确区分了“课程提供的框架”和“学生需要补充的接口”。本文将以下内容称为“你的课程实现（推断）”：

- 在原始目录中已经存在；
- 按实验报告明确属于学生需要实现的功能；
- 当前源码中具有对应实现。

教师框架、第三方库、生成器产物和样例测试仍归为原有项目内容。当前 Git 没有提供教师初始框架与学生提交之间的逐行历史，因此作者划分只能做到证据支持的功能级推断。

### 1.3 AI 后续工作

可以由 Git 直接确认的 AI 工作包括：

- 新增 `README.md`、`.gitignore` 和 `GEMINI_PROJECT_GUIDE.md`；
- 检查项目构建、单元测试、CTest 和 SQL 冒烟流程；
- 修复当前扁平有序索引模型下的 `IxScan` 推进逻辑；
- 补充索引句柄生命周期处理和索引扫描回归测试；
- 调整单元测试目标对 `index` 库的链接。

对应提交：

- `66768e6 add Gemini project learning guide`
- `bbf8466 complete index scan lifecycle and regression coverage`

`build-upload/` 和 `build-client-upload/` 是本地验证生成物，不是源代码功能。

## 2. 十一个题目的归属与完成度

## 题目一：存储管理

### 题目要求

实现磁盘管理器、LRU 页面替换、缓冲池管理器、定长记录管理和记录扫描，重点包括：

- 文件创建、打开、关闭、删除；
- 页面读写和页面分配；
- LRU `victim`、`pin`、`unpin`；
- 缓冲池页面换入、换出、脏页刷新、页面删除和并发控制；
- 记录的查询、插入、指定 RID 插入、删除、更新；
- 记录扫描器的初始化、推进和结束判断。

### 框架原有内容

类声明、页面结构、RID、位图、接口注释、替换器基类和 `unit_test.cpp` 中的参考测试。

### 推断为你的课程实现

- `src/storage/disk_manager.cpp`
- `src/storage/buffer_pool_manager.cpp`
- `src/replacer/lru_replacer.cpp`
- `src/record/rm_file_handle.cpp`
- `src/record/rm_scan.cpp`

这些文件中的核心接口在原始目录中已经有完整实现，且符合题目一的学生实现清单。

### 当前状态

基础存储和记录测试通过。当前仓库运行过原有存储、缓冲池并发和记录测试。

## 题目二：查询执行

### 题目要求

在题目一基础上支持元数据管理、DDL、DQL、DML和连接查询。测试点包括：

1. 建表、展示表、删表；
2. 单表插入和条件查询；
3. 单表更新和条件查询；
4. 单表删除和条件查询；
5. 连接查询。

### 框架原有内容

实验报告明确说明，框架已经提供：

- `create table` 的基本实现；
- `insert` 执行器；
- `insert`、`select`、`delete` 的部分分析代码；
- 基本查询计划结构。

### 推断为你的课程实现

- `src/system/sm_manager.cpp` 中的非索引表管理功能；
- `src/analyze/analyze.cpp` 中的 `UPDATE` 分析和语义检查；
- `src/execution/executor_seq_scan.h`；
- `src/execution/executor_nestedloop_join.h`；
- `src/execution/executor_projection.h`；
- `src/execution/executor_update.h`；
- `src/execution/executor_delete.h`；
- `src/optimizer/planner.cpp` 中的计划组装。

### 当前状态

基本查询链路已经可以运行。`logical_optimization()` 仍是空实现，但题目二的基础测试不要求完整逻辑优化。

## 题目三：BIGINT 类型

### 题目要求

支持有符号 8 字节 BIGINT，并支持建表、插入、查询、更新、删除和越界输入检查。

### 推断为你的课程实现

- `src/defs.h` 中的 `TYPE_BIGINT`；
- `src/parser/ast.h`、`src/parser/lex.l`、`src/parser/yacc.y` 中的类型和语法；
- 生成的 `src/parser/lex.yy.*`、`src/parser/yacc.tab.*`；
- `src/analyze/analyze.cpp` 中的整数范围处理和类型转换；
- `src/execution/executor_utils.h` 中的 BIGINT 比较；
- 相关输出和记录长度处理。

### 当前状态

源码已经包含 BIGINT 的 8 字节存储、范围判断和查询比较逻辑。

## 题目四：DATETIME 类型

### 题目要求

支持 `YYYY-MM-DD HH:MM:SS` 格式的 8 字节时间类型，并检查年份、月份、日期、闰年、时分秒和格式长度。

### 推断为你的课程实现

- `src/common/datetime_utils.h`；
- `src/parser/ast.h`、`src/parser/lex.l`、`src/parser/yacc.y`；
- `src/analyze/analyze.cpp` 中的字符串到时间值转换；
- `src/execution/executor_utils.h` 中的时间比较；
- `src/record_printer.h` 中的时间显示。

### 当前状态

源码包含合法性检查、时间编码和格式化输出。非法日期会进入错误路径。

## 题目五：唯一索引

### 题目要求

支持单列和多列唯一索引，以及：

- 创建、删除、展示索引；
- 单点查询和范围查询；
- 最左匹配；
- 插入、删除、更新时维护索引；
- 唯一性冲突检查。

### 框架原有内容

框架提供了索引文件头、索引节点结构、RID/Key 布局、索引管理器接口和 B+ 树相关函数声明。

### 推断为你的课程实现

- `src/system/sm_manager.cpp` 中的 `create_index`、`drop_index`、`show_index`；
- `src/execution/index_utils.h` 中的联合索引键构造；
- `src/execution/executor_insert.h`、`executor_update.h`、`executor_delete.h` 中的索引同步；
- `src/optimizer/planner.cpp` 中的索引匹配和最左前缀选择；
- `src/execution/executor_index_scan.h` 中的范围边界生成；
- `src/index/ix_index_handle.cpp` 中的唯一检查、单点查找、范围查找和有序条目维护。

### 当前状态

当前实现能够提供功能型唯一索引和范围查询，但不是完整磁盘 B+ 树。以下接口仍然是简化或空实现：

- `find_leaf_page`；
- `split`；
- `insert_into_parent`；
- `coalesce`；
- `redistribute`；
- `adjust_root`。

因此面试中应表述为“实现了唯一索引功能和范围查询”，不要表述为“完整实现了标准 B+ 树分裂合并”。

### AI 新增内容

AI 在 `bbf8466` 中修复了当前有序条目模型下的 `IxScan::next()`，补充句柄释放、索引头析构和范围扫描测试。这是后续维护，不是原始题目主体实现。

## 题目六：聚合函数

### 题目要求

支持 `COUNT`、`COUNT(*)`、`MAX`、`MIN`、`SUM`、别名和整数/浮点输出格式。

### 推断为你的课程实现

- `src/parser/ast.h`、`src/parser/yacc.y` 中的聚合语法和 AST 字段；
- `src/analyze/analyze.cpp` 中的聚合字段检查；
- `src/execution/executor_projection.h`；
- `src/execution/execution_manager.cpp` 中的聚合结果处理；
- 记录输出格式处理。

### 当前状态

源码包含聚合类型、聚合字段和聚合结果输出路径。

## 题目七：ORDER BY 与 LIMIT

### 题目要求

支持单列和多列排序、`ASC`、`DESC` 以及 `LIMIT`。

### 推断为你的课程实现

- `src/execution/execution_sort.h`；
- `src/optimizer/planner.cpp` 中的排序计划生成；
- `src/parser/ast.h`、`src/parser/yacc.y` 中的排序和限制语法；
- 查询执行阶段的排序结果输出。

### 当前状态

源码中已有独立 Sort Executor 和排序计划生成逻辑。

## 题目八：块嵌套循环连接

### 题目要求

实现超过内存数据规模时仍可工作的 Block Nested-Loop Join，支持等值和非等值连接。

### 推断为你的课程实现

`src/execution/executor_nestedloop_join.h` 中包含：

- 左表分块读取；
- 固定连接缓冲区；
- 右表逐批扫描；
- 连接条件判断；
- 无条件连接时的缓存路径。

### 当前状态

已有块式连接执行逻辑，属于原始项目中的重要可展示内容。

## 题目九：事务控制语句

### 题目要求

支持 `BEGIN`、`COMMIT`、`ABORT`，并在回滚时恢复插入、删除和更新操作。

### 推断为你的课程实现

- `src/rmdb.cpp` 中的事务生命周期；
- `src/transaction/transaction_manager.cpp`；
- `src/execution/executor_insert.h`；
- `src/execution/executor_update.h`；
- `src/execution/executor_delete.h`；
- `src/transaction/transaction.h` 中的写集合。

### 当前状态

源码包含事务开始、提交、回滚、写集合和索引同步回滚流程。

## 题目十：可串行化与死锁预防

### 题目要求

实现两阶段封锁、共享锁、排他锁，并用 no-wait 策略预防死锁，覆盖脏读、脏写、丢失更新、不可重复读和幻读场景。

### 推断为你的课程实现

- `src/transaction/concurrency/lock_manager.h`；
- `src/transaction/concurrency/lock_manager.cpp`；
- `src/transaction/transaction_manager.cpp`；
- 执行器中的表锁和记录锁调用。

### 当前状态

锁管理器包含表级锁、记录锁、锁兼容性判断、两阶段状态和冲突时 abort 的 no-wait 路径。

## 题目十一：系统故障恢复

### 题目要求

实现 WAL、日志缓冲、REDO/UNDO，并在系统异常终止后重启恢复一致状态。

### 推断为你的课程实现

- `src/recovery/log_manager.h`；
- `src/recovery/log_manager.cpp`；
- `src/recovery/log_recovery.h`；
- `src/recovery/log_recovery.cpp`；
- `src/rmdb.cpp` 中的启动恢复流程；
- 执行器和事务管理器中的 Insert/Delete/Update 日志写入。

### 当前状态

源码包含 Begin、Commit、Abort、Insert、Delete、Update 日志，以及恢复阶段的分析、REDO 和 UNDO。日志头中仍保留部分 TODO 注释，索引操作也没有展开成独立的物理日志记录，因此应表述为“完成基础 REDO/UNDO 恢复流程”。

## 3. 原有框架与第三方内容

以下内容不应作为个人重新实现的核心功能：

- `src/parser/lex.yy.*`、`src/parser/yacc.tab.*` 的生成器产物；
- `deps/googletest/` 第三方测试库；
- `src/common/config.h`、`src/storage/page.h` 等基础数据结构和配置；
- `rmdb_client/` 的基础客户端框架；
- 课程 PDF、项目图片、License 和示例测试框架。

## 4. AI 新增文件和代码清单

### 新增文件

- `README.md`：项目构建和启动说明；
- `.gitignore`：上传仓库的忽略规则；
- `GEMINI_PROJECT_GUIDE.md`：面向 VSCode/Gemini 的学习路线、阅读顺序和练习任务；
- 本文件：题目要求、代码归属和完成度整理。

### 修改文件

`bbf8466` 修改了：

- `src/CMakeLists.txt`；
- `src/index/ix_index_handle.cpp`；
- `src/index/ix_index_handle.h`；
- `src/index/ix_manager.h`；
- `src/index/ix_scan.cpp`；
- `src/unit_test.cpp`。

## 5. 面试版项目表述

> RMDB 是基于老师提供的数据库内核框架完成的课程项目。我主要完成了存储管理、记录管理、查询执行、BIGINT、DATETIME、唯一索引、聚合、排序、块嵌套循环连接、事务、并发控制和基础 REDO/UNDO 恢复。AI 后续协助我整理项目文档、验证构建和测试，并补充了索引扫描测试及资源生命周期处理。索引部分是功能型实现，标准磁盘 B+ 树的分裂和合并仍有简化。

## 6. 验证记录

在当前仓库中完成过：

- RMDB 构建通过；
- 原有 5 个单元测试通过；
- 新增索引扫描测试通过，总计 6 个单元测试；
- CTest `1/1` 通过；
- TCP 建表、插入、查询冒烟通过。

官方完整评分脚本没有随上传仓库提供，因此“功能已实现”和“通过课程全部隐藏测试”需要分开表述。
