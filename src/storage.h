// 文件存储层 —— 将 B+ 树持久化到磁盘文件
#ifndef BTREE_STORAGE_H
#define BTREE_STORAGE_H

#include "types.h"
#include "page.h"
#include "btree.h"

#define BTREE_STORAGE_MAGIC   0x45525442  /* "BTRE" */
#define BTREE_STORAGE_VERSION 1
#define STORAGE_HEADER_SIZE   64

/* 不透明文件存储句柄 */
typedef struct btree_storage btree_storage_t;

/* ─── 生命周期 ─── */

/* 创建新存储文件（覆盖已存在文件） */
btree_storage_t *btree_storage_create(const char *path);

/* 打开已有存储文件 */
btree_storage_t *btree_storage_open(const char *path);

/* 关闭存储 */
void btree_storage_close(btree_storage_t *store);

/* ─── root_id 持久化 ─── */

btree_error_t btree_storage_set_root_id(btree_storage_t *store, page_id_t root_id);
btree_error_t btree_storage_get_root_id(btree_storage_t *store, page_id_t *root_id);

/* ─── I/O 回调（ctx 参数为 btree_storage_t*） ─── */

btree_error_t btree_storage_read(void *ctx, page_id_t pid, page_t *page);
btree_error_t btree_storage_write(void *ctx, page_id_t pid, const page_t *page);
btree_error_t btree_storage_alloc(void *ctx, page_id_t *pid, page_type_t type);

#endif /* BTREE_STORAGE_H */
