// 事务 —— 基于读写锁的轻量级事务
#ifndef BTREE_TXN_H
#define BTREE_TXN_H

#include "types.h"
#include "btree.h"
#include "cursor.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        TXN_READ_ONLY = 0,
        TXN_READ_WRITE = 1,
    } txn_type_t;

    /* 不透明事务句柄 */
    typedef struct txn txn_t;

    /* ─── 生命周期 ─── */

    /* 开启事务。type 指定只读或读写。返回 NULL 表示失败 */
    txn_t *txn_begin(btree_t *tree, txn_type_t type);

    /* 提交事务（释放锁）。返回 BTREE_OK */
    btree_error_t txn_commit(txn_t *txn);

    /* 中止事务（释放锁，写事务的修改不会回滚——方案B的约束） */
    void txn_abort(txn_t *txn);

    /* ─── 事务内操作 ─── */

    btree_error_t txn_get(txn_t *txn,
                          const uint8_t *key, uint16_t key_len,
                          uint8_t *val_out, uint16_t *val_len);

    btree_error_t txn_put(txn_t *txn,
                          const uint8_t *key, uint16_t key_len,
                          const uint8_t *val, uint16_t val_len);

    btree_error_t txn_delete(txn_t *txn,
                             const uint8_t *key, uint16_t key_len);

    /* ─── 游标 ─── */

    /* 在当前事务中创建游标。游标生命周期不能超过事务。 */
    btree_cursor_t *txn_cursor_create(txn_t *txn);

    /* ─── 查询 ─── */

    /* 事务是否仍处于活跃状态（未 commit/abort） */
    bool txn_active(txn_t *txn);

    /* 事务类型 */
    txn_type_t txn_type(txn_t *txn);

#ifdef __cplusplus
}
#endif

#endif /* BTREE_TXN_H */
