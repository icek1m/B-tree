// 游标 —— B+ 树有序遍历与范围扫描
#ifndef BTREE_CURSOR_H
#define BTREE_CURSOR_H

#include "types.h"
#include "btree.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* 不透明游标句柄 */
    typedef struct btree_cursor btree_cursor_t;

    /* ─── 生命周期 ─── */

    /* 创建游标，关联到一棵 B+ 树 */
    btree_cursor_t *btree_cursor_create(btree_t *tree);
    void btree_cursor_destroy(btree_cursor_t *cursor);

    /* ─── 定位 ─── */

    /* 定位到第一个 key >= 给定 key 的记录 */
    btree_error_t btree_cursor_seek(btree_cursor_t *cursor,
                                     const uint8_t *key, uint16_t key_len);

    /* 定位到最左侧（最小 key）记录 */
    btree_error_t btree_cursor_first(btree_cursor_t *cursor);

    /* 定位到最右侧（最大 key）记录 */
    btree_error_t btree_cursor_last(btree_cursor_t *cursor);

    /* ─── 遍历 ─── */

    /* 移动到下一条记录（按 key 递增顺序） */
    btree_error_t btree_cursor_next(btree_cursor_t *cursor);

    /* 移动到上一条记录（按 key 递减顺序） */
    btree_error_t btree_cursor_prev(btree_cursor_t *cursor);

    /* ─── 读取 ─── */

    /* 读取当前记录。key_len/val_len 为 in/out 参数（入参为缓冲区容量） */
    btree_error_t btree_cursor_get(btree_cursor_t *cursor,
                                    uint8_t *key_out, uint16_t *key_len,
                                    uint8_t *val_out, uint16_t *val_len);

    /* ─── 状态 ─── */

    /* 游标是否指向有效位置 */
    bool btree_cursor_valid(btree_cursor_t *cursor);

#ifdef __cplusplus
}
#endif

#endif /* BTREE_CURSOR_H */
