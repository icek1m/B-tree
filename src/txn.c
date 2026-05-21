// 事务实现 —— 基于读写锁的轻量级事务
#include "txn.h"
#include <stdlib.h>

struct txn
{
    btree_t *tree;
    txn_type_t type;
    bool active;
};

/* ════════════════════════════════════════════════════════════════
 *  生命周期
 * ════════════════════════════════════════════════════════════════ */

txn_t *txn_begin(btree_t *tree, txn_type_t type)
{
    txn_t *txn = (txn_t *)calloc(1, sizeof(txn_t));
    if (!txn)
        return NULL;

    txn->tree = tree;
    txn->type = type;
    txn->active = true;

    if (type == TXN_READ_ONLY)
        btree_lock_read(tree);
    else
        btree_lock_write(tree);

    return txn;
}

btree_error_t txn_commit(txn_t *txn)
{
    if (!txn || !txn->active)
        return BTREE_NOT_FOUND;

    txn->active = false;
    btree_unlock(txn->tree);
    free(txn);
    return BTREE_OK;
}

void txn_abort(txn_t *txn)
{
    if (!txn || !txn->active)
        return;

    txn->active = false;
    btree_unlock(txn->tree);
    free(txn);
}

/* ════════════════════════════════════════════════════════════════
 *  事务内操作
 * ════════════════════════════════════════════════════════════════ */

btree_error_t txn_get(txn_t *txn,
                      const uint8_t *key, uint16_t key_len,
                      uint8_t *val_out, uint16_t *val_len)
{
    if (!txn || !txn->active)
        return BTREE_NOT_FOUND;

    return btree_get(txn->tree, key, key_len, val_out, val_len);
}

btree_error_t txn_put(txn_t *txn,
                      const uint8_t *key, uint16_t key_len,
                      const uint8_t *val, uint16_t val_len)
{
    if (!txn || !txn->active)
        return BTREE_NOT_FOUND;
    if (txn->type != TXN_READ_WRITE)
        return BTREE_CORRUPTED; /* 只读事务不能写入 */

    return btree_put(txn->tree, key, key_len, val, val_len);
}

btree_error_t txn_delete(txn_t *txn,
                         const uint8_t *key, uint16_t key_len)
{
    if (!txn || !txn->active)
        return BTREE_NOT_FOUND;
    if (txn->type != TXN_READ_WRITE)
        return BTREE_CORRUPTED;

    return btree_delete(txn->tree, key, key_len);
}

btree_cursor_t *txn_cursor_create(txn_t *txn)
{
    if (!txn || !txn->active)
        return NULL;

    return btree_cursor_create(txn->tree);
}

bool txn_active(txn_t *txn) { return txn && txn->active; }
txn_type_t txn_type(txn_t *txn) { return txn ? txn->type : TXN_READ_ONLY; }
