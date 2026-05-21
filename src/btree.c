// B+ 树核心操作实现
#include "btree.h"
#include <stdlib.h>
#include <string.h>

#define BTREE_MAX_DEPTH 64

/* ─── B+ 树句柄 ─── */
struct btree
{
    btree_compare_t cmp;
    btree_read_page_t read_page;
    btree_write_page_t write_page;
    btree_alloc_page_t alloc_page;
    void *io_ctx;
    page_id_t root_id;
};

/* ════════════════════════════════════════════════════════════════
 *  记录收集器 —— 用于分裂时临时存放记录
 * ════════════════════════════════════════════════════════════════ */

typedef struct
{
    uint8_t **recs; /* 每条记录独立 malloc 拷贝 */
    uint16_t *lens; /* 每条记录长度 */
    int count;
    int capacity;
} rec_array_t;

static void rec_array_destroy(rec_array_t *ra)
{
    if (ra->recs)
    {
        for (int i = 0; i < ra->count; i++)
            free(ra->recs[i]);
        free(ra->recs);
    }
    free(ra->lens);
}

static int rec_array_init(rec_array_t *ra, int capacity)
{
    ra->recs = (uint8_t **)calloc((size_t)capacity, sizeof(uint8_t *));
    ra->lens = (uint16_t *)malloc((size_t)capacity * sizeof(uint16_t));
    ra->count = 0;
    ra->capacity = capacity;
    if (!ra->recs || !ra->lens)
    {
        rec_array_destroy(ra);
        return -1;
    }
    return 0;
}

static int rec_array_add(rec_array_t *ra, const uint8_t *data, uint16_t len)
{
    if (ra->count >= ra->capacity)
        return -1;
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy)
        return -1;
    memcpy(copy, data, len);
    ra->recs[ra->count] = copy;
    ra->lens[ra->count] = len;
    ra->count++;
    return 0;
}

/*
 * 将一条新记录归并入已按 key 有序的数组中。
 * 记录格式满足：offset 0 = key_len(uint16)，offset 2 = key_data。
 */
static int rec_array_merge_sorted(rec_array_t *ra, const uint8_t *rec,
                                  uint16_t len, btree_compare_t cmp)
{
    if (ra->count >= ra->capacity)
        return -1;

    int lo = 0, hi = ra->count - 1;
    while (lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        int c = cmp(rec + 2, read_u16(rec),
                    ra->recs[mid] + 2, read_u16(ra->recs[mid]));
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    int pos = lo;

    memmove(&ra->recs[pos + 1], &ra->recs[pos],
            (size_t)(ra->count - pos) * sizeof(uint8_t *));
    memmove(&ra->lens[pos + 1], &ra->lens[pos],
            (size_t)(ra->count - pos) * sizeof(uint16_t));

    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy)
        return -1;
    memcpy(copy, rec, len);
    ra->recs[pos] = copy;
    ra->lens[pos] = len;
    ra->count++;
    return pos;
}

/* ════════════════════════════════════════════════════════════════
 *  二分查找辅助
 * ════════════════════════════════════════════════════════════════ */

/* 内部节点：返回 key 应下降到的子节点页号 */
static page_id_t internal_find_child(const page_t *page, btree_compare_t cmp,
                                     const uint8_t *key, uint16_t key_len)
{
    int n = page_num_slots(page);
    if (n == 0)
        return page_get_first_child(page);

    int lo = 0, hi = n - 1;
    while (lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        const uint8_t *r = page_slot_data_c(page, mid);
        int c = cmp(key, key_len, internal_key_ptr_c(r), internal_key_len(r));
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    if (hi < 0)
        return page_get_first_child(page);

    return internal_child_id(page_slot_data_c(page, hi));
}

/*
 * 叶子节点：返回 key 所在槽索引，未找到返回 ~insert_pos。
 * insert_pos 是最靠左的插入位置（保持全部 key 有序）。
 */
static int leaf_find_key(const page_t *page, btree_compare_t cmp,
                         const uint8_t *key, uint16_t key_len)
{
    int lo = 0, hi = page_num_slots(page) - 1;
    while (lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        const uint8_t *r = page_slot_data_c(page, mid);
        int c = cmp(key, key_len, leaf_key_ptr_c(r), leaf_key_len(r));
        if (c == 0)
            return mid;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return ~lo;
}

/* ════════════════════════════════════════════════════════════════
 *  路径遍历：从根到叶子
 * ════════════════════════════════════════════════════════════════ */

typedef struct
{
    page_id_t pid;
    page_t page;
} path_entry_t;

typedef struct
{
    path_entry_t entries[BTREE_MAX_DEPTH];
    int depth;
} path_t;

static btree_error_t build_path(btree_t *tree, path_t *path,
                                const uint8_t *key, uint16_t key_len)
{
    path->depth = 0;
    page_id_t pid = tree->root_id;

    for (;;)
    {
        path_entry_t *e = &path->entries[path->depth];
        e->pid = pid;
        btree_error_t err = tree->read_page(tree->io_ctx, pid, &e->page);
        if (err != BTREE_OK)
            return err;
        path->depth++;

        if (page_get_type(&e->page) == PAGE_LEAF)
            return BTREE_OK;

        pid = internal_find_child(&e->page, tree->cmp, key, key_len);
    }
}

/* ════════════════════════════════════════════════════════════════
 *  子节点 parent_id 更新
 * ════════════════════════════════════════════════════════════════ */

static btree_error_t set_parent_id(btree_t *tree, page_id_t child_pid,
                                   page_id_t parent_pid)
{
    page_t pg;
    btree_error_t err = tree->read_page(tree->io_ctx, child_pid, &pg);
    if (err != BTREE_OK)
        return err;
    page_set_parent(&pg, parent_pid);
    return tree->write_page(tree->io_ctx, child_pid, &pg);
}

/* ════════════════════════════════════════════════════════════════
 *  删除再平衡辅助
 * ════════════════════════════════════════════════════════════════ */

/* 在父节点中查找 child_pid 的位置：-1 = first_child，≥0 = slot 索引 */
static int find_pos_in_parent(const page_t *parent, page_id_t child_pid)
{
    if (page_get_first_child(parent) == child_pid)
        return -1;
    int n = page_num_slots(parent);
    for (int i = 0; i < n; i++)
    {
        const uint8_t *r = page_slot_data_c(parent, i);
        if (r && internal_child_id(r) == child_pid)
            return i;
    }
    return -2;
}

/* 从叶子层开始向上再平衡。仅处理叶子合并 + 根收缩 */
static btree_error_t rebalance_after_delete(btree_t *tree, path_t *path,
                                             int leaf_level)
{
    int level = leaf_level;

    while (level > 0)
    {
        page_t parent;
        page_id_t parent_pid = path->entries[level - 1].pid;
        btree_error_t err = tree->read_page(tree->io_ctx, parent_pid, &parent);
        if (err != BTREE_OK) return err;

        page_t node;
        page_id_t node_pid = path->entries[level].pid;
        err = tree->read_page(tree->io_ctx, node_pid, &node);
        if (err != BTREE_OK) return err;

        bool is_leaf = (page_get_type(&node) == PAGE_LEAF);
        int n = page_num_slots(&node);

        /* 阈值：叶子 < 2 或内部节点 < 1 时触发 */
        if (is_leaf ? (n >= 2) : (n >= 1))
            break;

        int pos = find_pos_in_parent(&parent, node_pid);
        if (pos < -1) return BTREE_CORRUPTED;

        /* 确定合并方向：始终将右节点合并到左节点 */
        page_id_t left_pid, right_pid;
        int remove_slot;

        if (pos >= 0)
        {
            left_pid = (pos == 0)
                ? page_get_first_child(&parent)
                : internal_child_id(page_slot_data_c(&parent, pos - 1));
            right_pid = node_pid;
            remove_slot = pos;
        }
        else
        {
            /* 当前节点是 first_child，右兄弟在 slot[0] */
            left_pid = node_pid;
            right_pid = internal_child_id(page_slot_data_c(&parent, 0));
            remove_slot = 0;
        }

        page_t left, right;
        err = tree->read_page(tree->io_ctx, left_pid, &left);
        if (err != BTREE_OK) return err;
        err = tree->read_page(tree->io_ctx, right_pid, &right);
        if (err != BTREE_OK) return err;

        /* 检查合并可行性 */
        uint16_t left_data  = (uint16_t)(PAGE_SIZE - left.header.free_offset);
        uint16_t right_data = (uint16_t)(PAGE_SIZE - right.header.free_offset);
        uint16_t total_slots = (uint16_t)(page_num_slots(&left)
                                        + page_num_slots(&right));
        if ((uint16_t)(total_slots * sizeof(slot_t)) + left_data + right_data
            > PAGE_SIZE - PAGE_HEADER_SIZE)
            break; /* 装不下，放弃合并 */

        /* 右叶记录追加到左叶 */
        int nr = page_num_slots(&right);
        for (int i = 0; i < nr; i++)
        {
            slot_t s = page_get_slot(&right, i);
            const uint8_t *d = page_slot_data_c(&right, i);
            if (d)
                page_alloc_slot(&left, d, s.length);
        }

        /* 修正兄弟链表 */
        page_id_t right_next = page_get_next(&right);
        page_set_next(&left, right_next);
        if (right_next != INVALID_PAGE_ID)
        {
            page_t nxt;
            err = tree->read_page(tree->io_ctx, right_next, &nxt);
            if (err != BTREE_OK) return err;
            page_set_prev(&nxt, left_pid);
            err = tree->write_page(tree->io_ctx, right_next, &nxt);
            if (err != BTREE_OK) return err;
        }

        /* 从父节点移除分隔键槽位 */
        page_remove_slot(&parent, remove_slot);

        err = tree->write_page(tree->io_ctx, left_pid, &left);
        if (err != BTREE_OK) return err;
        err = tree->write_page(tree->io_ctx, parent_pid, &parent);
        if (err != BTREE_OK) return err;

        level--;
    }

    /* ── 根收缩 ── */
    if (level == 0)
    {
        page_t root;
        btree_error_t err = tree->read_page(tree->io_ctx,
                                             path->entries[0].pid, &root);
        if (err != BTREE_OK) return err;

        if (page_get_type(&root) == PAGE_INTERNAL && page_num_slots(&root) == 0)
        {
            page_id_t child = page_get_first_child(&root);
            tree->root_id = child;
            page_t child_pg;
            err = tree->read_page(tree->io_ctx, child, &child_pg);
            if (err != BTREE_OK) return err;
            page_set_parent(&child_pg, INVALID_PAGE_ID);
            err = tree->write_page(tree->io_ctx, child, &child_pg);
            if (err != BTREE_OK) return err;
        }
    }

    return BTREE_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  公共 API
 * ════════════════════════════════════════════════════════════════ */

btree_t *btree_create(btree_compare_t cmp,
                      btree_read_page_t read_page,
                      btree_write_page_t write_page,
                      btree_alloc_page_t alloc_page,
                      void *io_ctx)
{
    btree_t *tree = (btree_t *)calloc(1, sizeof(btree_t));
    if (!tree)
        return NULL;

    tree->cmp = cmp;
    tree->read_page = read_page;
    tree->write_page = write_page;
    tree->alloc_page = alloc_page;
    tree->io_ctx = io_ctx;
    tree->root_id = INVALID_PAGE_ID;

    page_id_t root_id;
    btree_error_t err = alloc_page(io_ctx, &root_id, PAGE_LEAF);
    if (err != BTREE_OK)
    {
        free(tree);
        return NULL;
    }
    tree->root_id = root_id;
    return tree;
}

btree_t *btree_open(btree_compare_t cmp,
                    btree_read_page_t read_page,
                    btree_write_page_t write_page,
                    btree_alloc_page_t alloc_page,
                    void *io_ctx,
                    page_id_t root_id)
{
    btree_t *tree = (btree_t *)calloc(1, sizeof(btree_t));
    if (!tree)
        return NULL;

    tree->cmp = cmp;
    tree->read_page = read_page;
    tree->write_page = write_page;
    tree->alloc_page = alloc_page;
    tree->io_ctx = io_ctx;
    tree->root_id = root_id;
    return tree;
}

page_id_t btree_root_id(btree_t *tree)
{
    return tree->root_id;
}

void btree_destroy(btree_t *tree)
{
    free(tree);
}

btree_error_t btree_get(btree_t *tree,
                        const uint8_t *key, uint16_t key_len,
                        uint8_t *val_out, uint16_t *val_len)
{
    path_t path;
    btree_error_t err = build_path(tree, &path, key, key_len);
    if (err != BTREE_OK)
        return err;

    const page_t *leaf = &path.entries[path.depth - 1].page;
    int idx = leaf_find_key(leaf, tree->cmp, key, key_len);
    if (idx < 0)
        return BTREE_NOT_FOUND;

    const uint8_t *rec = page_slot_data_c(leaf, idx);
    if (!rec || leaf_is_deleted(rec))
        return BTREE_NOT_FOUND;

    uint16_t vlen = leaf_val_len(rec);
    if (val_out)
    {
        uint16_t copy_len = *val_len < vlen ? *val_len : vlen;
        memcpy(val_out, leaf_val_ptr_c(rec), copy_len);
    }
    *val_len = vlen;
    return BTREE_OK;
}

btree_error_t btree_delete(btree_t *tree,
                           const uint8_t *key, uint16_t key_len)
{
    path_t path;
    btree_error_t err = build_path(tree, &path, key, key_len);
    if (err != BTREE_OK)
        return err;

    page_t *leaf = &path.entries[path.depth - 1].page;
    page_id_t leaf_id = path.entries[path.depth - 1].pid;

    int idx = leaf_find_key(leaf, tree->cmp, key, key_len);
    if (idx < 0)
        return BTREE_NOT_FOUND;

    uint8_t *rec = page_slot_data(leaf, idx);
    if (!rec || leaf_is_deleted(rec))
        return BTREE_NOT_FOUND;

    /* 物理删除 */
    page_remove_slot(leaf, idx);
    err = tree->write_page(tree->io_ctx, leaf_id, leaf);
    if (err != BTREE_OK)
        return err;

    /* 再平衡（叶子层到根） */
    return rebalance_after_delete(tree, &path, path.depth - 1);
}

btree_error_t btree_put(btree_t *tree,
                        const uint8_t *key, uint16_t key_len,
                        const uint8_t *val, uint16_t val_len)
{
    path_t path;
    btree_error_t err = build_path(tree, &path, key, key_len);
    if (err != BTREE_OK)
        return err;

    /* ── 定位目标叶子 ── */
    page_t *leaf = &path.entries[path.depth - 1].page;
    page_id_t leaf_id = path.entries[path.depth - 1].pid;

    /* 若 key 已存在（含逻辑删除），先物理移除旧记录 */
    int idx = leaf_find_key(leaf, tree->cmp, key, key_len);
    if (idx >= 0)
    {
        page_remove_slot(leaf, idx);
        idx = leaf_find_key(leaf, tree->cmp, key, key_len); /* 重新定位 */
    }

    /* ── 打包新记录 ── */
    uint8_t rec_buf[PAGE_SIZE];
    uint16_t rec_len = leaf_rec_size(key_len, val_len);
    leaf_rec_pack(rec_buf, key, key_len, val, val_len, false);

    int insert_pos = ~idx; /* 二分给出的插入点 */
    int result = page_insert_slot(leaf, insert_pos, rec_buf, rec_len);
    if (result >= 0)
        return tree->write_page(tree->io_ctx, leaf_id, leaf);

    /* ═══════════════════════════════════════════════════════════
     *  叶子满 → 分裂
     * ═══════════════════════════════════════════════════════════ */

    /* 1. 收集所有已有记录 + 新记录 */
    int n = page_num_slots(leaf);
    rec_array_t ra;
    if (rec_array_init(&ra, n + 1) != 0)
        return BTREE_OUT_OF_MEMORY;

    for (int i = 0; i < n; i++)
    {
        const uint8_t *d = page_slot_data_c(leaf, i);
        slot_t s = page_get_slot(leaf, i);
        if (d)
            rec_array_add(&ra, d, s.length);
    }
    rec_array_merge_sorted(&ra, rec_buf, rec_len, tree->cmp);

    /* 2. 中点分割 */
    int mid = ra.count / 2; /* 右半首条记录索引 */

    /* 分隔键 = 右半首条记录的 key */
    uint16_t sep_len = read_u16(ra.recs[mid]);
    uint8_t *sep_key_buf = (uint8_t *)malloc(sep_len);
    if (!sep_key_buf)
    {
        rec_array_destroy(&ra);
        return BTREE_OUT_OF_MEMORY;
    }
    memcpy(sep_key_buf, ra.recs[mid] + 2, sep_len);

    /* 保存叶子原有链接 */
    page_id_t old_prev = page_get_prev(leaf);
    page_id_t old_next = page_get_next(leaf);
    page_id_t parent_id = page_get_parent(leaf);

    /* 3. 重写左半到原叶子 */
    page_init(leaf, leaf_id, PAGE_LEAF);
    page_set_parent(leaf, parent_id);
    page_set_prev(leaf, old_prev);
    page_set_next(leaf, leaf_id); /* 临时自指，后续创建右页后修正 */

    for (int i = 0; i < mid; i++)
        page_alloc_slot(leaf, ra.recs[i], ra.lens[i]);

    /* 4. 创建新右叶子 */
    page_id_t new_pid;
    err = tree->alloc_page(tree->io_ctx, &new_pid, PAGE_LEAF);
    if (err != BTREE_OK)
    {
        free(sep_key_buf);
        rec_array_destroy(&ra);
        return err;
    }

    page_t new_page;
    tree->read_page(tree->io_ctx, new_pid, &new_page);
    page_init(&new_page, new_pid, PAGE_LEAF);
    page_set_parent(&new_page, parent_id);
    page_set_prev(&new_page, leaf_id);
    page_set_next(&new_page, old_next);

    for (int i = mid; i < ra.count; i++)
        page_alloc_slot(&new_page, ra.recs[i], ra.lens[i]);

    /* 5. 修正左叶子的 next 指针 */
    page_set_next(leaf, new_pid);

    /* 6. 写回左右叶子 */
    tree->write_page(tree->io_ctx, leaf_id, leaf);
    tree->write_page(tree->io_ctx, new_pid, &new_page);

    /* 7. 修正兄弟指针 */
    if (old_next != INVALID_PAGE_ID)
    {
        page_t next_pg;
        tree->read_page(tree->io_ctx, old_next, &next_pg);
        page_set_prev(&next_pg, new_pid);
        tree->write_page(tree->io_ctx, old_next, &next_pg);
    }
    if (old_prev != INVALID_PAGE_ID)
    {
        page_t prev_pg;
        tree->read_page(tree->io_ctx, old_prev, &prev_pg);
        page_set_next(&prev_pg, leaf_id);
        tree->write_page(tree->io_ctx, old_prev, &prev_pg);
    }

    rec_array_destroy(&ra);

    /* ═══════════════════════════════════════════════════════════
     *  分隔键上溯传播
     * ═══════════════════════════════════════════════════════════ */

    page_id_t cur_pid = new_pid; /* 新分裂出的页号 */
    uint8_t *cur_sep = sep_key_buf;
    uint16_t cur_sep_len = sep_len;
    int level = path.depth - 2; /* 父节点所在层次 */

    for (;;)
    {
        if (level < 0)
        {
            /* 根已满 → 创建新根 */
            page_t root_pg;
            page_id_t root_pid;

            err = tree->alloc_page(tree->io_ctx, &root_pid, PAGE_INTERNAL);
            if (err != BTREE_OK)
                goto out;

            tree->read_page(tree->io_ctx, root_pid, &root_pg);
            page_init(&root_pg, root_pid, PAGE_INTERNAL);
            page_set_first_child(&root_pg, tree->root_id);
            set_parent_id(tree, tree->root_id, root_pid);

            uint8_t internal_buf[PAGE_SIZE];
            internal_rec_pack(internal_buf, cur_sep, cur_sep_len, cur_pid);
            page_alloc_slot(&root_pg, internal_buf, internal_rec_size(cur_sep_len));
            set_parent_id(tree, cur_pid, root_pid);

            tree->write_page(tree->io_ctx, root_pid, &root_pg);
            tree->root_id = root_pid;
            err = BTREE_OK;
            goto out;
        }

        /* 读父节点 */
        page_t parent_pg;
        page_id_t parent_pid = path.entries[level].pid;

        err = tree->read_page(tree->io_ctx, parent_pid, &parent_pg);
        if (err != BTREE_OK)
            goto out;

        /* 构造内部记录并找插入位置 */
        uint8_t internal_buf[PAGE_SIZE];
        internal_rec_pack(internal_buf, cur_sep, cur_sep_len, cur_pid);
        uint16_t internal_len = internal_rec_size(cur_sep_len);

        /* 父节点中二分查找插入位置 */
        int np = page_num_slots(&parent_pg);
        int lo = 0, hi = np - 1;
        while (lo <= hi)
        {
            int m = lo + (hi - lo) / 2;
            const uint8_t *r = page_slot_data_c(&parent_pg, m);
            int c = tree->cmp(cur_sep, cur_sep_len,
                              internal_key_ptr_c(r), internal_key_len(r));
            if (c < 0)
                hi = m - 1;
            else
                lo = m + 1;
        }
        int ins_pos = lo;

        result = page_insert_slot(&parent_pg, ins_pos, internal_buf, internal_len);
        if (result >= 0)
        {
            /* 插入成功 */
            err = tree->write_page(tree->io_ctx, parent_pid, &parent_pg);
            goto out;
        }

        /* ── 父节点也满 → 分裂 ── */

        rec_array_t ra2;
        if (rec_array_init(&ra2, np + 1) != 0)
        {
            err = BTREE_OUT_OF_MEMORY;
            goto out;
        }

        for (int i = 0; i < np; i++)
        {
            const uint8_t *d = page_slot_data_c(&parent_pg, i);
            slot_t s = page_get_slot(&parent_pg, i);
            if (d)
                rec_array_add(&ra2, d, s.length);
        }
        rec_array_merge_sorted(&ra2, internal_buf, internal_len, tree->cmp);

        int mid2 = ra2.count / 2;

        uint8_t *median_rec = ra2.recs[mid2];
        uint16_t median_key_len = read_u16(median_rec);
        page_id_t median_child = internal_child_id(median_rec);

        /* 保存新旧分隔键信息，替换 cur_sep */
        uint8_t *new_sep = (uint8_t *)malloc(median_key_len);
        if (!new_sep)
        {
            rec_array_destroy(&ra2);
            err = BTREE_OUT_OF_MEMORY;
            goto out;
        }
        memcpy(new_sep, median_rec + 2, median_key_len);

        /* 重写左半 */
        page_id_t old_first_child = page_get_first_child(&parent_pg);
        page_id_t old_parent = page_get_parent(&parent_pg);

        page_init(&parent_pg, parent_pid, PAGE_INTERNAL);
        page_set_parent(&parent_pg, old_parent);
        page_set_first_child(&parent_pg, old_first_child);

        for (int i = 0; i < mid2; i++)
            page_alloc_slot(&parent_pg, ra2.recs[i], ra2.lens[i]);

        tree->write_page(tree->io_ctx, parent_pid, &parent_pg);

        /* 创建右半新内部节点 */
        page_id_t new_internal_pid;
        err = tree->alloc_page(tree->io_ctx, &new_internal_pid, PAGE_INTERNAL);
        if (err != BTREE_OK)
        {
            free(new_sep);
            rec_array_destroy(&ra2);
            goto out;
        }

        page_t new_internal_pg;
        tree->read_page(tree->io_ctx, new_internal_pid, &new_internal_pg);
        page_init(&new_internal_pg, new_internal_pid, PAGE_INTERNAL);
        page_set_parent(&new_internal_pg, old_parent);
        page_set_first_child(&new_internal_pg, median_child);
        set_parent_id(tree, median_child, new_internal_pid);

        for (int i = mid2 + 1; i < ra2.count; i++)
        {
            page_alloc_slot(&new_internal_pg, ra2.recs[i], ra2.lens[i]);
            page_id_t child = internal_child_id(ra2.recs[i]);
            set_parent_id(tree, child, new_internal_pid);
        }

        tree->write_page(tree->io_ctx, new_internal_pid, &new_internal_pg);

        rec_array_destroy(&ra2);

        /* 替换分隔键，进入下一轮 */
        free(cur_sep);
        cur_sep = new_sep;
        cur_sep_len = median_key_len;
        cur_pid = new_internal_pid;
        level--;
    }

out:
    free(cur_sep);
    return err;
}
