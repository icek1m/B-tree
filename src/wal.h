// WAL —— 预写日志，位于缓冲池与存储层之间
#ifndef BTREE_WAL_H
#define BTREE_WAL_H

#include "types.h"
#include "page.h"
#include "btree.h"

/* 不透明 WAL 句柄 */
typedef struct wal wal_t;

/*
 * 创建 WAL 实例。
 * 包装下层存储的 I/O 回调；返回的 wal_read/wal_write/wal_alloc
 * 可直接传给 bp_create 或 btree_create。
 * wal_path 为 WAL 文件路径（NULL 表示默认 "btree.wal"）。
 * 若 WAL 文件已存在且有内容，调用者应在使用前调用 wal_recover 恢复。
 */
wal_t *wal_create(const char *wal_path,
                  btree_read_page_t storage_read,
                  btree_write_page_t storage_write,
                  btree_alloc_page_t storage_alloc,
                  void *storage_ctx);

/* 销毁 WAL 实例（关闭 WAL 文件，不自动恢复） */
void wal_destroy(wal_t *wal);

/* ─── I/O 回调（对上层透明） ─── */

btree_error_t wal_read(void *ctx, page_id_t pid, page_t *page);
btree_error_t wal_write(void *ctx, page_id_t pid, const page_t *page);
btree_error_t wal_alloc(void *ctx, page_id_t *pid, page_type_t type);

/* ─── 恢复 ─── */

/*
 * 重放 WAL 中的所有日志到 storage。
 * 必须在 btree / buffer_pool 创建之前调用。
 * 恢复成功后 WAL 文件被清空。
 */
btree_error_t wal_recover(wal_t *wal);

/* ─── 统计 ─── */

/* 当前 WAL 文件中未恢复的日志条目数 */
uint32_t wal_num_entries(wal_t *wal);

#endif /* BTREE_WAL_H */
