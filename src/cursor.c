// 游标实现 —— 利用 B+ 树叶子节点双向链表进行有序遍历
#include "cursor.h"
#include "page.h"
#include <stdlib.h>
#include <string.h>

struct btree_cursor
{
    btree_t *tree;
    page_id_t page_id;  /* 当前所在页号 */
    page_t page;        /* 当前页面数据 */
    int slot;           /* 当前槽位，-1 表示未定位 */
};

/* ════════════════════════════════════════════════════════════════
 *  内部辅助
 * ════════════════════════════════════════════════════════════════ */

static btree_error_t read_page(btree_cursor_t *c, page_id_t pid, page_t *pg)
{
    return btree_reader(c->tree)(btree_context(c->tree), pid, pg);
}

static int compare(btree_cursor_t *c, const uint8_t *a, uint16_t al,
                   const uint8_t *b, uint16_t bl)
{
    return btree_comparator(c->tree)(a, al, b, bl);
}

/* 从根下降到包含 key 的叶子节点 */
static btree_error_t descend_to_leaf(btree_cursor_t *c,
                                     const uint8_t *key, uint16_t key_len,
                                     page_t *leaf, page_id_t *leaf_id)
{
    page_id_t pid = btree_root_id(c->tree);

    for (;;)
    {
        btree_error_t err = read_page(c, pid, leaf);
        if (err != BTREE_OK)
            return err;

        if (page_get_type(leaf) == PAGE_LEAF)
        {
            *leaf_id = pid;
            return BTREE_OK;
        }

        /* 内部节点：二分查找下降路径 */
        int n = page_num_slots(leaf);
        if (n == 0)
        {
            pid = page_get_first_child(leaf);
            continue;
        }

        int lo = 0, hi = n - 1;
        while (lo <= hi)
        {
            int mid = lo + (hi - lo) / 2;
            const uint8_t *r = page_slot_data_c(leaf, mid);
            int cv = compare(c, key, key_len,
                             internal_key_ptr_c(r), internal_key_len(r));
            if (cv < 0)
                hi = mid - 1;
            else
                lo = mid + 1;
        }
        pid = (hi < 0)
            ? page_get_first_child(leaf)
            : internal_child_id(page_slot_data_c(leaf, hi));
    }
}

/* 叶子节点内二分查找，返回槽索引或 ~插入点 */
static int leaf_search(const page_t *leaf, btree_cursor_t *c,
                       const uint8_t *key, uint16_t key_len)
{
    int lo = 0, hi = page_num_slots(leaf) - 1;
    while (lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        const uint8_t *r = page_slot_data_c(leaf, mid);
        int cv = compare(c, key, key_len,
                         leaf_key_ptr_c(r), leaf_key_len(r));
        if (cv == 0)
            return mid;
        if (cv < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return ~lo;
}

/* 沿 next 指针前进到下一个有数据的叶子节点 */
static void advance_to_next(btree_cursor_t *c)
{
    page_id_t next = page_get_next(&c->page);
    while (next != INVALID_PAGE_ID)
    {
        if (read_page(c, next, &c->page) != BTREE_OK)
        {
            c->page_id = INVALID_PAGE_ID;
            c->slot = -1;
            return;
        }
        c->page_id = next;
        if (page_num_slots(&c->page) > 0)
        {
            c->slot = 0;
            return;
        }
        next = page_get_next(&c->page);
    }
    c->page_id = INVALID_PAGE_ID;
    c->slot = -1;
}

/* 沿 prev 指针后退到上一个有数据的叶子节点 */
static void advance_to_prev(btree_cursor_t *c)
{
    page_id_t prev = page_get_prev(&c->page);
    while (prev != INVALID_PAGE_ID)
    {
        if (read_page(c, prev, &c->page) != BTREE_OK)
        {
            c->page_id = INVALID_PAGE_ID;
            c->slot = -1;
            return;
        }
        c->page_id = prev;
        int n = page_num_slots(&c->page);
        if (n > 0)
        {
            c->slot = n - 1;
            return;
        }
        prev = page_get_prev(&c->page);
    }
    c->page_id = INVALID_PAGE_ID;
    c->slot = -1;
}

/* ════════════════════════════════════════════════════════════════
 *  生命周期
 * ════════════════════════════════════════════════════════════════ */

btree_cursor_t *btree_cursor_create(btree_t *tree)
{
    btree_cursor_t *c = (btree_cursor_t *)calloc(1, sizeof(btree_cursor_t));
    if (!c)
        return NULL;
    c->tree = tree;
    c->page_id = INVALID_PAGE_ID;
    c->slot = -1;
    return c;
}

void btree_cursor_destroy(btree_cursor_t *cursor)
{
    free(cursor);
}

/* ════════════════════════════════════════════════════════════════
 *  定位
 * ════════════════════════════════════════════════════════════════ */

btree_error_t btree_cursor_seek(btree_cursor_t *c,
                                const uint8_t *key, uint16_t key_len)
{
    btree_error_t err = descend_to_leaf(c, key, key_len, &c->page, &c->page_id);
    if (err != BTREE_OK)
    {
        c->slot = -1;
        return err;
    }

    int r = leaf_search(&c->page, c, key, key_len);
    if (r >= 0)
    {
        /* 精确匹配 */
        c->slot = r;
        return BTREE_OK;
    }

    int insert_pos = ~r;
    if (insert_pos < page_num_slots(&c->page))
    {
        /* 定位到第一个 >= key 的记录 */
        c->slot = insert_pos;
        return BTREE_OK;
    }

    /* 超出当前叶子末尾，前进到下一叶子 */
    advance_to_next(c);
    return c->slot >= 0 ? BTREE_OK : BTREE_NOT_FOUND;
}

btree_error_t btree_cursor_first(btree_cursor_t *c)
{
    page_id_t pid = btree_root_id(c->tree);

    /* 沿最左路径下降到叶子 */
    for (;;)
    {
        btree_error_t err = read_page(c, pid, &c->page);
        if (err != BTREE_OK)
        {
            c->slot = -1;
            return err;
        }
        c->page_id = pid;

        if (page_get_type(&c->page) == PAGE_LEAF)
            break;

        pid = page_get_first_child(&c->page);
    }

    if (page_num_slots(&c->page) > 0)
    {
        c->slot = 0;
        return BTREE_OK;
    }

    /* 最左叶子为空（空树），尝试前进 */
    advance_to_next(c);
    return c->slot >= 0 ? BTREE_OK : BTREE_NOT_FOUND;
}

btree_error_t btree_cursor_last(btree_cursor_t *c)
{
    page_id_t pid = btree_root_id(c->tree);

    /* 沿最右路径下降到叶子 */
    for (;;)
    {
        btree_error_t err = read_page(c, pid, &c->page);
        if (err != BTREE_OK)
        {
            c->slot = -1;
            return err;
        }
        c->page_id = pid;

        if (page_get_type(&c->page) == PAGE_LEAF)
            break;

        int n = page_num_slots(&c->page);
        if (n > 0)
        {
            const uint8_t *r = page_slot_data_c(&c->page, n - 1);
            pid = internal_child_id(r);
        }
        else
        {
            pid = page_get_first_child(&c->page);
        }
    }

    int n = page_num_slots(&c->page);
    if (n > 0)
    {
        c->slot = n - 1;
        return BTREE_OK;
    }

    c->page_id = INVALID_PAGE_ID;
    c->slot = -1;
    return BTREE_NOT_FOUND;
}

/* ════════════════════════════════════════════════════════════════
 *  遍历
 * ════════════════════════════════════════════════════════════════ */

btree_error_t btree_cursor_next(btree_cursor_t *c)
{
    if (c->slot < 0)
        return BTREE_NOT_FOUND;

    c->slot++;
    if (c->slot < page_num_slots(&c->page))
        return BTREE_OK;

    advance_to_next(c);
    return c->slot >= 0 ? BTREE_OK : BTREE_NOT_FOUND;
}

btree_error_t btree_cursor_prev(btree_cursor_t *c)
{
    if (c->slot < 0)
        return BTREE_NOT_FOUND;

    c->slot--;
    if (c->slot >= 0)
        return BTREE_OK;

    advance_to_prev(c);
    return c->slot >= 0 ? BTREE_OK : BTREE_NOT_FOUND;
}

/* ════════════════════════════════════════════════════════════════
 *  读取
 * ════════════════════════════════════════════════════════════════ */

btree_error_t btree_cursor_get(btree_cursor_t *c,
                                uint8_t *key_out, uint16_t *key_len,
                                uint8_t *val_out, uint16_t *val_len)
{
    if (c->slot < 0)
        return BTREE_NOT_FOUND;

    const uint8_t *rec = page_slot_data_c(&c->page, c->slot);
    if (!rec)
        return BTREE_CORRUPTED;

    uint16_t kl = leaf_key_len(rec);
    if (key_out && key_len)
    {
        uint16_t copy = *key_len < kl ? *key_len : kl;
        memcpy(key_out, leaf_key_ptr_c(rec), copy);
    }
    if (key_len)
        *key_len = kl;

    uint16_t vl = leaf_val_len(rec);
    if (val_out && val_len)
    {
        uint16_t copy = *val_len < vl ? *val_len : vl;
        memcpy(val_out, leaf_val_ptr_c(rec), copy);
    }
    if (val_len)
        *val_len = vl;

    return BTREE_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  状态
 * ════════════════════════════════════════════════════════════════ */

bool btree_cursor_valid(btree_cursor_t *cursor)
{
    return cursor && cursor->slot >= 0;
}
