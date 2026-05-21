// 缓冲池 —— 位于 B+ 树与存储层之间，缓存热点页
#ifndef BTREE_BUFFER_POOL_H
#define BTREE_BUFFER_POOL_H

#include "types.h"
#include "page.h"
#include "btree.h"

#define BP_DEFAULT_CAPACITY 128

/* 不透明缓冲池句柄 */
typedef struct btree_buffer_pool btree_buffer_pool_t;

/* ─── 生命周期 ─── */

/*
 * 创建缓冲池。capacity 为最大缓存页帧数（0 表示使用默认值 128）。
 * storage_* 为下层存储层的 I/O 回调（通常是 btree_storage_*）。
 */
btree_buffer_pool_t *bp_create(uint32_t capacity,
                                btree_read_page_t storage_read,
                                btree_write_page_t storage_write,
                                btree_alloc_page_t storage_alloc,
                                void *storage_ctx);

/* 销毁缓冲池，刷出所有脏页后释放资源 */
void bp_destroy(btree_buffer_pool_t *bp);

/* ─── I/O 回调（可直接传给 btree_create / btree_open） ─── */

btree_error_t bp_read(void *ctx, page_id_t pid, page_t *page);
btree_error_t bp_write(void *ctx, page_id_t pid, const page_t *page);
btree_error_t bp_alloc(void *ctx, page_id_t *pid, page_type_t type);

/* ─── 显式刷盘 ─── */

btree_error_t bp_flush(btree_buffer_pool_t *bp, page_id_t pid);
btree_error_t bp_flush_all(btree_buffer_pool_t *bp);

/* ─── 统计 ─── */

uint32_t bp_hit_count(btree_buffer_pool_t *bp);
uint32_t bp_miss_count(btree_buffer_pool_t *bp);

#endif /* BTREE_BUFFER_POOL_H */
