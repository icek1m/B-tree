// 页头、内部节点槽位、叶子节点槽位布局
#ifndef BTREE_PAGE_H
// 页面布局声明（页头、槽位、记录打包）
#define BTREE_PAGE_H

#include "types.h"
#include <string.h>

#define PAGE_HEADER_SIZE 32
#define SLOT_SIZE 4

/*
 * Page header layout (32 bytes on disk):
 *   偏移  大小  字段
 *   0     4    page_id
 *   4     1    type        (PAGE_LEAF / PAGE_INTERNAL)
 *   5     2    num_slots   (槽位目录中的条目数)
 *   7     2    free_offset (空闲区起始偏移)
 *   9     2    free_size   (空闲区字节数)
 *  11     4    parent_id
 *  15     4    prev_id     (叶子节点的前驱兄弟页号)
 *  19     4    next_id     (叶子节点的后继兄弟页号)
 *  23     4    first_child_id (内部节点: 最左子节点)
 *  27     5    reserved
 */
#pragma pack(push, 1)
typedef struct
{
    uint32_t page_id;
    uint8_t type;
    uint16_t num_slots;
    uint16_t free_offset;
    uint16_t free_size;
    uint32_t parent_id;
    uint32_t prev_id;
    uint32_t next_id;
    uint32_t first_child_id;
    uint8_t reserved[5];
} page_header_t;

/* 槽位目录项：每项 4 字节，从页尾向前生长 */
typedef struct
{
    uint16_t offset; /* 记录在页内的起始偏移 */
    uint16_t length; /* 记录长度 */
} slot_t;
#pragma pack(pop)

_Static_assert(sizeof(page_header_t) == PAGE_HEADER_SIZE,
               "page_header_t must be exactly 32 bytes");
_Static_assert(sizeof(slot_t) == SLOT_SIZE,
               "slot_t must be exactly 4 bytes");

/* 原始页面缓冲区 */
typedef union
{
    uint8_t bytes[PAGE_SIZE];
    page_header_t header;
} page_t;

/* ========== 页头操作 ========== */

/* 初始化页头，其余部分清零 */
void page_init(page_t *page, page_id_t pid, page_type_t type);

static inline page_id_t page_get_id(const page_t *p) { return p->header.page_id; }
static inline page_type_t page_get_type(const page_t *p) { return (page_type_t)p->header.type; }
static inline int page_num_slots(const page_t *p) { return p->header.num_slots; }
static inline uint16_t page_free_size(const page_t *p) { return p->header.free_size; }

static inline page_id_t page_get_parent(const page_t *p) { return p->header.parent_id; }
static inline void page_set_parent(page_t *p, page_id_t pid) { p->header.parent_id = pid; }

static inline page_id_t page_get_prev(const page_t *p) { return p->header.prev_id; }
static inline void page_set_prev(page_t *p, page_id_t pid) { p->header.prev_id = pid; }

static inline page_id_t page_get_next(const page_t *p) { return p->header.next_id; }
static inline void page_set_next(page_t *p, page_id_t pid) { p->header.next_id = pid; }

static inline page_id_t page_get_first_child(const page_t *p) { return p->header.first_child_id; }
static inline void page_set_first_child(page_t *p, page_id_t pid) { p->header.first_child_id = pid; }

/* ========== 槽位操作 ========== */

/* 分配一个新槽：将 data 写入空闲区，在目录尾部追加一个槽项，返回槽索引 */
int page_alloc_slot(page_t *page, const uint8_t *data, uint16_t len);

/* 删除指定槽（压缩数据区），保持剩余记录连续 */
void page_remove_slot(page_t *page, int idx);

/* 获取/设置槽项 */
slot_t page_get_slot(const page_t *page, int idx);
uint8_t *page_slot_data(page_t *page, int idx);
const uint8_t *page_slot_data_c(const page_t *page, int idx);

/* ========== 内部辅助：无对齐读 / 写 ========== */

static inline uint16_t read_u16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline void write_u16(uint8_t *p, uint16_t v)
{
    memcpy(p, &v, sizeof(v));
}

/* ========== 叶子节点记录操作 ========== */

/*
 * 叶子记录在槽中的二进制布局：
 *   key_len(2B) + key_data + value_len(2B) + value_data + is_deleted(1B)
 *   总开销 = 5 字节
 */

/* 计算叶子记录占用的空间 */
static inline uint16_t leaf_rec_size(uint16_t key_len, uint16_t val_len)
{
    return (uint16_t)(2 + key_len + 2 + val_len + 1);
}

/* 将叶子记录打包到 dest 缓冲区 */
static inline void leaf_rec_pack(uint8_t *dest,
                                 const uint8_t *key, uint16_t key_len,
                                 const uint8_t *val, uint16_t val_len,
                                 bool deleted)
{
    write_u16(dest, key_len);
    dest += 2;
    memcpy(dest, key, key_len);
    dest += key_len;
    write_u16(dest, val_len);
    dest += 2;
    memcpy(dest, val, val_len);
    dest += val_len;
    *dest = deleted ? 1 : 0;
}

static inline uint16_t leaf_key_len(const uint8_t *rec) { return read_u16(rec); }
static inline uint8_t *leaf_key_ptr(uint8_t *rec) { return rec + 2; }
static inline const uint8_t *leaf_key_ptr_c(const uint8_t *rec) { return rec + 2; }

static inline uint16_t leaf_val_len(const uint8_t *rec)
{
    return read_u16(rec + 2 + leaf_key_len(rec));
}
static inline uint8_t *leaf_val_ptr(uint8_t *rec)
{
    return rec + 2 + leaf_key_len(rec) + 2;
}
static inline const uint8_t *leaf_val_ptr_c(const uint8_t *rec)
{
    return rec + 2 + leaf_key_len(rec) + 2;
}

static inline bool leaf_is_deleted(const uint8_t *rec)
{
    return rec[2 + leaf_key_len(rec) + 2 + leaf_val_len(rec)] != 0;
}

/* ========== 内部节点记录操作 ========== */

/*
 * 内部节点记录在槽中的二进制布局：
 *   key_len(2B) + key_data + child_id(4B)
 *   内部节点的每个槽记录一个"分隔键"及其"右孩子页号"
 *   最左孩子通过 page->first_child_id 单独存储
 *   总开销 = 6 字节
 */

static inline uint16_t internal_rec_size(uint16_t key_len)
{
    return (uint16_t)(2 + key_len + 4);
}

static inline void internal_rec_pack(uint8_t *dest,
                                     const uint8_t *key, uint16_t key_len,
                                     page_id_t child_id)
{
    write_u16(dest, key_len);
    dest += 2;
    memcpy(dest, key, key_len);
    dest += key_len;
    memcpy(dest, &child_id, sizeof(child_id));
}

static inline uint16_t internal_key_len(const uint8_t *rec) { return read_u16(rec); }
static inline uint8_t *internal_key_ptr(uint8_t *rec) { return rec + 2; }
static inline const uint8_t *internal_key_ptr_c(const uint8_t *rec) { return rec + 2; }

static inline page_id_t internal_child_id(const uint8_t *rec)
{
    page_id_t pid;
    memcpy(&pid, rec + 2 + internal_key_len(rec), sizeof(pid));
    return pid;
}

#endif /* BTREE_PAGE_H */
