// B+ 树核心操作接口
#ifndef BTREE_BTREE_H
#define BTREE_BTREE_H

#include "types.h"
#include "comparator.h"
#include "page.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* ─── I/O 回调：解耦存储层 ─── */
    typedef btree_error_t (*btree_read_page_t)(void *ctx, page_id_t pid, page_t *page);
    typedef btree_error_t (*btree_write_page_t)(void *ctx, page_id_t pid, const page_t *page);
    typedef btree_error_t (*btree_alloc_page_t)(void *ctx, page_id_t *pid, page_type_t type);

    /* ─── 不透明 B+ 树句柄 ─── */
    typedef struct btree btree_t;

    /* ─── 生命周期 ─── */
    btree_t *btree_create(btree_compare_t cmp,
                          btree_read_page_t read_page,
                          btree_write_page_t write_page,
                          btree_alloc_page_t alloc_page,
                          void *io_ctx);

    void btree_destroy(btree_t *tree);

    /* 打开已有树的根节点（不分配新根） */
    btree_t *btree_open(btree_compare_t cmp,
                        btree_read_page_t read_page,
                        btree_write_page_t write_page,
                        btree_alloc_page_t alloc_page,
                        void *io_ctx,
                        page_id_t root_id);

    /* 获取当前根节点页号 */
    page_id_t btree_root_id(btree_t *tree);

    /* ─── 核心操作 ─── */
    btree_error_t btree_get(btree_t *tree,
                            const uint8_t *key, uint16_t key_len,
                            uint8_t *val_out, uint16_t *val_len);

    btree_error_t btree_put(btree_t *tree,
                            const uint8_t *key, uint16_t key_len,
                            const uint8_t *val, uint16_t val_len);

    btree_error_t btree_delete(btree_t *tree,
                               const uint8_t *key, uint16_t key_len);

#ifdef __cplusplus
}
#endif

#endif /* BTREE_BTREE_H */
