// 缓冲池实现 —— LRU 淘汰、哈希表查找、脏页追踪
#include "buffer_pool.h"
#include <stdlib.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════
 *  内部常量 & 类型
 * ════════════════════════════════════════════════════════════════ */

/* 哈希表槽位状态 */
#define BP_EMPTY   0
#define BP_TOMB    1  /* 墓碑：曾被占用，查找时继续探测 */
#define BP_OCCUPIED 2

/* 一个缓存帧 */
typedef struct
{
    page_t page;         /* 页数据 */
    page_id_t pid;       /* 缓存的页号 */
    bool used;           /* true = 帧已被占用（不能用 pid==0 判断，因为 page 0 是合法根页） */
    bool dirty;          /* 已被修改，需要刷盘 */
    uint32_t access_stamp;
} bp_frame_t;

/* 哈希表槽位（开放定址 + 线性探测） */
typedef struct
{
    uint8_t state;       /* BP_EMPTY / BP_TOMB / BP_OCCUPIED */
    page_id_t pid;
    int frame_idx;       /* 对应 frames[] 索引 */
} bp_hash_slot_t;

/* 缓冲池主结构 */
struct btree_buffer_pool
{
    bp_frame_t *frames;
    uint32_t capacity;
    uint32_t access_counter;  /* 全局访问计数，用于 LRU 比较 */

    bp_hash_slot_t *hash_table;
    uint32_t hash_size;       /* 总是 2 的幂 */

    btree_read_page_t  storage_read;
    btree_write_page_t storage_write;
    btree_alloc_page_t storage_alloc;
    void *storage_ctx;

    uint32_t hits;
    uint32_t misses;
};

/* ════════════════════════════════════════════════════════════════
 *  工具函数
 * ════════════════════════════════════════════════════════════════ */

/* 将 v 向上取整到 2 的幂（v > 0） */
static uint32_t next_pow2(uint32_t v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

/* 哈希函数（hash_size 必须是 2 的幂） */
static inline uint32_t bp_hash(page_id_t pid, uint32_t hash_size)
{
    return (pid * 2654435761u) & (hash_size - 1);
}

/* ════════════════════════════════════════════════════════════════
 *  哈希表操作
 * ════════════════════════════════════════════════════════════════ */

/* 查找 pid，返回 frame_idx；未找到返回 -1 */
static int ht_lookup(btree_buffer_pool_t *bp, page_id_t pid)
{
    uint32_t h = bp_hash(pid, bp->hash_size);
    for (uint32_t i = 0; i < bp->hash_size; i++)
    {
        uint32_t slot = (h + i) & (bp->hash_size - 1);
        bp_hash_slot_t *s = &bp->hash_table[slot];
        if (s->state == BP_EMPTY)
            return -1;
        if (s->state == BP_OCCUPIED && s->pid == pid)
            return s->frame_idx;
        /* BP_TOMB：继续探测 */
    }
    return -1; /* 理论上不应到达 */
}

/* 插入 (pid, frame_idx) */
static void ht_insert(btree_buffer_pool_t *bp, page_id_t pid, int frame_idx)
{
    uint32_t h = bp_hash(pid, bp->hash_size);
    for (uint32_t i = 0; i < bp->hash_size; i++)
    {
        uint32_t slot = (h + i) & (bp->hash_size - 1);
        bp_hash_slot_t *s = &bp->hash_table[slot];
        if (s->state != BP_OCCUPIED)
        {
            s->state = BP_OCCUPIED;
            s->pid = pid;
            s->frame_idx = frame_idx;
            return;
        }
    }
    /* 哈希表满 —— 不会发生，hash_size 设计为 capacity 的 2 倍以上 */
}

/* 移除 pid 对应的哈希条目 */
static void ht_remove(btree_buffer_pool_t *bp, page_id_t pid)
{
    uint32_t h = bp_hash(pid, bp->hash_size);
    for (uint32_t i = 0; i < bp->hash_size; i++)
    {
        uint32_t slot = (h + i) & (bp->hash_size - 1);
        bp_hash_slot_t *s = &bp->hash_table[slot];
        if (s->state == BP_EMPTY)
            return;
        if (s->state == BP_OCCUPIED && s->pid == pid)
        {
            s->state = BP_TOMB;
            return;
        }
    }
}

/* ════════════════════════════════════════════════════════════════
 *  LRU 淘汰
 * ════════════════════════════════════════════════════════════════ */

/*
 * 淘汰一帧：返回可用帧索引。
 * 优先返回空闲帧；若无空闲则全局扫描找出 access_stamp 最小的帧淘汰。
 * 被淘汰的脏页会先刷盘。
 */
static int evict_one(btree_buffer_pool_t *bp)
{
    /* 1. 找空闲帧 */
    for (uint32_t i = 0; i < bp->capacity; i++)
    {
        if (!bp->frames[i].used)
            return (int)i;
    }

    /* 2. LRU 扫描：找 access_stamp 最小的帧淘汰 */
    int victim = 0;
    uint32_t oldest = bp->frames[0].access_stamp;
    for (uint32_t i = 1; i < bp->capacity; i++)
    {
        if (bp->frames[i].access_stamp < oldest)
        {
            oldest = bp->frames[i].access_stamp;
            victim = (int)i;
        }
    }

    /* 3. 若脏则刷盘 */
    if (bp->frames[victim].dirty)
    {
        bp->storage_write(bp->storage_ctx,
                          bp->frames[victim].pid,
                          &bp->frames[victim].page);
    }

    /* 4. 从哈希表移除 */
    ht_remove(bp, bp->frames[victim].pid);

    /* 5. 清空帧 */
    bp->frames[victim].used = false;
    bp->frames[victim].dirty = false;
    bp->frames[victim].access_stamp = 0;

    return victim;
}

/* ════════════════════════════════════════════════════════════════
 *  生命周期
 * ════════════════════════════════════════════════════════════════ */

btree_buffer_pool_t *bp_create(uint32_t capacity,
                                btree_read_page_t storage_read,
                                btree_write_page_t storage_write,
                                btree_alloc_page_t storage_alloc,
                                void *storage_ctx)
{
    if (capacity == 0)
        capacity = BP_DEFAULT_CAPACITY;

    btree_buffer_pool_t *bp = (btree_buffer_pool_t *)
        calloc(1, sizeof(btree_buffer_pool_t));
    if (!bp)
        return NULL;

    bp->frames = (bp_frame_t *)calloc(capacity, sizeof(bp_frame_t));
    if (!bp->frames)
    {
        free(bp);
        return NULL;
    }

    uint32_t hs = next_pow2(capacity * 2);
    bp->hash_table = (bp_hash_slot_t *)calloc(hs, sizeof(bp_hash_slot_t));
    if (!bp->hash_table)
    {
        free(bp->frames);
        free(bp);
        return NULL;
    }

    bp->capacity = capacity;
    bp->hash_size = hs;
    bp->access_counter = 0;
    /* frames[].used = false 由 calloc 保证 */

    bp->storage_read  = storage_read;
    bp->storage_write = storage_write;
    bp->storage_alloc = storage_alloc;
    bp->storage_ctx   = storage_ctx;

    bp->hits = 0;
    bp->misses = 0;

    return bp;
}

void bp_destroy(btree_buffer_pool_t *bp)
{
    if (!bp)
        return;

    bp_flush_all(bp);

    free(bp->frames);
    free(bp->hash_table);
    free(bp);
}

/* ════════════════════════════════════════════════════════════════
 *  I/O 回调
 * ════════════════════════════════════════════════════════════════ */

btree_error_t bp_read(void *ctx, page_id_t pid, page_t *page)
{
    btree_buffer_pool_t *bp = (btree_buffer_pool_t *)ctx;

    int fi = ht_lookup(bp, pid);
    if (fi >= 0)
    {
        /* 缓存命中 */
        bp->hits++;
        bp->frames[fi].access_stamp = ++bp->access_counter;
        *page = bp->frames[fi].page;
        return BTREE_OK;
    }

    /* 缓存未命中 */
    bp->misses++;

    fi = evict_one(bp);
    if (fi < 0)
        return BTREE_IO_ERROR;

    btree_error_t err = bp->storage_read(bp->storage_ctx, pid,
                                          &bp->frames[fi].page);
    if (err != BTREE_OK)
        return err;

    bp->frames[fi].used = true;
    bp->frames[fi].pid = pid;
    bp->frames[fi].dirty = false;
    bp->frames[fi].access_stamp = ++bp->access_counter;

    ht_insert(bp, pid, fi);

    *page = bp->frames[fi].page;
    return BTREE_OK;
}

btree_error_t bp_write(void *ctx, page_id_t pid, const page_t *page)
{
    btree_buffer_pool_t *bp = (btree_buffer_pool_t *)ctx;

    int fi = ht_lookup(bp, pid);
    if (fi < 0)
    {
        /* 页不在缓存中：分配一帧 */
        fi = evict_one(bp);
        if (fi < 0)
            return BTREE_IO_ERROR;

        bp->frames[fi].page = *page;
        bp->frames[fi].used = true;
        bp->frames[fi].pid = pid;
        bp->frames[fi].dirty = true;
        bp->frames[fi].access_stamp = ++bp->access_counter;

        ht_insert(bp, pid, fi);
        return BTREE_OK;
    }

    bp->frames[fi].page = *page;
    bp->frames[fi].dirty = true;
    bp->frames[fi].access_stamp = ++bp->access_counter;
    return BTREE_OK;
}

btree_error_t bp_alloc(void *ctx, page_id_t *pid, page_type_t type)
{
    btree_buffer_pool_t *bp = (btree_buffer_pool_t *)ctx;
    return bp->storage_alloc(bp->storage_ctx, pid, type);
}

/* ════════════════════════════════════════════════════════════════
 *  显式刷盘
 * ════════════════════════════════════════════════════════════════ */

btree_error_t bp_flush(btree_buffer_pool_t *bp, page_id_t pid)
{
    int fi = ht_lookup(bp, pid);
    if (fi < 0)
        return BTREE_OK; /* 不在缓存中，无需刷盘 */

    if (bp->frames[fi].dirty)
    {
        btree_error_t err = bp->storage_write(bp->storage_ctx, pid,
                                               &bp->frames[fi].page);
        if (err != BTREE_OK)
            return err;
        bp->frames[fi].dirty = false;
    }
    return BTREE_OK;
}

btree_error_t bp_flush_all(btree_buffer_pool_t *bp)
{
    for (uint32_t i = 0; i < bp->capacity; i++)
    {
        if (bp->frames[i].used && bp->frames[i].dirty)
        {
            btree_error_t err = bp->storage_write(bp->storage_ctx,
                                                   bp->frames[i].pid,
                                                   &bp->frames[i].page);
            if (err != BTREE_OK)
                return err;
            bp->frames[i].dirty = false;
        }
    }
    return BTREE_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  统计
 * ════════════════════════════════════════════════════════════════ */

uint32_t bp_hit_count(btree_buffer_pool_t *bp)  { return bp->hits; }
uint32_t bp_miss_count(btree_buffer_pool_t *bp) { return bp->misses; }
