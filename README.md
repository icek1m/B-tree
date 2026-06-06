# B-tree

一、需求分析

- 基础键值操作：支持 写入Put(key, value)、读取Get(key) → value、删除Delete(key)。

- 范围扫描：支持 Scan(start_key, end_key) → 迭代器，能在给定范围内高效顺序遍历。

- 多数据类型：键与值均视为不透明的字节序列，由上层传入自定义比较器，引擎不预设其类型。

- 并发读写：允许多个读操作与一个写操作同时进行，接口是线程安全的。


二、核心数据结构

### 1. 基础类型与常量

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

### 2. 通用页面布局（page_t）

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

### 3. 槽位目录（slot directory）

页内数据通过槽位目录管理，目录项从页尾向前生长，空闲区从页头后向后生长。

```c
// 槽位目录项（4 字节）
typedef struct {
    uint16_t offset;  // 记录在页内的起始偏移
    uint16_t length;  // 记录长度
} slot_t;
```

### 4. 叶子节点结构

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

### 5. 内部节点结构

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

### 6. B+ 树句柄（btree_t）

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

### 7. 记录收集器（分裂辅助）

用于节点分裂时临时存放所有记录的动态数组。

```c
typedef struct {
    uint8_t  **recs;    // 每条记录独立 malloc 拷贝
    uint16_t  *lens;    // 每条记录长度
    int        count;   // 当前记录数
    int        capacity;// 最大容量
} rec_array_t;
```


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