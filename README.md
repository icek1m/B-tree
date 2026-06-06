### B+ tree

一、需求分析

- 基础键值操作：支持 写入Put(key, value)、读取Get(key) → value、删除Delete(key)。

- 范围扫描：支持 Scan(start_key, end_key) → 迭代器，能在给定范围内高效顺序遍历。

- 多数据类型：键与值均视为不透明的字节序列，由上层传入自定义比较器，引擎不预设其类型。

- 并发读写：允许多个读操作与一个写操作同时进行，接口是线程安全的。


二、核心数据结构

## 1. 基础类型与常量

```c
#define PAGE_SIZE 4096       // 页大小
#define INVALID_PAGE_ID 0    // 无效页号

typedef uint32_t page_id_t;  // 页号类型（4 字节）

typedef enum {
    PAGE_UNDEFINED = 0,
    PAGE_INTERNAL = 1,       // 内部节点
    PAGE_LEAF = 2,           // 叶子节点
} page_type_t;

typedef enum {
    BTREE_OK = 0,
    BTREE_NOT_FOUND,
    BTREE_PAGE_FULL,
    BTREE_IO_ERROR,
    BTREE_OUT_OF_MEMORY,
    BTREE_CORRUPTED,
    BTREE_DUPLICATE_KEY,
} btree_error_t;

// 键比较器函数类型（由上层传入，支持自定义类型）
typedef int (*btree_compare_t)(const uint8_t *a, uint16_t a_len,
                               const uint8_t *b, uint16_t b_len);
```

**设计功能**：
- `PAGE_SIZE`（4096 字节）——固定页大小，与文件系统块对齐，减少磁盘 I/O 碎片。
- `INVALID_PAGE_ID`（0）——哨兵值，用于链表末尾和未初始化的根节点。
- `page_id_t`（uint32_t）——4 字节页号，支持 16 TiB 地址空间，兼顾文件大小上限与存储开销。
- `page_type_t`——三态枚举区分内部节点与叶子节点，是树结构路由的基础。
- `btree_error_t`——统一错误码，所有 API 返回相同类型，调用者无需分别处理不同模块的错误。
- `btree_compare_t`——函数指针类型，使 B+ 树不依赖键的具体类型，上层可注入任意比较逻辑。

## 2. 通用页面布局（page_t）

每个页面大小为 4096 字节，由定长 32 字节页头与变长数据区组成。

```c
// 页头（32 字节，磁盘布局）
#define PAGE_HEADER_SIZE 32

typedef struct {
    uint32_t page_id;       // 0:  页号
    uint8_t  type;          // 4:  节点类型（LEAF / INTERNAL）
    uint16_t num_slots;     // 5:  槽位目录条目数
    uint16_t free_offset;   // 7:  空闲区起始偏移
    uint16_t free_size;     // 9:  空闲区字节数
    uint32_t parent_id;     // 11: 父节点页号
    uint32_t prev_id;       // 15: 前驱兄弟页号（叶子节点双向链表）
    uint32_t next_id;       // 19: 后继兄弟页号
    uint32_t first_child_id;// 23: 内部节点最左子节点页号
    uint8_t  reserved[5];   // 27: 保留
} page_header_t;

// 原始页面缓冲区
typedef union {
    uint8_t      bytes[PAGE_SIZE];
    page_header_t header;
} page_t;
```

**设计功能**：
- `page_header_t`（32 字节定长）——嵌入在每个页面头部，集中管理页的元信息：`page_id` 标识页号；`type` 区分内部/叶子节点；`num_slots` / `free_offset` / `free_size` 构成槽位目录协议，共同追踪空闲空间；`prev_id` / `next_id` 实现叶子节点的双向链表，支持范围扫描正向/反向遍历；`first_child_id` 独立存储内部节点的最左子节点；`parent_id` 在分裂/合并时向上回溯。
- `page_t`（union 类型）——同一片 4096 字节内存既可解释为页头字段，也可作为原始字节缓冲区读写，避免显式类型转换。

## 3. 槽位目录（slot directory）

页内数据通过槽位目录管理，目录项从页尾向前生长，空闲区从页头后向后生长。

```c
// 槽位目录项（4 字节）
typedef struct {
    uint16_t offset;  // 记录在页内的起始偏移
    uint16_t length;  // 记录长度
} slot_t;
```

**设计功能**：
- `slot_t`（4 字节）——槽位目录的核心，每个条目记录一条数据的偏移和长度。目录从页尾向前生长，数据从页头后向后生长，两者相遇即页满。这种双向生长设计使插入和删除只需移动槽目录（4 字节/项）而非整个数据区，降低数据搬移开销。

## 4. 叶子节点结构

叶子节点存储实际的键值对，槽位目录中的记录按键排序。

```c
/*
 * 叶子记录二进制布局（槽中存储格式）：
 *   key_len(2B) + key_data(变长) + value_len(2B) + value_data(变长) + is_deleted(1B)
 *   固定开销 = 5 字节
 */
```

| 字段         | 大小 | 说明                     |
|-------------|------|--------------------------|
| key_len     | 2B   | 键长度                   |
| key_data    | 变长 | 用户键（不透明字节序列）   |
| value_len   | 2B   | 值长度                   |
| value_data  | 变长 | 用户值                   |
| is_deleted  | 1B   | 逻辑删除标记（0/1）       |

**叶子节点相关处理函数**：

```c
// 计算叶子记录占用空间
uint16_t leaf_rec_size(uint16_t key_len, uint16_t val_len);

// 将叶子记录打包到缓冲区
void leaf_rec_pack(uint8_t *dest,
                   const uint8_t *key, uint16_t key_len,
                   const uint8_t *val, uint16_t val_len,
                   bool deleted);

// 访问器（内联）
uint16_t leaf_key_len(const uint8_t *rec);   // 提取键长度
uint8_t *leaf_key_ptr(uint8_t *rec);         // 提取键指针
uint16_t leaf_val_len(const uint8_t *rec);   // 提取值长度
uint8_t *leaf_val_ptr(uint8_t *rec);         // 提取值指针
bool     leaf_is_deleted(const uint8_t *rec);// 是否为逻辑删除
```

**设计功能**：
- `leaf_rec_size` / `leaf_rec_pack` ——序列化/反序列化函数，将键值对按定长格式写入页缓冲区。固定开销 5 字节（2+2+1），支持变长键值。
- `leaf_key_len` / `leaf_key_ptr` / `leaf_val_len` / `leaf_val_ptr` ——从已打包的记录中提取各字段，避免重复解析。设计为内联函数，消除函数调用开销。
- `leaf_is_deleted` ——读取删除标记位，支持逻辑删除（标记后记录仍在页内，仅查询时跳过）。

## 5. 内部节点结构

内部节点的记录只存储分隔键与子节点页号，最左子节点通过 `first_child_id` 单独存储。

```c
/*
 * 内部节点记录二进制布局：
 *   key_len(2B) + key_data(变长) + child_id(4B)
 *   每个槽记录一个"分隔键"及其"右孩子页号"
 *   最左孩子通过 page->first_child_id 单独存储
 *   固定开销 = 6 字节
 */

// 计算内部记录占用空间
uint16_t internal_rec_size(uint16_t key_len);

// 打包内部记录
void internal_rec_pack(uint8_t *dest,
                       const uint8_t *key, uint16_t key_len,
                       page_id_t child_id);

// 访问器（内联）
uint16_t  internal_key_len(const uint8_t *rec);  // 提取键长度
uint8_t  *internal_key_ptr(uint8_t *rec);         // 提取键指针
page_id_t internal_child_id(const uint8_t *rec); // 提取子节点页号
```

**设计功能**：
- `internal_rec_size` / `internal_rec_pack` ——内部节点记录的序列化。固定开销 6 字节（2+4），比叶子记录少 1 字节（无删除标记位），因为内部节点只做路由不做数据存储。
- `internal_key_len` / `internal_key_ptr` / `internal_child_id` ——提取分隔键和对应的右孩子页号。内部节点中第 i 个槽的分隔键对应第 i+1 个子树（最左子树由 `first_child_id` 单独存储）。

## 6. B+ 树句柄（btree_t）

运行时上下文，封装存储层回调与并发控制。

```c
struct btree {
    btree_compare_t   cmp;         // 键比较器
    btree_read_page_t  read_page;  // 读页回调
    btree_write_page_t write_page; // 写页回调
    btree_alloc_page_t alloc_page; // 分配新页回调
    void              *io_ctx;     // 存储层上下文指针
    page_id_t root_id;             // 当前根节点页号
    pthread_rwlock_t rwlock;       // 读写锁（支持多读一写）
};

// I/O 回调类型
typedef btree_error_t (*btree_read_page_t)(void *ctx, page_id_t pid, page_t *page);
typedef btree_error_t (*btree_write_page_t)(void *ctx, page_id_t pid, const page_t *page);
typedef btree_error_t (*btree_alloc_page_t)(void *ctx, page_id_t *pid, page_type_t type);
```

**设计功能**：
- `btree_t` ——引擎的核心句柄，聚合了比较器、I/O 回调、根页号和读写锁。通过回调函数指针（`read_page` / `write_page` / `alloc_page`）将存储层解耦，上层可注入缓冲池、WAL 或纯内存等多种实现，无需修改 B+ 树算法代码。
- 三组 I/O 回调类型（`btree_read_page_t` 等）——将页的读写和分配抽象为接口，使得 B+ 树算法与底层存储完全解耦，便于测试和替换后端。
- `root_id` ——缓存根节点页号，每次操作从根开始二分下降到目标叶子。根节点在分裂时可能变更，通过 `btree_storage_set_root_id` 持久化。
- `rwlock`（`pthread_rwlock_t`）——POSIX 读写锁，允许多个读线程同时访问，写线程独占，满足多读一写的并发需求。

## 7. 记录收集器（分裂辅助）

用于节点分裂时临时存放所有记录的动态数组。

```c
typedef struct {
    uint8_t  **recs;    // 每条记录独立 malloc 拷贝
    uint16_t  *lens;    // 每条记录长度
    int        count;   // 当前记录数
    int        capacity;// 最大容量
} rec_array_t;
```

**设计功能**：
- `rec_array_t` ——节点分裂时的临时容器。分裂前将所有现存记录和新记录收集到数组中（通过 `rec_array_merge_sorted` 保持有序），然后按中点平分到两个新页中。这种"先收集再重分"的策略简化了分裂逻辑，但代价是额外的内存分配和数据拷贝。
- `recs` / `lens` 分离存储 ——记录指针和长度分别用两个数组存放，便于按长度计算空间占用，支持按需分配。

三、核心算法设计
1. 搜索算法（Get / Seek）
目标：给定一个键，在无写入的情况下，不阻塞读取地找到叶子节点及数据位置。

2. 插入算法（Put）

定位与加锁：沿着根到叶子的路径，找到目标叶子节点。

插入记录：若键已存在，标记旧记录为 is_deleted，再插入新记录。

3. 删除与合并算法（Delete）
采用逻辑删除 + 后台物理清理。

定位：同插入，找到叶子节点。

标记删除：将记录 is_deleted 置位。写一条 DELETE 日志。

页面空间回收：当页内活跃记录占用空间 < 页面容量的 50% 时，触发页面空间回收

四、业务流程设计
现在将上述算法嵌入到完整的运行时流程中。

1. 事务写入流程（Put 示例）

2. 后台检查点流程
目的是减少恢复时需要重放的日志量。

3. 崩溃恢复流程
引擎启动时，必须先执行恢复。

4. 撤销：扫描未提交事务表，找到其所有日志，执行反操作（如将 is_deleted 复位），并写入补偿日志。

5. 清理：恢复完成，系统以一致状态打开对外服务。

排行榜 CLI — main.c
  - 菜单界面：查看前 N 名、查询个人财富与排名、添加/更新、删除
  - 财富值千分位格式显示
  - ./btree_engine → 排行榜


五、可改进的设计方向

## 5.1 存储层

| 不足 | 说明 | 改进方向 |
|------|------|----------|
| **缺少 fsync 保障** | `btree_storage_write` 未调用 `fsync`，崩溃时数据页可能未落盘 | 在关键写路径（事务提交、检查点）增加 `fsync`，或依赖 WAL 层统一保证持久性 |
| **页分配器过于朴素** | `alloc` 返回单调递增的 `num_pages`，不回收已释放的页面，文件大小只增不减 | 引入空闲页链表（free list）或位图（page bitmap），支持页面复用 |
| **写文件头过于频繁** | `alloc` 和 `set_root_id` 每次都重写整个文件头（含 `fseek` + `fwrite`） | 将元数据缓存在内存中，仅在检查点或 clean shutdown 时持久化 |
| **无校验和保护** | 数据页和文件头均无 checksum，静默损坏无法检测 | 页尾追加 CRC32，读时校验 |

## 5.2 缓冲池

| 不足 | 说明 | 改进方向 |
|------|------|----------|
| **LRU 淘汰为 O(n) 扫描** | `evict_one` 每次全局线性扫描所有帧找最小 `access_stamp`，容量大时性能差 | 改用 CLOCK 算法（二次机会）或链式 LRU（O(1) 淘汰） |
| **缺少 Pin / Unpin 机制** | 游标遍历时，当前页可能被淘汰，每次访问需重新读盘 | 增加引用计数（pin count），被钉住的页不被淘汰 |
| **缓冲池非线程安全** | `bp_read` / `bp_write` 本身未加锁，并发访问下哈希表可能损坏 | 引入 `pthread_mutex` 或分段锁（hash table stripe locking）保护内部结构 |
| **全表扫描不友好** | 顺序遍历时逐页读取，LRU 反复淘汰刚用过的页 | 增加页面预取（prefetch）或顺序扫描提示（sequential scan hint） |

## 5.3 WAL（预写日志）

| 不足 | 说明 | 改进方向 |
|------|------|----------|
| **只有 REDO 日志，无 UNDO** | 事务回滚（`txn_abort`）是空操作，修改无法撤销 | 增加 UNDO 日志 + 补偿日志（CLR），支持原子回滚 |
| **页级日志粒度太粗** | 每次写操作都记录完整的 4096 字节页，WAL 膨胀快 | 采用逻辑日志（记录操作语义）或页内增量日志（只记录修改的字节范围） |
| **每条写入都 fsync** | 每次 `wal_write` 都执行一次 `fsync`，批量写入场景性能瓶颈 | 组提交（group commit）：攒够一批日志后统一 fsync |
| **缺少 checksum** | WAL 条目无校验和，恢复时无法检测日志损坏 | 每条日志条目追加 CRC32，恢复时校验 |
| **WAL 与数据文件耦合** | WAL 恢复后直接写存储层，无法保证原子切换 | 引入 checkpoint 机制，标记已刷盘的 LSN，恢复时只重放 LSN 之后的日志 |

## 5.4 事务子系统

| 不足 | 说明 | 改进方向 |
|------|------|----------|
| **树级读写锁粒度太粗** | 整个 B+ 树只有一把 `pthread_rwlock`，写事务完全串行，读事务也阻塞写 | 页级锁（page latch）+ 意向锁（intention lock），或行级锁 |
| **无 MVCC** | 读事务看到的是写事务提交后的状态，无法提供时间点快照 | 基于页版本链（page version chain）或回滚段（undo segment）的快照隔离 |
| **回滚为空操作** | `txn_abort` 只是释放锁，不撤销写操作 | 结合 UNDO 日志实现真正的回滚 |
| **无死锁检测** | 多写事务串行化避免了死锁，但如果引入页级锁，需要死锁检测 | 超时检测（timeout）或等待图（waits-for graph）检测 |

## 5.5 B+ 树算法

| 不足 | 说明 | 改进方向 |
|------|------|----------|
| **分裂拷贝全部记录** | 叶子/内部节点分裂时，将所有记录拷贝到 `rec_array_t` 再重写，内存开销大 | 原地分裂（in-place split）：直接在新页插入右半记录，减少复制与动态分配 |
| **合并阈值固定** | 删除合并条件写死为 `(叶子 < 2) 或 (内部 < 1)`，不够自适应 | 基于节点空间利用率动态决定合并时机（如 < 40% 触发） |
| **缺少前缀压缩** | 内部节点的相邻分隔键通常共享前缀，逐字节存储浪费空间 | 引入前缀压缩（prefix compression），内部节点只存储区分部分 |
| **无批量插入优化** | 逐条插入大量有序数据时，每次从根走到叶，路径重复 | 检测有序插入模式，缓存最右叶子路径（rightmost leaf optimization） |
| **删除无物理空间回收** | 删除调用 `page_remove_slot` 但页内空洞仅在下次插入时被覆盖，不缩文件 | 增加页面合并后释放空页到 free list，支持 Shrink 文件 |

## 5.6 页面布局

| 不足 | 说明 | 改进方向 |
|------|------|----------|
| **槽位目录查找 O(n)** | 槽位目录数组是顺序排列但仍需逐项计算偏移读取记录 | 将槽位目录设计为"指针数组 + 二分查找友好"格式，或直接在内节点中存储完整键 |
| **页头 5 字节保留浪费** | 保留字段占 5 字节，在 PAGE_HEADER 中占比较高 | 利用保留字段存储页级别（level）、校验和或版本号等有用信息 |

## 5.7 并发与锁

| 不足 | 说明 | 改进方向 |
|------|------|----------|
| **仅支持多读一写** | 写事务持有写锁期间所有读被阻塞，高写入负载下吞吐低 | 多版本并发控制（MVCC）或读-写无锁化（lock-free B-link tree） |
| **游标无并发保护** | 游标在遍历过程中若树结构发生变化（分裂/合并），缓存页可能失效 | 游标与页的版本号绑定，或通过 latch crabbing 协议保证一致性 |
| **缺少 Lock Manager** | 锁直接映射到 pthread_rwlock，无法升级锁或处理锁超时 | 实现独立的锁管理器，支持锁升级、超时和死锁检测 |

## 5.8 测试与可观测性

| 不足 | 说明 | 改进方向 |
|------|------|----------|
| **缺少随机故障注入** | 测试均在"无故障"假设下运行，未验证崩溃恢复路径的鲁棒性 | 引入 fault injection：在 I/O 回调中随机模拟写入失败、页损坏等 |
| **缺少性能基准** | 现有测试只验证正确性，无吞吐量（ops/sec）和延迟分布数据 | 集成 benchmark 框架（如 Google Benchmark），记录不同负载下的性能指标 |
| **缺少内存检测** | 未使用 AddressSanitizer / Valgrind 验证内存安全 | 在 CI 中启用 ASan / UBSan / Valgrind |
| **日志级别单一** | 仅有 `printf` 输出，无分级日志系统 | 引入 spdlog 或自定义日志级别（DEBUG / INFO / WARN / ERROR） |

## 5.9 架构层面的长远方向

- **异步 I/O**：当前 I/O 是同步阻塞的，可用 `io_uring`（Linux 5.1+）或 `libaio` 实现异步读写，大幅提升 I/O 吞吐。
- **NUMA 感知**：在多 socket 服务器上，页分配和线程调度可以感知 NUMA 拓扑，减少远程内存访问延迟。
- **智能压缩**：对叶子节点数据块启用透明压缩（如 LZ4 / Zstd），以 CPU 换 I/O 带宽，适合文本类负载。
- **向量化查找**：利用 SIMD 指令（如 AVX2）在内部节点中并行比较多个分隔键，加速查找路径。
- **列式存**：对于宽值的场景，将键与值分离存储（LSM-tree 风格的分离），减少扫描时不必要的 I/O。