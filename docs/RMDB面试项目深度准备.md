# RMDB 面试项目深度准备

> 这是 [面试问答](interview-qa.md) 的**深挖篇**，面向"把这个项目讲成一整场面试"的目标。
> [面试问答](interview-qa.md) 解决"怎么开场、怎么讲主线、怎么划边界"；
> [SQL 执行流程图](sql-execution-flow.md) 提供十张可现场指认的调用链图；
> **本文解决"面试官往深处追，你还能撑多久"**。
>
> 本文所有结论都对应仓库当前 main 分支的真实代码，可打开文件核对；
> 所有实测数据（ASan 报告、内存地址、代码统计）都在本机跑过。

## 目录

- [第一部分 项目叙事](#第一部分-项目叙事)
- [第二部分 五个深挖专题](#第二部分-五个深挖专题)
  - [专题一 帧与页](#专题一帧与页缓冲池抽象的地基)
  - [专题二 pin 的并发难题](#专题二pin-的并发难题)
  - [专题三 两个 data](#专题三page_data-与-rmrecorddata缓冲池与执行层的分界线)
  - [专题四 PageIdHash](#专题四pageidhash一个哈希函数里的三种缺陷)
  - [专题五 Context](#专题五context参数对象与两阶段初始化)
- [第三部分 工程方法论](#第三部分工程方法论面试官最看重的部分)
- [第四部分 问答速查](#第四部分问答速查)
- [第五部分 边界与路线](#第五部分边界与路线)

---

# 第一部分 项目叙事

## 1.1 三分钟版本

> RMDB 是一个 C++17 写的教学关系型数据库，约 1.1 万行，标准分层架构：
> 网络层一个连接一个线程收发 SQL，解析层用 flex/bison 产出 AST，语义层做表/列/类型校验生成 Query，
> 优化层生成 Plan 树，Portal 把 Plan 翻译成 Volcano 迭代器算子树，执行层拉取记录，
> 存储层是"磁盘管理器 → 缓冲池 → 记录/索引管理器"三级。
>
> **我想重点讲的是存储和事务这条线**：缓冲池的页帧管理、LRU 淘汰、pin 生命周期，
> 以及事务的 WAL 写入时序和回滚补偿。
> 在做的过程中我修了几个静默失效的缺陷，也发现了一些结构性的设计边界。

三句话定调：**讲系统全貌 → 聚焦一条纵深线 → 给出可验证的贡献**。

## 1.2 为什么这个项目有纵深

它覆盖了数据库最核心的五个子系统，而且**每一层都有可以往下追三层的细节**：

    SQL 文本
      → 解析      （拆包粘包、flex/bison 不可重入）
      → 语义分析  （列绑定、歧义检测、类型提升）
      → 优化      （谓词下推、索引选择、连接顺序）
      → 执行      （Volcano 模型、算子组合）
      → 事务      （2PL、WAL 时序、回滚补偿）
      → 缓冲池    （pin/淘汰/写回、并发）
      → 磁盘      （页面布局、pread/pwrite）

换句话说：**面试官从任何一层切进来，你都有东西可讲**。这比"我调过某个框架的 API"强得多。

## 1.3 主动暴露边界是加分项

面试官最怕的是候选人把没做的东西说成做了。**主动划边界反而建立信任**，因为这说明你真的理解自己的系统。

本文和 [面试问答](interview-qa.md) 里那张"能力边界表"（索引是内存 vector 不是 B+ 树、
logical_optimization 是空实现、行级锁 API 零调用、恢复不是 ARIES）**要主动说，不要等被问出来**。

配套话术：

> "这块我做的是 X，还没做 Y，因为 Z。如果要做，我会先 ______。"

用"如果要做"接住，就把一个减分项变成了展示判断力的机会。

---

# 第二部分 五个深挖专题

# 专题一：帧与页——缓冲池抽象的地基

## 一句话答案

> **页是磁盘上的逻辑单位（第几页），帧是内存里的物理槽位（桌上第几个位置）。**
> page_table_ 是那张写着"第 7 页现在放在第 3 个位置"的索引卡，淘汰就是腾位置放别的页。

## 代码证据

| | 页 Page | 帧 Frame |
| --- | --- | --- |
| 位置 | 磁盘 | 内存 |
| 身份 | PageId{fd, page_no} | frame_id_t，就是个数组下标 |
| 数量 | 理论上无限 | 固定 65536 |

    // storage/page.h —— 帧就是这块内存
    char data_[PAGE_SIZE] = {};   // 内嵌定长数组，不是指针
    PageId id_;                   // 这个帧现在装的是哪一页
    bool is_dirty_;
    int pin_count_;

    // buffer_pool_manager.h —— 这一次 new 就是整个缓冲池
    pages_ = new Page[pool_size_];                          // 65536 × 4KB ≈ 256 MB
    std::unordered_map<PageId, frame_id_t, PageIdHash> page_table_;   // 页号 → 帧号
    using frame_id_t = int32_t;                             // common/config.h

## 为什么必须区分

**理由一：页号不能当地址用。** 拿到 page_no = 7 不能解引用——它可能根本不在内存。
必须先查 page_table_。反过来 frame_id = 3 也不能反推页号，因为同一个帧会先后装不同的页。
所以每个帧得自己记住身份，这就是 Page::id_ 存在的理由，也是 update_page 里这行的意义：

    page->id_ = new_page_id;      // 淘汰的本质就是改身份

**理由二：解耦磁盘规模和内存容量。** 表文件可以有 10 亿页，池子只有 65536 帧。
这个"多对一、动态变化"的关系必须有显式结构表达，就是 page_table_。

**理由三：pin 只有落在帧上才成立。** 不能淘汰的是"桌上这块内存"，不是"书里这个逻辑页"。
同一个页在内存最多一个副本，所以钉住帧等于钉住页。

## 追问链

**Q：为什么 Page 用内嵌数组而不是 char* ？**

内嵌数组：一次分配、无指针追逐、帧头和数据相邻对 cache 友好、new Page[N] 天然成立。
指针版：Page 可拷贝但代价是 65536 次额外分配，且一旦有人按值拷贝 Page，就会共享 buffer 出 bug。

代价是 **Page 不可拷贝**（拷 4KB 太贵），全项目只能传 Page* / Page&。

**Q：为什么用 fd 而不是表名标识页？**

PageId{fd, page_no} 里的 fd 是**打开文件后的文件描述符**。用 fd 的好处是文件被关闭再打开、
或者表被 drop 重建，标识会自然变化，不会出现"表名相同但文件内容已经换了"的歧义。
代价是同一个物理文件被打开两次会算成两页——但项目用 path2fd_ 保证了同名文件只打开一次。

**Q：256 MB 的池子为什么要一次性分配？**

三个原因：一是避免运行期碎片和 realloc；
二是**帧地址必须稳定**——Page* 会被执行器长期持有，帧能移动的话 pin 机制就得推倒重来（见专题二）；
三是 Page 内嵌 4KB 数组，本来就不能放进会重新分配的容器里。

**Q：帧会不会回收到 free_list_ ？**

会，但**只有两条路径**：delete_page（页被删除）和 close_file（文件被关闭）。
正常淘汰**不会**把帧还给 free_list_ —— 它是直接被 update_page 换成新页复用。
所以 find_victim_page 的两级来源（先 free_list_ 再 replacer）中，free_list_ 只在冷启动和删页后非空。

---

# 专题二：pin 的并发难题

## 一句话答案

> RMDB 现在用**一把全局锁**保证正确——pin_count_ 的全部 11 个访问点都在 latch_ 临界区内，
> 所以没有数据竞争。代价是整个缓冲池并发度为 1，而且锁覆盖了磁盘 I/O。
> **要改细，难点不在计数本身，而在于"判定可淘汰"和"占为己有"必须原子。**

## 现状（可核对）

pin_count_ 是普通 int（page.h:89），所有访问都在持锁函数内：

    storage/buffer_pool_manager.cpp:27,39,52,64,68,69,97,113,121,143

7 个对外函数全部 std::scoped_lock lock{latch_}。
其中 fetch_page 的锁从查表一直持有到 read_page 返回（cpp:35-51），**磁盘 I/O 也在锁里**。

## 为什么不能简单把锁改细

**核心矛盾：TOCTOU。**

    // 线程 T1（淘汰者）                // 线程 T2（访问者）
    if (page->pin_count_ == 0) {        // T1 判定：可淘汰
                                        page = fetch_page(pid);   // T2 抢先 pin 住
        update_page(page, other_pid);   // T1 把它换成了别的页
                                        ... use page->data_ ...   // T2 读写的是别的页
    }

这比普通 TOCTOU **危险得多**：T2 手里那个 Page* 仍指向合法内存，**不会段错误，只会静默读写错误数据**。
等发现数据对不上，现场早没了。

## 一个必须先成立的前提

注意上面例子里 T2 的指针之所以危险，是因为**帧的内容变了但地址没变**。

反过来说，如果帧地址会移动，pin 就必须用别的机制（句柄 + 二次查表）。
pages_ = new Page[pool_size_] **一次性分配、永不移动**——"帧地址稳定"是能用裸指针实现 pin 的隐含前提。
**想改成动态扩容帧数组，整个 pin 机制都得推倒重来。**

## 四个方案（由易到难）

### 方案 A：分桶锁（推荐的第一步）

按 hash(page_id) % N 把 page_table_、free_list_、replacer 分成 N 个桶，每桶一把锁。
命中路径只锁一个桶，并发度从 1 提到 N。

**难点是淘汰可能跨桶**（本桶没帧可用）。做法是每桶自给自足（各有帧配额和 replacer），
或者保留全局空闲帧池兜底。

### 方案 B：每帧状态机 + EVICTING 中间态

每帧一个状态：FREE / PINNED / EVICTING。

- 淘汰者 CAS 把 FREE 改成 EVICTING，**谁抢到谁负责**，然后**在锁外**写回磁盘，完成后再置回 FREE
- 访问者 pin 时看到 EVICTING 就等待或换帧

**EVICTING 这个中间态把"正在被换出的帧"从可 pin 集合摘出去了**，堵住了 TOCTOU 窗口。

### 方案 C：真实数据库怎么做

以 PostgreSQL 的 buffer manager 为代表：

1. **分区**：缓冲池切成若干分区（PG 默认 128 个），每分区一把轻量锁保护映射表和"时钟针"。
   **pin 是分区锁内的极短临界区**，只做"查表 + 计数加一"。
2. **pin 计数放进一个原子状态字**（和 locked / dirty / valid / io_in_progress 打包），而不是独立的 int。
3. **IO_IN_PROGRESS 标志 + 条件变量**：谁要读盘谁先置位，别人 pin 到就等条件变量，**磁盘 I/O 全在锁外**。
4. **淘汰用 CLOCK 时钟扫描，不用严格 LRU。**

第 4 点值得展开，因为它回答了一个常见追问：

> **为什么真实系统几乎不用严格 LRU？** 一半是命中率（顺序扫描污染），
> **另一半是并发**——维护一条全局 LRU 双向链表，每次访问都要改链表、都要抢锁，
> 锁争用会成为新瓶颈。CLOCK 只需要一个引用位和一根针，扫描时批量推进，锁持有时间短得多。

RMDB 的 LRUlist_ + LRUhash_ 在单线程里 O(1) 很优雅，但它天生是**全局可变状态**，很难分片。

### 方案 D：把 pin 做成 RAII（正确性，比性能更重要）

    class PageGuard {           // RMDB 目前没有这个东西
        BufferPoolManager *bpm_; PageId pid_;
      public:
        ~PageGuard() { bpm_->unpin_page(pid_, dirty_); }
    };

并发下 **pin 泄漏的后果比单线程严重得多**：一个泄漏的帧永久不可淘汰，可用帧持续减少，
最终 fetch_page 全线返回 nullptr，整个数据库退化——**全程没有报错，只是越来越慢然后集体失败**。

这也解释了为什么 fetch_page 返回 nullptr 是**有意的契约**（单元测试里就断言了它），
以及为什么调用方必须判空 —— RmFileHandle 判了，而 IxIndexHandle 曾经没判（见 [面试问答](interview-qa.md) 素材 B）。

## 回到 RMDB 的最小改造路径

1. **pin_count_ 不能简单改成 std::atomic&lt;int&gt;**。单看递增是原子的，
   但"判断淘汰"依赖的是**计数 + replacer 决策的联合状态**，只把计数原子化解决不了 TOCTOU，
   反而给人"已经线程安全"的错误信心。
2. 先改**分桶锁**，投入小收益直接。
3. 把 read_page / write_page 移出临界区 —— **前提已经就位**：
   main 上 DiskManager 已经改用带重试的 pread/pwrite（无共享文件偏移），正是为了让 I/O 能安全并发。
4. 顺手补 **RAII guard**，把"记得 unpin"从人的纪律变成编译期保证。

---

# 专题三：Page::data_ 与 RmRecord::data——缓冲池与执行层的分界线

## 先纠正一个常见混淆

代码里**没有** char* data_。有两个名字很像、本质完全不同的东西：

| 名字 | 声明 | 类型 | 归属 |
| --- | --- | --- | --- |
| Page::data_ | char data_[PAGE_SIZE] | **定长内嵌数组** | 缓冲池的帧 |
| RmRecord::data | char* data = nullptr | **堆指针** | 执行层的记录 |

## Page::data_ 被谁用

**只有两处：**

**A. Page 自己内部**（storage/page.h）：
- get_data() —— 唯一公开出口
- reset_memory() —— 换页时 memset 清零
- get_page_lsn() / set_page_lsn() —— 读写前 4 字节（**注意：这两个是死代码，见下文**）

**B. BufferPoolManager**（靠 friend class BufferPoolManager 破例）—— 5 处，全是整页 4KB 裸搬运：

    cpp:19   update_page     淘汰脏页  → write_page(..., page->data_, PAGE_SIZE)
    cpp:51   fetch_page      缺页读入  → read_page (..., page->data_, PAGE_SIZE)
    cpp:82   flush_page
    cpp:133  flush_all_pages
    cpp:150  close_file

**这就是缓冲池设计的核心：它对页里装的是什么一无所知**，只负责把 4096 字节在磁盘和内存之间搬。

**C. 上层全部走 get_data()，各自解释同一块 4KB：**

    // record/rm_file_handle.h:33-34 —— 记录页布局
    page_hdr = (RmPageHdr *)(page->get_data() + OFFSET_PAGE_HDR);   // 偏移 4
    bitmap   = page->get_data() + sizeof(RmPageHdr) + OFFSET_PAGE_HDR;
    slots    = bitmap + file_hdr->bitmap_size;

    // index/ix_index_handle.h:75-76 —— 索引页布局
    page_hdr = reinterpret_cast<IxPageHdr *>(page->get_data());     // 偏移 0！
    keys     = page->get_data() + sizeof(IxPageHdr);

**埋着一个隐患**：两个页头起始偏移不一致（记录页从 4 开始让出 LSN，索引页从 0 开始），
而 Page::set_page_lsn() 恰恰往 [0,4) 写 —— 真给索引页调它就会覆盖 IxPageHdr 的头 4 字节。

我 grep 过：get_page_lsn / set_page_lsn **除了定义处没有任何调用点**，所以现在不会炸。
但这暴露了结构性问题：**"偏移 4 留给 LSN"这个约定只对记录页成立，Page 类本身没有能力强制它**。

## RmRecord::data 被谁用（这才是那个 char*）

record/rm_defs.h:37-89，也是全项目**唯一有正确拷贝语义**的地方（深拷贝 + 自赋值保护 + delete[]）。

**① 数据出入口**：

    rm_file_handle.cpp:16
    get_record:  make_unique<RmRecord>(record_size, page_handle.get_slot(rid.slot_no))

**这一步是缓冲池与执行层的分界线，也是整个设计里最关键的一个动作。**

**② 执行器的列访问 rec->data + col.offset**（几十处）：

    execution_manager.cpp:199/263   char *rec_buf = Tuple->data + col.offset;
    executor_utils.h:61/72          谓词求值
    executor_nestedloop_join.h:36   左右记录拼成一条
    executor_projection.h:60/64     投影
    execution_sort.h:45             排序比较
    executor_update.h:31/35         算术更新 SET score = score + 5

col.offset 是列在**记录内的字节偏移**，由 TabMeta 在元数据里维护 —— 这是"定长记录"的红利：
取值不需要解析，直接指针加法。

**③ 索引键构造与日志**：

    transaction_manager.cpp:28 / sm_manager.cpp:44   make_index_key(index, rec->data)
    log_recovery.cpp:164/166 / log_manager.h        恢复与日志序列化

## 设计取舍：拷贝式 vs 零拷贝式

**拷贝式（当前做法）**：get_record 深拷贝一份，之后立刻 unpin_page。
记录的生命周期与帧**完全解耦** —— 你可以放掉 pin、帧可以被淘汰换页，手里的 RmRecord 照样有效。
代价是每条记录多一次 memcpy 加一次堆分配。

**零拷贝式**：直接返回 page_handle.get_slot(rid.slot_no) 指针。省掉拷贝，
但**必须把 pin 持有到记录用完为止** —— pin 的生命周期就泄漏到了执行器的调用栈里，
异常路径、提前 return、算子嵌套每一处都得配对 unpin。在带并发和异常的代码里，这是极难维护的契约。

RMDB 选了前者，所以 get_record 里那对 fetch_page_handle / unpin_page
才能紧挨着写在同一行附近，不需要上层操心。

> **用一次 memcpy 换掉一个跨模块的生命周期契约，这笔交易是划算的。**

## 顺带：三种"悬垂"性质完全不同

| 形态 | 泄漏？ | 危险 |
| --- | --- | --- |
| 返回栈变量地址 return &local | 不漏 | 读栈垃圾 |
| 返回指向帧的裸指针（假如 get_record 这么做） | 不漏 | **帧被淘汰后内存还在，内容已是别的页** |
| new 了不 delete | **漏** | 对象永不析构，成员指针悬垂 |

第二种最阴险：**不泄漏、不崩溃、只是数据静默错乱**。这是"拷贝式"设计的真正动机。

---

# 专题四：PageIdHash——一个哈希函数里的三种缺陷

## 它做什么

    struct PageIdHash {
        size_t operator()(const PageId &x) const { return (x.fd << 16) | x.page_no; }
    };

**位段打包**：把两个 32 位字段塞进互不重叠的位区间。

    63                    31        16        0
    ┌─────────────────────┬──────────┬─────────┐
    │      (未使用)        │    fd    │ page_no │
    └─────────────────────┴──────────┴─────────┘

只要各自不溢出 16 位，不同 (fd, page_no) 必得不同结果 → **零碰撞的单射编码**。

实测：

    fd=1, page_no=5      → 65541   = 0x00010005
    fd=2, page_no=3      → 131075  = 0x00020003
    fd=0, page_no=65541  → 65541   = 0x00010005   ← 碰撞！

## 为什么是 16

这是个**隐含假设**，代码里没写：

- **fd 侧**：Linux 文件描述符是小整数（通常 < 100），16 位绰绰有余
- **page_no 侧**：16 位只能到 65535，乘 PAGE_SIZE 4096 → **单文件最大 256 MB**

## 三个缺陷

### ① 位移在 int 里做，有符号溢出是 UB

x.fd 是 int，x.fd << 16 在 32 位里算。一旦 fd >= 32768，结果 ≥ 2^31 → **有符号整型溢出，未定义行为**。
返回类型是 size_t（64 位），作者显然想要 64 位结果，却没有先把操作数拓宽。

现实里 fd 到不了 32768 所以从不触发——但代码既没有掩码也没有注释声明这个前提。

### ② 同族的 PageId::Get() 把错误写得更明显

    inline int64_t Get() const {
        return (static_cast<int64_t>(fd << 16) | page_no);
    }

**cast 加在了位移之后**。位移仍在 32 位里完成，拓宽救不了它。正确写法是把操作数先拓宽：

    return (static_cast<int64_t>(fd) << 16) | page_no;   // 作者的本意

对比 LockDataId::Get()（txn_defs.h:93-102）—— 那里**每一处都是先把字段拓宽再移位**：

    return ((static_cast<int64_t>(type_)) << 63) | ((static_cast<int64_t>(fd_)) << 31) |
           ((static_cast<int64_t>(rid_.page_no)) << 16) | rid_.slot_no;

**同一个打包惯用法，一处对一处错。** 这是个很好的 code review 素材。

### ③ operator&lt; 违反反对称性（真正的逻辑错误）

    bool operator<(const PageId& x) const {
        if(fd < x.fd) return true;
        return page_no < x.page_no;      // 忘了先判断 fd 不相等
    }

取 A={fd:1, page_no:5}、B={fd:2, page_no:3}：

- A &lt; B：1 &lt; 2 → **true**
- B &lt; A：2 &lt; 1 为假 → 落到 page_no &lt; x.page_no → 3 &lt; 5 → **true**

**A&lt;B 和 B&lt;A 同时成立**，违反反对称性，会让任何 std::map&lt;PageId,...&gt; / std::set&lt;PageId&gt; 的树结构损坏。

修法：

    if (fd != x.fd) return fd < x.fd;
    return page_no < x.page_no;

我 grep 过：全项目的 map/set 键是 ColType、int、txn_id_t、std::string、Slot、SvType、CompOp，
**没有一个是 PageId**，所以这个 bug 目前踩不到。但它是三处里唯一"一旦被用上必然出错"的，
而且 operator&lt; 这种基础运算符很容易被后来的人默认信任。

## 一个必须建立的正确认知：哈希碰撞 ≠ 错误

unordered_map 用 operator==（比较两个字段）做最终判定，**碰撞只是让两个不同的页落进同一个桶、
多一次键比较，结果依然正确**。

真正的代价是**性能**——page_table_ 在每一次页访问上都会被查，是系统最热路径之一。

更值得警惕的是：Get() 返回 int64_t、名字叫 Get，**看起来像个唯一标识**；
哪天有人拿它当唯一键（而不是哈希值）用，碰撞就从"变慢"变成"数据错乱"。

## 正确的写法

    size_t operator()(const PageId &x) const {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x.fd)) << 32)
             | static_cast<uint32_t>(x.page_no);
    }

用满 32 位、永不重叠，仍然单射。或者交给标准库混合：

    size_t h = std::hash<int>()(x.fd);
    h ^= std::hash<page_id_t>()(x.page_no) + 0x9e3779b9 + (h << 6) + (h >> 2);

## 顺带：旁边两个死代码

- **std::hash&lt;PageId&gt;**（page.h:42-45）：全项目无人使用，它走 obj.Get()，
  而 Get 除了这里也没别的调用者 —— **同一个键类型存在两套哈希实现**，是维护隐患。
- **operator&lt;**：如上，是 bug，但暂时无害。

---

# 专题五：Context——参数对象与两阶段初始化

## 唯一构造点

Context 在全项目**只有一个构造点**（rmdb.cpp:125）：

    memset(data_send, 0, BUFFER_LENGTH);
    offset = 0;

    Context statement_context(lock_manager.get(), log_manager.get(), nullptr, data_send, &offset);
    Context *context = &statement_context;
    SetTransaction(&txn_id, context);      // ← 事务在这里才被填进去

其他地方清一色 Context *context —— **全项目没有一处按值传递**。

## 五个字段

    class Context {
    public:
        Context(LockManager *lock_mgr, LogManager *log_mgr,
                Transaction *txn, char *data_send = nullptr, int *offset = &const_offset)
            : lock_mgr_(lock_mgr), log_mgr_(log_mgr), txn_(txn),
              data_send_(data_send), offset_(offset) { ellipsis_ = false; }

        LockManager *lock_mgr_;
        LogManager  *log_mgr_;
        Transaction *txn_;
        char        *data_send_;
        int         *offset_;
        bool         ellipsis_;
    };

| 字段 | 谁消费 | 用途 |
| --- | --- | --- |
| lock_mgr_ | SeqScan / IndexScan / Insert / Update / Delete / SmManager | 表级加锁 |
| log_mgr_ | TransactionManager begin/commit/abort | 写 WAL |
| txn_ | 执行器 append_write_record、LockManager::lock | 当前事务 |
| data_send_ | RecordPrinter、QlManager、run_cmd_utility | 响应缓冲区 |
| offset_ | 同上 | 缓冲区游标 |
| ellipsis_ | RecordPrinter | 输出溢出标志 |

**注意 offset_ 是 int* 而不是 int。** 因为 Context 按指针传递，如果 offset_ 是值，
每个模块改的都是自己的副本，client_handler 最后的 send(fd, data_send, offset + 1) 拿到的还是 0。
传指针让**所有追加者写回调用者的那个变量**。所以 data_send_ + offset_ 是一对手搓的"字符串累加器"。

## 两阶段初始化：为什么构造时传 nullptr

    void SetTransaction(txn_id_t *txn_id, Context *context) {
        context->txn_ = txn_manager->get_transaction(*txn_id);
        if (context->txn_ == nullptr || 状态已终结) {
            context->txn_ = txn_manager->begin(nullptr, context->log_mgr_);   // ← 需要 log_mgr_
            ...
        }
    }

**这是个鸡生蛋问题**：开事务需要 log_mgr 去写 Begin 日志，而 log_mgr 装在 Context 里。

解法就是两阶段 —— **先构造出带锁管理器和日志管理器的"壳"，再用这个壳去开事务，最后把事务塞回壳里**。
这也是为什么 txn_ 不能是 const。

同时这里决定了事务语义：新建的事务被 set_txn_mode(false)，即**单条语句的隐式事务**；
只有用户显式敲 begin 才置 true。

## 生命周期：语句级 vs 连接级

Context 是**栈对象、每轮循环重建**，但它持有的两个指针指向**循环外**的变量：

    client_handler(fd)                              连接级 —— 循环外
     ├─ response = make_unique<char[]>(BUFFER_LENGTH)   堆，活到连接结束
     ├─ data_send = response.get()        ────┐
     ├─ offset = 0                        ────┤  Context 指向这两个
     └─ while (receive_request(...)) {        语句级 —— 每轮重建
            Context statement_context(..., data_send, &offset);   ← 栈对象
        }

这个**外内顺序是有意安排的**：如果 data_send / offset 声明在循环内，Context 就会持有悬垂指针。
反过来 Context 每轮重建则保证了"语句级"语义 —— 它只是把连接级的资源**借过来用一条语句的时间**。

## ellipsis_ 的真实用途

它是**响应缓冲区的溢出标志**。缓冲区只有 BUFFER_LENGTH = 8192 字节：

    #define RECORD_COUNT_LENGTH 40
    if (context->ellipsis_ == false &&
        *context->offset_ + RECORD_COUNT_LENGTH + ss.str().length() < BUFFER_LENGTH) {
        memcpy(...); *context->offset_ += ...;
    } else {
        context->ellipsis_ = true;      // 装不下 → 置标志
    }

语义：装不下时**不是截断这一行，而是整个放弃** —— 标志一旦置位，后续所有追加全跳过。
收尾时 print_record_count 看到标志就补一行 "... ..."。

**这里有个精巧的约定**：收尾那次 memcpy **不做边界检查**，靠的是前面每次检查都预留了
RECORD_COUNT_LENGTH = 40 字节，正好够打印省略号和总行数那两行。
**用"提前预留"换掉最后一道判断。**

## 隐患：头文件里的 static

    static int const_offset = -1;                                    // context.h:20
    Context(..., char *data_send = nullptr, int *offset = &const_offset)

命名空间作用域的 static 是**内部链接**，所以**每个 .cpp 各有一份 const_offset**。
而默认实参 &const_offset 在**调用点**求值 —— a.cpp 用默认值构造拿到 a.cpp 那份的地址，
b.cpp 拿到 b.cpp 那份。两处通过 offset_ 累加的输出长度**互不可见**。

**目前踩不到**：唯一构造点显式传了 &offset，默认实参从未被使用。
但这属于"编译通过、运行不报错、结果静默错乱"那一类陷阱。

而且初值 -1 本身危险 —— 真用默认实参，向缓冲区起点前一个字节写入就是越界。
（好在 data_send 默认 nullptr，会先段错误，反而算快速失败。）

正确写法是 C++17 的 inline int const_offset = -1;。

**更彻底的看法**：既然唯一调用点显式传全了参数，这两个默认值**应该删掉** ——
让编译器帮忙挡住"忘记传缓冲区"的调用，而不是给一个必然崩溃的默认值。

---

# 第三部分 工程方法论（面试官最看重的部分）

## 3.1 内存泄漏排查：从"看出来"到"证明它"

### 先讲结论

client_handler 里旧版的 Context *context = new Context(...) 在循环体内，**没有任何配对 delete** ——
这是个用眼睛就能定位的泄漏。但**"看出来"和"证明它、并确认修干净了"是两回事**。

### 第一步：确认泄漏的"形状"

| 问题 | 怎么测 | 答案 |
| --- | --- | --- |
| 泄漏一次还是每次？ | 发 1 条 vs 1 万条，比对 | **每次** —— 与请求数成正比 |
| 单次多大？ | 按对象字段估算 | Context ≈ 5 指针 + 1 bool ≈ **48 B** + malloc 头部 |
| 谁分配的？ | 分配调用栈 | rmdb.cpp 循环体 |

**"与请求数成正比"是最强的信号** —— 它把范围从"某个全局对象没析构"直接缩小到"循环体内 new 了没 delete"。

48 字节这个量级带来一个重要推论：

> 发 1 万条 SQL 才漏 0.5 MB，**完全淹没在 rmdb 正常的内存波动里**（光缓冲池就 256 MB）。
> **别指望 top / RSS** —— 要么把负载放大到百万级，要么直接上工具。

### 第二步：选工具

**ASan + LSan（首选）**。项目 CMake **没有** sanitizer 选项，得手动加：

    cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
    cmake --build build-asan -j4

然后**必须跑真实的服务端 + 真客户端**：

    ASAN_OPTIONS=detect_leaks=1:log_path=/tmp/asan-rmdb \
      python3 tests/test_sql_regressions.py build-asan/bin/rmdb

LSan 在进程退出时给出分配栈：

    Direct leak of 48 byte(s) in 1 object(s) allocated from:
        #0 operator new(unsigned long)
        #1 client_handler(int) src/rmdb.cpp:118

**三个坑，不避开就是白跑：**

**坑 1：单元测试抓不到这个泄漏。** client_handler 只有真实 TCP 连接才会被调用。
ctest 里的 unit_test、storage_regression 跑一万遍也不会触发它。
**必须走 tests/ 下那几个真起服务端的脚本。**

**坑 2：LSan 只在进程正常退出时报告。** 好在这个项目是干净的：
should_exit 是 volatile sig_atomic_t（rmdb.cpp:33），信号处理器**只置标志**（:55-58），
SIGINT 和 SIGTERM 都注册了（:319-320），accept 循环退出后正常返回 main。

而 tests/sql_harness.py:77 恰好用的就是 SIGINT：

    self.process.send_signal(signal.SIGKILL if crash else signal.SIGINT)

**所以现成脚本天然满足 LSan 触发条件** —— 正常路径有报告，crash=True 那条路径用 SIGKILL 就没有。

**坑 3：报告会被临时目录一起删掉。** sql_harness.py 把服务端 stdout/stderr 重定向到
self.root/server.log，而 self.root 是 tempfile.TemporaryDirectory，__exit__ 时连报告一起清理。
所以要用 log_path= 写到固定位置。

（sql_harness.py:24 的 Popen 没传 env=，ASAN_OPTIONS 通过外部环境变量传入、由子进程继承即可。）

**备选一：Valgrind**（不重编译，但慢 10~50 倍）

    valgrind --leak-check=full --show-leak-kinds=definite,indirect \
             --num-callers=30 --log-file=/tmp/vg.log ./build/bin/rmdb testdb

**重点只看 definitely lost**。still reachable 通常**不是** bug —— 比如 rmdb.cpp 里那批全局单例
（disk_manager / buffer_pool_manager / sm_manager …），它们本来就活到进程结束。

**备选二：计数法**（不装任何工具）

    static std::atomic<long> g_ctx_live{0};
    Context(...) { ++g_ctx_live; }
    ~Context()   { --g_ctx_live; }

每处理一条 SQL 后打印。**差值随请求数线性增长 → 确认泄漏；恒定 → 不是每次泄漏。**
这招的价值在于**直接验证"与请求数成正比"这个关键假设**。

### 第三步：二分法缩小范围

    std::shared_ptr<Query> query = analyze->do_analyze(ast::parse_tree);
    // std::shared_ptr<Plan> plan = optimizer->plan_query(query, context);
    // std::shared_ptr<PortalStmt> portalStmt = portal->start(plan, context);
    // portal->run(portalStmt, ql_manager.get(), &txn_id, context);

逐段打开，看计数从哪一步开始涨。

### 第四步：修法 —— 别补 delete，换所有权模型

**最直觉的改法是循环末尾补 delete context;，但这是错的：**

    try {
        portal->run(...);      // 这里 throw，控制流直接跳到 catch
    } catch (std::exception &e) { ... }
    delete context;            // 永远执行不到

rmdb.cpp 的 try/catch 正好包住了执行阶段，**任何 SQL 报错都会让 delete 被跳过**。
这个补法会把"每次都漏"变成"只在报错时漏"——**更难查**，因为测试不报错时看起来完全正常。

**正确的改法是栈对象**，也正是 main 现在的写法：

    Context statement_context(lock_manager.get(), log_manager.get(), nullptr, data_send, &offset);
    Context *context = &statement_context;

零分配、零泄漏、异常安全。而且 Context 就是个 5 指针 + 1 bool 的聚合，**本来就没理由上堆**。

### 一个更隐蔽的点：代价不只是 48 字节

Context 持有 Transaction *txn_，而事务在语句结束时会被**真的 delete 掉**（rmdb.cpp:186-191）。
如果 Context 本身泄漏在堆上，它就成了一个**永不析构、且 txn_ 已悬垂的对象**。
现在恰好没人再碰它所以没事 —— 但这是"今天不漏数据、明天改一处就 use-after-free"的埋雷。

> **泄漏真正的代价不是 48 B × N，而是一个本该有明确生命周期的对象，
> 变成了生命周期无限长、内容已失效的幽灵。**

## 3.2 堆、栈、静态区：句柄与本体

### 不是两个区，是三个

| 区 | 放什么 | 谁释放 |
| --- | --- | --- |
| **栈** | 函数帧：返回地址、保存的寄存器、参数、局部变量、临时对象 | 编译器（含异常展开） |
| **堆** | new / malloc 出来的块、容器元素、智能指针对象与控制块 | **程序员**（或智能指针） |
| **静态区** .data/.bss | 全局变量、static 变量、类静态成员、字面量 | 运行时 |

### 语法判断：看"怎么被创建的"

| 写法 | 本体在哪 |
| --- | --- |
| Type x; / Type x(args); | 栈 |
| new Type(args) | 堆（你负责 delete） |
| std::make_unique&lt;Type&gt;(...) | 堆（unique_ptr 负责） |
| std::make_shared&lt;Type&gt;(...) | 堆（shared_ptr 负责） |
| 全局 Type x; / static Type x; | 静态区 |
| 类的成员变量 | **跟着宿主走** |

### 最关键：句柄和本体是两个东西

**同一个声明可能同时涉及两三个区。**

    // rmdb.cpp:36 —— 一句话里两个区，且没有栈
    auto disk_manager = std::make_unique<DiskManager>();
    //  disk_manager 这个 unique_ptr 对象本身 → 静态区（它是全局变量）
    //  它指向的 DiskManager 对象        → 堆

    // rmdb.cpp:96-97
    auto response = std::make_unique<char[]>(BUFFER_LENGTH);
    char *data_send = response.get();
    //  response（8 字节 unique_ptr）→ 栈
    //  它管的 8192 字节数组          → 堆
    //  data_send                    → 栈上的指针

    // buffer_pool_manager.h:40 —— 都在堆，但是两次独立分配
    pages_ = new Page[pool_size_];
    //  pages_ 这个指针（BPM 的成员，BPM 在堆上）→ 堆
    //  它指向的 65536 个 Page                    → 堆（另一次 256 MB）

> **判断口诀：变量名问"它在哪"，指针问"它指向的东西在哪"——两个答案经常不同。**
> 而"在堆上"不等于"同一次分配"，这对泄漏分析很重要。

### 项目实测分布

    177  make_shared<
     52  make_unique<
     13  new char[
      3  new WriteRecord
      2  new IxNodeHandle
      1  new Transaction
      1  new Page[

**裸 new 一共只有 20 处**，其余 229 处都走了智能指针。
这是个很有用的数字 —— **泄漏的审计面其实很小，可以直接人工过一遍**。

### 运行时怎么验证

**方法 1：看地址范围 —— 不可靠。** 实测：

    g_data     0x55e24538d010   .data 静态存储区
    g_rodata   0x55e24538b008   .rodata 字面量
    s_local    0x55e24538d014   函数内 static
    local      0x7fff3996f64c   栈上的变量
    &heap      0x7fff3996f650   栈上的指针变量本身
    heap       0x55e2676a42b0   堆上的对象

**关键观察**：堆 0x55e2676a42b0 和全局区 0x55e24538d010 都在 0x55e2... 区间 ——
因为 PIE 下小分配走 brk，堆紧跟在二进制数据段之后。

> **光看地址前缀分不出"堆"和"全局"。** 只有栈（0x7fff...）能一眼认出。加上 ASLR，靠地址判断更不可靠。

**方法 2：看内存映射表 —— 权威。**

    pid=$(pgrep rmdb)
    cat /proc/$pid/maps | grep -E '\[stack\]|\[heap\]'

gdb 里对应 info proc mappings。

**方法 3：让 ASan 直接告诉你 —— 最实用。** 实测两份报告：

堆悬垂：

    ==447==ERROR: AddressSanitizer: heap-use-after-free on address 0x502000000030
    READ of size 4 at 0x502000000030 thread T0
        #0 ... in main .tmp-regions.cpp:31
    freed by thread T0 here:
        #1 ... in heap_dangling() .tmp-regions.cpp:13
    previously allocated by thread T0 here:
        #1 ... in heap_dangling() .tmp-regions.cpp:12

栈悬垂：

    ==580==ERROR: AddressSanitizer: stack-use-after-scope on address 0x7f5c95f00020
    Address 0x7f5c95f00020 is located in stack of thread T0 at offset 32 in frame
        #0 ... in main .tmp-scope.cpp:2
      This frame has 1 object(s):
        [32, 36) 'x' (line 5) <== Memory access at offset 32 is inside this variable

**规律很干净 —— ASan 的错误类型名就是答案：**

| 报错前缀 | 区域 |
| --- | --- |
| heap-use-after-free / heap-buffer-overflow / double-free | **堆** |
| stack-use-after-scope / stack-use-after-return / stack-buffer-overflow | **栈** |
| global-buffer-overflow | **全局 / 静态区** |

栈那份还会额外告诉你**哪个变量、在哪个作用域、占几个字节**。

**一个反直觉的坑**：上面栈报告的地址是 0x7f5c...，不是真实的 0x7fff... 栈地址 ——
ASan 为了检测 use-after-return 会把栈帧搬到 "fake stack"。
**所以在 ASan 下地址前缀更不能用来判断区域**，但无所谓，错误类型名已经说清楚了。

**另一个实测发现**：我第一次用 return &local; 那种写法时，ASan **只报了 SEGV 没报 stack-use-after-scope**
（函数帧整个没了，指针读出来是 0）。换成"变量离开作用域、指针还留着"的写法才拿到标准报告。

> **教训：检测能力取决于具体的悬垂形态，不能因为某次没报就认为没问题。**

### 为什么这个区分对排查是决定性的

1. **泄漏只可能发生在堆。** 所以"找泄漏" ≡ "找裸 new/malloc 且没有配对释放、也没有 RAII 包装的地方"。
   本项目裸 new 只有 20 处 —— 人工过一遍完全可行，而不是翻一万行代码。

2. **栈对象不会泄漏，但会悬垂，性质完全不同**（见专题三的三种悬垂表）。

3. **栈有大小上限（Linux 默认 8 MB），堆几乎没有。** 这直接解释了为什么
   pages_ = new Page[65536]（256 MB）**必须**在堆上 —— 放栈上直接爆栈。

4. **栈对象自带异常安全。** 那个 delete context 被异常跳过的 bug，
   根因就是**堆对象的释放是程序员的责任**。栈对象在栈展开时自动析构，压根没有这个失败模式。

## 3.3 版本陷阱：我如何推翻自己的三个结论

这条适合回答"**你怎么保证自己的判断是对的**"。

### 事情经过

调研早期我基于一份**较早的项目副本**读代码，得出三个结论：

1. 磁盘 I/O 用 lseek + read，存在并发共享偏移问题
2. 索引层缺少空指针检查，会段错误
3. disk_manager 没有重试逻辑

改用哈希逐文件比对当前 main 分支后发现：**两份副本 116 个源文件里有 38 个真实不同**
（先做 CRLF 归一化，否则会误报 111 个）。逐条复核的结果：

| 我的结论 | 复核结果 |
| --- | --- |
| lseek + read 共享偏移 | **作废** —— main 已改成带 EINTR 与短读写重试的 pread/pwrite |
| 索引层会段错误 | **不成立** —— fetch_node/create_node **从未被调用**，索引实际是内存有序 vector |
| 缺判空（作为防御性缺陷） | **成立** —— 消费者 release_node_handle 判了空，生产者不判，内部不一致 |

### 我的处理方法

1. **把所有结论逐条回到真源重新验证**，而不是"看起来对就写进文档"
2. **按行号加内容核对**，而不是靠记忆
3. **对自己已经说出口的结论主动更正**

### 教训

> **代码审查的第一原则是确认你读的是哪个版本。**
> 在有多个副本、多个分支的项目里，"我读过这个文件"和"我读的是这个分支的这个文件的这一版"是两件事。

### 怎么讲这个故事

强调两点：**一是发现机制**（用哈希比对而不是肉眼 diff，因为肉眼会漏掉 CRLF 之外的细微差异）；
**二是处理姿态**（把"我错了"讲成"我用可复现的方法把错误找出来了"）。

---

# 第四部分 问答速查

| 追问 | 一句话答案 | 展开位置 |
| --- | --- | --- |
| 帧和页有什么区别？ | 页是磁盘逻辑单位，帧是内存槽位，page_table_ 是两者映射 | 专题一 |
| 为什么 Page 用内嵌数组？ | 一次分配、cache 友好、new Page[N] 成立；代价是不可拷贝 | 专题一 |
| pin_count 干什么？ | 引用计数 + 淘汰许可；大于 0 表示有人正拿着这个 Page* | 专题二 |
| LRU 按访问时刻排序吗？ | 不是，按**最后一次 unpin 的时刻**；链表里的帧必定 pin_count==0 | 专题二 |
| LRU 有什么问题？ | 顺序扫描污染工作集；且全局链表难以分片，锁争用大 | 专题二 |
| 真实系统为什么用 CLOCK？ | 省链表开销且可分片（一半是命中率，一半是并发） | 专题二 |
| 缓冲池并发度如何？ | 全局锁覆盖磁盘 I/O，并发度实际为 1 | 专题二 |
| pin_count 能直接改成 atomic 吗？ | 不能。"判空闲 + 占为己有"必须原子，只原子化计数解决不了 TOCTOU | 专题二 |
| pin 泄漏的后果？ | 帧永久不可淘汰，池子耗尽后 fetch_page 全返回 nullptr，系统退化且不报错 | 专题二 |
| Page::data_ 和 RmRecord::data 区别？ | 前者是帧内嵌的 4KB 仓库，后者是执行层堆上的记录快照 | 专题三 |
| 为什么 get_record 要深拷贝？ | 用一次 memcpy 换掉一个跨模块的 pin 生命周期契约 | 专题三 |
| 缓冲池知道页里装什么吗？ | 不知道，只搬 4096 字节；布局由 RmPageHandle / IxNodeHandle 各自解释 | 专题三 |
| PageIdHash 怎么做的？ | 位段打包，fd 高 16 位、page_no 低 16 位，在范围内是零碰撞单射 | 专题四 |
| 这个哈希有什么问题？ | int 位移 UB、Get() 的 cast 位置错、operator&lt; 违反反对称性 | 专题四 |
| 哈希碰撞会导致错误吗？ | 不会，unordered_map 用 operator== 兜底；代价是热路径退化 | 专题四 |
| Context 为什么两阶段构造？ | 开事务需要 log_mgr，而 log_mgr 在 Context 里 —— 先造壳，再填事务 | 专题五 |
| Context 是语句级还是连接级？ | 语句级（栈对象每轮重建），但借用了连接级的缓冲区和游标 | 专题五 |
| offset_ 为什么是指针？ | 让所有追加者写回调用者的同一个变量，否则 send 时长度是 0 | 专题五 |
| ellipsis_ 干什么？ | 8KB 响应缓冲区溢出标志；溢出后整个放弃追加并打印省略号 | 专题五 |
| 怎么排查内存泄漏？ | 先确认形状（是否与请求数成正比）→ ASan/LSan → 二分 → 换所有权模型 | 3.1 |
| 为什么不能直接补 delete？ | try/catch 包住执行阶段，任何 SQL 报错都会跳过 delete，反而更难查 | 3.1 |
| 怎么区分堆和栈对象？ | 看怎么创建的；且要分开看句柄和本体；运行时看 maps 或 ASan 错误类型名 | 3.2 |

---

# 第五部分 边界与路线

## 必须主动说的能力边界

| 能力 | 真实状态 |
| --- | --- |
| **索引结构** | 内存中的有序 std::vector（g_index_entries），**不是磁盘 B+ 树**；IxNodeHandle 是未调用的框架骨架 |
| **逻辑优化** | Planner::logical_optimization **是空实现**，只做物理优化 |
| **索引选择** | 要求前缀列全部等值才走索引；无 CBO、无统计信息 |
| **连接算法** | 只有块嵌套循环，无 hash join / sort-merge join |
| **并发控制** | **表级 2PL + no-wait**；行级锁与意向锁 API 已实现但**零调用点** |
| **隔离级别** | 声明 SERIALIZABLE，靠表级 2PL；无 MVCC |
| **恢复** | **完整 WAL 重放，不是 ARIES**；无检查点、页 LSN、CLR、校验和 |
| **聚合** | 支持 SUM/MIN/MAX/COUNT 与 AS 别名，**无 GROUP BY** |
| **DDL 与事务** | DDL 只支持自动提交，不能在显式事务里回滚，无 savepoint |
| **磁盘空间** | deallocate_page 是空实现，不回收页号 |
| **缓冲池并发** | 全局锁覆盖 I/O，并发度实际为 1 |

## 如果再做两周（按性价比排序）

1. **细化缓冲池的锁** —— 分桶锁 + 把 I/O 移出临界区。
   投入小收益直接（并发度从 1 提到接近核数），而且 **pread/pwrite 的前提已经就位**，适合第一个动手。
2. **把替换策略真正做成可插拔** —— 实现 CLOCKReplacer 做 A/B 对比。
   这正好能回答"LRU 有什么问题"，并且用数据说话。
3. **补上 RAII 的 PageGuard** —— 把 pin 泄漏从"靠纪律"变成"编译期保证"，并发改造的前置条件。
4. **修掉本文发现的三个缺陷** —— PageIdHash 系列、索引层判空、Context 默认实参。
   工作量小、可验证、可写成完整的"发现 → 定位 → 修复 → 验证"闭环。
5. **索引换成真的 B+ 树** —— 最大的一块，也是能撑起一整场面试的深度话题。
6. **给恢复加检查点** —— 把恢复时间从"正比于日志长度"降到"正比于活跃事务"。

## 收尾话术

> "这个项目我最想讲清楚的是**一条 SQL 从进来到出去，每一层做了什么决定、为什么这么决定**。
> 我在做的过程中修了几个静默失效的问题，也明确知道它还有哪些没做 ——
> 比如索引还是内存 vector、并发还是表级锁。**这些边界我很清楚，也知道下一步该动哪里。**"

---

## 相关文档

| 文档 | 作用 |
| --- | --- |
| [面试问答](interview-qa.md) | 主线讲解：SQL 执行链路、事务管理器、缓冲池与 LRU、能力边界 |
| [SQL 执行流程图](sql-execution-flow.md) | 十张 Mermaid 调用链图，可现场指认 |
| [题目对照](requirements-audit.md) | 11 项课程要求与实现的逐条对应、缺陷编号 D01–D12 |
| [修复与验证记录](maintenance-20260919.md) | 本轮修复清单与测试证据 |
| **本文** | 深挖专题：帧与页、pin 并发、两个 data、PageIdHash、Context、工程方法论 |
