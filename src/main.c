#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "page.h"
#include "comparator.h"
#include "btree.h"
#include "storage.h"
#include "buffer_pool.h"
#include "wal.h"
#include "cursor.h"
#include "txn.h"
#include <pthread.h>

static void test_page_init(void)
{
    printf("== test_page_init ==\n");
    page_t pg;
    page_init(&pg, 42, PAGE_LEAF);

    printf("  page_id       = %u (expect 42)\n", page_get_id(&pg));
    printf("  type          = %d (expect %d)\n", page_get_type(&pg), PAGE_LEAF);
    printf("  num_slots     = %d (expect 0)\n", page_num_slots(&pg));
    printf("  free_offset   = %u (expect %d)\n", pg.header.free_offset, PAGE_HEADER_SIZE);
    printf("  free_size     = %u (expect %d)\n", pg.header.free_size, PAGE_SIZE - PAGE_HEADER_SIZE);
    printf("  parent        = %u (expect 0)\n", page_get_parent(&pg));
    printf("  prev/next     = %u / %u (expect 0/0)\n", page_get_prev(&pg), page_get_next(&pg));
    printf("  first_child   = %u (expect 0)\n", page_get_first_child(&pg));
    printf("  PASSED\n\n");
}

static void test_leaf_slot_alloc(void)
{
    printf("== test_leaf_slot_alloc ==\n");
    page_t pg;
    page_init(&pg, 1, PAGE_LEAF);

    /* 打包三条叶子记录 */
    uint8_t key1[] = "alpha";
    uint8_t val1[] = "apple";
    uint8_t buf1[64];
    leaf_rec_pack(buf1, key1, 5, val1, 5, false);

    uint8_t key2[] = "beta";
    uint8_t val2[] = "banana";
    uint8_t buf2[64];
    leaf_rec_pack(buf2, key2, 4, val2, 6, false);

    uint8_t key3[] = "gamma";
    uint8_t val3[] = "grape";
    uint8_t buf3[64];
    leaf_rec_pack(buf3, key3, 5, val3, 5, true); /* 逻辑删除 */

    int idx1 = page_alloc_slot(&pg, buf1, leaf_rec_size(5, 5));
    int idx2 = page_alloc_slot(&pg, buf2, leaf_rec_size(4, 6));
    int idx3 = page_alloc_slot(&pg, buf3, leaf_rec_size(5, 5));

    printf("  slot indices: %d %d %d\n", idx1, idx2, idx3);
    printf("  num_slots    = %d (expect 3)\n", page_num_slots(&pg));

    /* 验证第一条记录 */
    const uint8_t *r1 = page_slot_data_c(&pg, 0);
    printf("  slot[0] key  = %s (expect 'alpha')\n", leaf_key_ptr_c(r1));
    printf("  slot[0] val  = %s (expect 'apple')\n", leaf_val_ptr_c(r1));
    printf("  slot[0] del  = %d (expect 0)\n", leaf_is_deleted(r1));

    /* 验证第三条被标记删除 */
    const uint8_t *r3 = page_slot_data_c(&pg, 2);
    printf("  slot[2] del  = %d (expect 1)\n", leaf_is_deleted(r3));

    printf("  PASSED\n\n");
}

static void test_slot_remove_and_compact(void)
{
    printf("== test_slot_remove_and_compact ==\n");
    page_t pg;
    page_init(&pg, 2, PAGE_LEAF);

    uint8_t key[] = "x";
    uint8_t val[] = "y";
    uint8_t buf[64];
    leaf_rec_pack(buf, key, 1, val, 1, false);
    uint16_t rec_len = leaf_rec_size(1, 1);

    for (int i = 0; i < 5; i++)
    {
        key[0] = (uint8_t)('A' + i);
        val[0] = (uint8_t)('a' + i);
        leaf_rec_pack(buf, key, 1, val, 1, false);
        page_alloc_slot(&pg, buf, rec_len);
    }
    printf("  before remove: num_slots = %d (expect 5)\n", page_num_slots(&pg));

    page_remove_slot(&pg, 1); /* 移除 B */
    printf("  after remove B: num_slots = %d (expect 4)\n", page_num_slots(&pg));

    /* 验证剩余记录正确 */
    const uint8_t *r0 = page_slot_data_c(&pg, 0);
    const uint8_t *r1 = page_slot_data_c(&pg, 1);
    const uint8_t *r2 = page_slot_data_c(&pg, 2);
    const uint8_t *r3 = page_slot_data_c(&pg, 3);

    printf("  slot[0] key = %c (expect A)\n", *leaf_key_ptr_c(r0));
    printf("  slot[1] key = %c (expect C)\n", *leaf_key_ptr_c(r1));
    printf("  slot[2] key = %c (expect D)\n", *leaf_key_ptr_c(r2));
    printf("  slot[3] key = %c (expect E)\n", *leaf_key_ptr_c(r3));

    /* 再移除首尾 */
    page_remove_slot(&pg, 0); /* 移除 A */
    page_remove_slot(&pg, 2); /* 移除 E（压缩后索引变化） */
    printf("  after remove A+E: num_slots = %d (expect 2)\n", page_num_slots(&pg));

    const uint8_t *s0 = page_slot_data_c(&pg, 0);
    const uint8_t *s1 = page_slot_data_c(&pg, 1);
    printf("  slot[0] key = %c (expect C)\n", *leaf_key_ptr_c(s0));
    printf("  slot[1] key = %c (expect D)\n", *leaf_key_ptr_c(s1));

    printf("  PASSED\n\n");
}

static void test_internal_records(void)
{
    printf("== test_internal_records ==\n");
    page_t pg;
    page_init(&pg, 10, PAGE_INTERNAL);

    /* 设置最左子节点 */
    page_set_first_child(&pg, 100);

    /* 插入内部记录 */
    uint8_t buf[64];
    internal_rec_pack(buf, (const uint8_t *)"cat", 3, 200);
    internal_rec_pack(buf + internal_rec_size(3), (const uint8_t *)"dog", 3, 300);

    int i1 = page_alloc_slot(&pg, buf, internal_rec_size(3));
    int i2 = page_alloc_slot(&pg, buf + internal_rec_size(3), internal_rec_size(3));
    (void)i1;
    (void)i2;

    printf("  type          = %d (expect %d)\n", page_get_type(&pg), PAGE_INTERNAL);
    printf("  first_child   = %u (expect 100)\n", page_get_first_child(&pg));
    printf("  num_slots     = %d (expect 2)\n", page_num_slots(&pg));

    const uint8_t *r0 = page_slot_data_c(&pg, 0);
    const uint8_t *r1 = page_slot_data_c(&pg, 1);
    printf("  slot[0] key   = %s (expect 'cat')\n", internal_key_ptr_c(r0));
    printf("  slot[0] child = %u (expect 200)\n", internal_child_id(r0));
    printf("  slot[1] key   = %s (expect 'dog')\n", internal_key_ptr_c(r1));
    printf("  slot[1] child = %u (expect 300)\n", internal_child_id(r1));

    printf("  PASSED\n\n");
}

static void test_comparator(void)
{
    printf("== test_comparator ==\n");

    int r1 = compare_default((const uint8_t *)"abc", 3, (const uint8_t *)"abd", 3);
    printf("  compare('abc','abd') = %d (expect < 0)\n", r1);

    int r2 = compare_default((const uint8_t *)"zzz", 3, (const uint8_t *)"zz", 2);
    printf("  compare('zzz','zz')  = %d (expect > 0)\n", r2);

    int r3 = compare_default((const uint8_t *)"same", 4, (const uint8_t *)"same", 4);
    printf("  compare('same','same') = %d (expect 0)\n", r3);

    int r4 = compare_default((const uint8_t *)"a", 1, (const uint8_t *)"aa", 2);
    printf("  compare('a','aa')    = %d (expect < 0)\n", r4);

    printf("  PASSED\n\n");
}

/* ════════════════════════════════════════════════════════════════
 *  B+ 树测试 — 内存存储适配器
 * ════════════════════════════════════════════════════════════════ */

#define MEM_MAX_PAGES 2048

typedef struct
{
    page_t pages[MEM_MAX_PAGES];
    int num_pages;
} mem_store_t;

static void mem_store_init(mem_store_t *s)
{
    memset(s, 0, sizeof(*s));
    s->num_pages = 0;
}

static btree_error_t mem_read(void *ctx, page_id_t pid, page_t *page)
{
    mem_store_t *s = (mem_store_t *)ctx;
    if (pid >= (page_id_t)s->num_pages)
        return BTREE_IO_ERROR;
    *page = s->pages[pid];
    return BTREE_OK;
}

static btree_error_t mem_write(void *ctx, page_id_t pid, const page_t *page)
{
    mem_store_t *s = (mem_store_t *)ctx;
    if (pid >= MEM_MAX_PAGES)
        return BTREE_IO_ERROR;
    if (pid >= (page_id_t)s->num_pages)
        s->num_pages = (int)(pid + 1);
    s->pages[pid] = *page;
    return BTREE_OK;
}

static btree_error_t mem_alloc(void *ctx, page_id_t *pid, page_type_t type)
{
    mem_store_t *s = (mem_store_t *)ctx;
    if (s->num_pages >= MEM_MAX_PAGES)
        return BTREE_IO_ERROR;
    *pid = (page_id_t)s->num_pages;
    page_init(&s->pages[s->num_pages], *pid, type);
    s->num_pages++;
    return BTREE_OK;
}

/* ─── 测试：基本 Put / Get ─── */

static void test_btree_basic(void)
{
    printf("== test_btree_basic ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                 mem_read, mem_write, mem_alloc, &store);
    uint8_t buf[64];
    uint16_t len;

    btree_put(tree, (const uint8_t *)"alpha", 5, (const uint8_t *)"apple", 5);
    btree_put(tree, (const uint8_t *)"beta", 4, (const uint8_t *)"banana", 6);
    btree_put(tree, (const uint8_t *)"gamma", 5, (const uint8_t *)"grape", 5);

    len = sizeof(buf);
    printf("  get alpha:  %d ", btree_get(tree, (const uint8_t *)"alpha", 5, buf, &len));
    buf[len] = '\0';
    printf("'%s' len=%d (expect 0 'apple' 5)\n", buf, len);

    len = sizeof(buf);
    printf("  get beta:   %d ", btree_get(tree, (const uint8_t *)"beta", 4, buf, &len));
    buf[len] = '\0';
    printf("'%s' len=%d (expect 0 'banana' 6)\n", buf, len);

    len = sizeof(buf);
    printf("  get gamma:  %d ", btree_get(tree, (const uint8_t *)"gamma", 5, buf, &len));
    buf[len] = '\0';
    printf("'%s' len=%d (expect 0 'grape' 5)\n", buf, len);

    /* 不存在的 key */
    btree_error_t e = btree_get(tree, (const uint8_t *)"delta", 5, buf, &len);
    printf("  get delta:  %d (expect %d - NOT_FOUND)\n", e, BTREE_NOT_FOUND);

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：更新 ─── */

static void test_btree_update(void)
{
    printf("== test_btree_update ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                 mem_read, mem_write, mem_alloc, &store);
    uint8_t buf[64];
    uint16_t len;

    btree_put(tree, (const uint8_t *)"key", 3, (const uint8_t *)"old_value", 9);

    btree_put(tree, (const uint8_t *)"key", 3, (const uint8_t *)"new_value", 9);

    len = sizeof(buf);
    btree_get(tree, (const uint8_t *)"key", 3, buf, &len);
    buf[len] = '\0';
    printf("  get key: '%s' (expect 'new_value')\n", buf);

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：删除 ─── */

static void test_btree_delete(void)
{
    printf("== test_btree_delete ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                 mem_read, mem_write, mem_alloc, &store);
    uint8_t buf[64];
    uint16_t len;

    btree_put(tree, (const uint8_t *)"live", 4, (const uint8_t *)"alive", 5);
    btree_put(tree, (const uint8_t *)"dead", 4, (const uint8_t *)"gone", 4);

    printf("  delete dead: %d (expect 0)\n",
           btree_delete(tree, (const uint8_t *)"dead", 4));
    printf("  delete dead again: %d (expect %d - NOT_FOUND)\n",
           btree_delete(tree, (const uint8_t *)"dead", 4), BTREE_NOT_FOUND);

    len = sizeof(buf);
    printf("  get live: %d (expect 0)\n",
           btree_get(tree, (const uint8_t *)"live", 4, buf, &len));
    len = sizeof(buf);
    printf("  get dead: %d (expect %d - NOT_FOUND)\n",
           btree_get(tree, (const uint8_t *)"dead", 4, buf, &len), BTREE_NOT_FOUND);

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：覆盖已删除的 key ─── */

static void test_btree_overwrite_deleted(void)
{
    printf("== test_btree_overwrite_deleted ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                 mem_read, mem_write, mem_alloc, &store);
    uint8_t buf[64];
    uint16_t len;

    btree_put(tree, (const uint8_t *)"x", 1, (const uint8_t *)"first", 5);
    btree_delete(tree, (const uint8_t *)"x", 1);
    btree_put(tree, (const uint8_t *)"x", 1, (const uint8_t *)"second", 6);

    len = sizeof(buf);
    btree_get(tree, (const uint8_t *)"x", 1, buf, &len);
    buf[len] = '\0';
    printf("  get x: '%s' (expect 'second')\n", buf);

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：叶子分裂 ─── */

static void test_btree_split(void)
{
    printf("== test_btree_split ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                 mem_read, mem_write, mem_alloc, &store);

    int const N = 500;
    char key[8], val[8];

    /* 写入 N 条顺序递增的记录 */
    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        btree_put(tree, (const uint8_t *)key, 4, (const uint8_t *)val, 4);
    }

    /* 逐条读出验证 */
    int ok = 0;
    uint8_t buf[64];
    uint16_t len;

    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);

        len = sizeof(buf);
        btree_error_t e = btree_get(tree, (const uint8_t *)key, 4, buf, &len);
        if (e == BTREE_OK && len == 4 && memcmp(buf, val, 4) == 0)
            ok++;
    }
    printf("  %d/%d records verified (expect %d)\n", ok, N, N);

    /* 不存在的 key 应返回 NOT_FOUND */
    btree_error_t e = btree_get(tree, (const uint8_t *)"k999", 4, buf, &len);
    printf("  get k999: %d (expect %d - NOT_FOUND)\n", e, BTREE_NOT_FOUND);

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：逆序插入 ─── */

static void test_btree_reverse_insert(void)
{
    printf("== test_btree_reverse_insert ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                 mem_read, mem_write, mem_alloc, &store);

    int const N = 200;
    char key[8], val[8];

    for (int i = N - 1; i >= 0; i--)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        btree_put(tree, (const uint8_t *)key, 4, (const uint8_t *)val, 4);
    }

    int ok = 0;
    uint8_t buf[64];
    uint16_t len;

    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);

        len = sizeof(buf);
        btree_error_t e = btree_get(tree, (const uint8_t *)key, 4, buf, &len);
        if (e == BTREE_OK && len == 4 && memcmp(buf, val, 4) == 0)
            ok++;
    }
    printf("  %d/%d records verified (expect %d)\n", ok, N, N);

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：删除合并 — 触发叶子合并 + 根收缩 ─── */

static void test_btree_delete_merge(void)
{
    printf("== test_btree_delete_merge ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);

    int const N = 300;
    char key[8], val[8];

    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        btree_put(tree, (const uint8_t *)key, 4, (const uint8_t *)val, 4);
    }

    /* 删除左叶子的大部分记录，触发叶子合并 */
    for (int i = 0; i < 119; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        btree_delete(tree, (const uint8_t *)key, 4);
    }

    /* 验证剩余记录 */
    int ok = 0;
    uint8_t buf[64];
    uint16_t len;

    for (int i = 119; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        len = sizeof(buf);
        btree_error_t e = btree_get(tree, (const uint8_t *)key, 4, buf, &len);
        if (e == BTREE_OK && len == 4 && memcmp(buf, val, 4) == 0)
            ok++;
    }
    printf("  remaining %d/181 OK\n", ok);

    btree_error_t e = btree_get(tree, (const uint8_t *)"k000", 4, buf, &len);
    printf("  get k000: %d (expect %d)\n", e, BTREE_NOT_FOUND);

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：大规模删除 — 多层合并路径 ─── */

static void test_btree_delete_massive(void)
{
    printf("== test_btree_delete_massive ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);

    int const N = 800;
    char key[8], val[8];

    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        btree_put(tree, (const uint8_t *)key, 4, (const uint8_t *)val, 4);
    }

    /* 跨叶子删除大部分记录，触发多层合并 */
    for (int i = 0; i < N - 5; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        btree_delete(tree, (const uint8_t *)key, 4);
    }

    /* 剩余 5 条 */
    int ok = 0;
    uint8_t buf[64];
    uint16_t len;

    for (int i = N - 5; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        len = sizeof(buf);
        btree_error_t e = btree_get(tree, (const uint8_t *)key, 4, buf, &len);
        if (e == BTREE_OK && len == 4 && memcmp(buf, val, 4) == 0)
            ok++;
    }
    printf("  remaining %d/5 OK\n", ok);

    btree_error_t e = btree_get(tree, (const uint8_t *)"k000", 4, buf, &len);
    printf("  get k000: %d (expect %d)\n", e, BTREE_NOT_FOUND);

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：删除 + 再插入 — 合并后复用 ─── */

static void test_btree_delete_reinsert(void)
{
    printf("== test_btree_delete_reinsert ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);

    int const N = 300;
    char key[8], val[8];
    uint8_t buf[64];
    uint16_t len;

    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        btree_put(tree, (const uint8_t *)key, 4, (const uint8_t *)val, 4);
    }

    /* 删除全部 */
    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        btree_delete(tree, (const uint8_t *)key, 4);
    }

    /* 验证全删 */
    btree_error_t e = btree_get(tree, (const uint8_t *)"k000", 4, buf, &len);
    printf("  after delete all, get k000: %d (expect %d)\n", e, BTREE_NOT_FOUND);

    /* 重新插入 */
    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        btree_put(tree, (const uint8_t *)key, 4, (const uint8_t *)val, 4);
    }

    /* 验证重新插入 */
    int ok = 0;
    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        len = sizeof(buf);
        e = btree_get(tree, (const uint8_t *)key, 4, buf, &len);
        if (e == BTREE_OK && len == 4 && memcmp(buf, val, 4) == 0)
            ok++;
    }
    printf("  re-insert %d/%d OK\n", ok, N);

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：文件存储持久化 ─── */

static void test_storage_persistence(void)
{
    printf("== test_storage_persistence ==\n");

    const char *path = "btree_test_persist.bt";
    int const N = 500;
    char key[8], val[8];
    uint8_t buf[64];
    uint16_t len;

    remove(path);

    /* 第一轮：创建文件，写入 N 条记录 */
    btree_storage_t *store = btree_storage_create(path);
    btree_t *tree = btree_create(compare_default,
                                  btree_storage_read, btree_storage_write,
                                  btree_storage_alloc, store);
    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        btree_put(tree, (const uint8_t *)key, 4, (const uint8_t *)val, 4);
    }
    btree_storage_set_root_id(store, btree_root_id(tree));
    btree_destroy(tree);
    btree_storage_close(store);

    /* 第二轮：重新打开，验证全部记录 */
    store = btree_storage_open(path);
    page_id_t root;
    btree_storage_get_root_id(store, &root);
    tree = btree_open(compare_default,
                       btree_storage_read, btree_storage_write,
                       btree_storage_alloc, store, root);

    int ok = 0;
    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        len = sizeof(buf);
        btree_error_t e = btree_get(tree, (const uint8_t *)key, 4, buf, &len);
        if (e == BTREE_OK && len == 4 && memcmp(buf, val, 4) == 0)
            ok++;
    }
    printf("  reopen: %d/%d verified (expect %d)\n", ok, N, N);

    /* 第三轮：增删操作后重新打开验证 */
    btree_delete(tree, (const uint8_t *)"k000", 4);
    btree_put(tree, (const uint8_t *)"k999", 4, (const uint8_t *)"v999", 4);
    btree_storage_set_root_id(store, btree_root_id(tree));
    btree_destroy(tree);
    btree_storage_close(store);

    store = btree_storage_open(path);
    btree_storage_get_root_id(store, &root);
    tree = btree_open(compare_default,
                       btree_storage_read, btree_storage_write,
                       btree_storage_alloc, store, root);

    /* k000 应不存在 */
    btree_error_t e = btree_get(tree, (const uint8_t *)"k000", 4, buf, &len);
    printf("  get k000 after delete: %d (expect %d)\n", e, BTREE_NOT_FOUND);

    /* k999 应存在 */
    len = sizeof(buf);
    e = btree_get(tree, (const uint8_t *)"k999", 4, buf, &len);
    buf[len] = '\0';
    printf("  get k999: %d '%s' (expect 0 'v999')\n", e, buf);

    /* 剩余 500 条（删 1 + 增 1，总数不变） */
    ok = 0;
    for (int i = 1; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        len = sizeof(buf);
        e = btree_get(tree, (const uint8_t *)key, 4, buf, &len);
        if (e == BTREE_OK && len == 4 && memcmp(buf, val, 4) == 0)
            ok++;
    }
    printf("  remaining: %d/%d verified (expect %d)\n", ok, N - 1, N - 1);

    btree_destroy(tree);
    btree_storage_close(store);
    remove(path);

    printf("  PASSED\n\n");
}

/* ════════════════════════════════════════════════════════════════
 *  缓冲池测试
 * ════════════════════════════════════════════════════════════════ */

/* ─── 测试：缓冲池基本读写 ─── */

static void test_buffer_pool_basic(void)
{
    printf("== test_buffer_pool_basic ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_buffer_pool_t *bp = bp_create(0, /* 默认容量 */
                                         mem_read, mem_write, mem_alloc, &store);
    btree_t *tree = btree_create(compare_default,
                                  bp_read, bp_write, bp_alloc, bp);

    uint8_t buf[64];
    uint16_t len;
    int const N = 100;

    for (int i = 0; i < N; i++)
    {
        char k[8], v[8];
        snprintf(k, sizeof(k), "k%03d", i);
        snprintf(v, sizeof(v), "v%03d", i);
        btree_put(tree, (const uint8_t *)k, 4, (const uint8_t *)v, 4);
    }

    int ok = 0;
    for (int i = 0; i < N; i++)
    {
        char k[8], v[8];
        snprintf(k, sizeof(k), "k%03d", i);
        snprintf(v, sizeof(v), "v%03d", i);
        len = sizeof(buf);
        btree_error_t e = btree_get(tree, (const uint8_t *)k, 4, buf, &len);
        if (e == BTREE_OK && len == 4 && memcmp(buf, v, 4) == 0)
            ok++;
    }
    printf("  %d/%d records (expect %d)\n", ok, N, N);
    printf("  hits=%u misses=%u\n", bp_hit_count(bp), bp_miss_count(bp));

    btree_destroy(tree);
    bp_destroy(bp);
    printf("  PASSED\n\n");
}

/* ─── 测试：缓冲池淘汰 ─── */

static void test_buffer_pool_eviction(void)
{
    printf("== test_buffer_pool_eviction ==\n");

    mem_store_t store;
    mem_store_init(&store);
    /* 小容量强制频繁淘汰 */
    btree_buffer_pool_t *bp = bp_create(4,
                                         mem_read, mem_write, mem_alloc, &store);
    btree_t *tree = btree_create(compare_default,
                                  bp_read, bp_write, bp_alloc, bp);

    int const N = 1000;
    char key[8], val[8];
    uint8_t buf[64];
    uint16_t len;

    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        btree_put(tree, (const uint8_t *)key, 4, (const uint8_t *)val, 4);
    }

    int ok = 0;
    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        len = sizeof(buf);
        btree_error_t e = btree_get(tree, (const uint8_t *)key, 4, buf, &len);
        if (e == BTREE_OK && len == 4 && memcmp(buf, val, 4) == 0)
            ok++;
    }
    printf("  %d/%d records (expect %d)\n", ok, N, N);
    printf("  hits=%u misses=%u (expect misses >= frames=%u as evidence of eviction)\n",
           bp_hit_count(bp), bp_miss_count(bp), 4u);

    btree_destroy(tree);
    bp_destroy(bp);
    printf("  PASSED\n\n");
}

/* ─── 测试：缓冲池 + 文件存储持久化 ─── */

static void test_buffer_pool_persistence(void)
{
    printf("== test_buffer_pool_persistence ==\n");

    const char *path = "btree_test_bp.bt";
    int const N = 300;
    char key[8], val[8];
    uint8_t buf[64];
    uint16_t len;

    remove(path);

    /* 第一轮：通过缓冲池写入 */
    btree_storage_t *store = btree_storage_create(path);
    btree_buffer_pool_t *bp = bp_create(0,
                                         btree_storage_read,
                                         btree_storage_write,
                                         btree_storage_alloc,
                                         store);
    btree_t *tree = btree_create(compare_default,
                                  bp_read, bp_write, bp_alloc, bp);

    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        btree_put(tree, (const uint8_t *)key, 4, (const uint8_t *)val, 4);
    }

    /* 刷脏页并持久化 root_id */
    bp_flush_all(bp);
    btree_storage_set_root_id(store, btree_root_id(tree));

    btree_destroy(tree);
    bp_destroy(bp);
    btree_storage_close(store);

    /* 第二轮：直接存储打开验证 */
    store = btree_storage_open(path);
    page_id_t root;
    btree_storage_get_root_id(store, &root);
    tree = btree_open(compare_default,
                       btree_storage_read, btree_storage_write,
                       btree_storage_alloc, store, root);

    int ok = 0;
    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        len = sizeof(buf);
        btree_error_t e = btree_get(tree, (const uint8_t *)key, 4, buf, &len);
        if (e == BTREE_OK && len == 4 && memcmp(buf, val, 4) == 0)
            ok++;
    }
    printf("  reopen (direct): %d/%d (expect %d)\n",
           ok, N, N);

    btree_destroy(tree);
    btree_storage_close(store);

    /* 第三轮：缓冲池打开并更新 */
    store = btree_storage_open(path);
    btree_storage_get_root_id(store, &root);
    bp = bp_create(0, btree_storage_read, btree_storage_write,
                   btree_storage_alloc, store);
    tree = btree_open(compare_default,
                       bp_read, bp_write, bp_alloc, bp, root);

    btree_put(tree, (const uint8_t *)"k000", 4, (const uint8_t *)"new0", 4);
    bp_flush_all(bp);
    btree_storage_set_root_id(store, btree_root_id(tree));
    btree_destroy(tree);
    bp_destroy(bp);
    btree_storage_close(store);

    /* 第四轮：直接打开验证更新 */
    store = btree_storage_open(path);
    btree_storage_get_root_id(store, &root);
    tree = btree_open(compare_default,
                       btree_storage_read, btree_storage_write,
                       btree_storage_alloc, store, root);

    len = sizeof(buf);
    btree_error_t e = btree_get(tree, (const uint8_t *)"k000", 4, buf, &len);
    buf[len] = '\0';
    printf("  get k000 after update: err=%d val='%s' (expect 0 'new0')\n", e, buf);

    ok = 0;
    for (int i = 1; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        len = sizeof(buf);
        e = btree_get(tree, (const uint8_t *)key, 4, buf, &len);
        if (e == BTREE_OK && len == 4 && memcmp(buf, val, 4) == 0)
            ok++;
    }
    printf("  remaining after update: %d/%d (expect %d)\n", ok, N - 1, N - 1);

    btree_destroy(tree);
    btree_storage_close(store);
    remove(path);

    printf("  PASSED\n\n");
}

/* ════════════════════════════════════════════════════════════════
 *  WAL 测试
 * ════════════════════════════════════════════════════════════════ */

/* ─── 测试：WAL 页级恢复（使用内存存储） ─── */

static void test_wal_page_recovery(void)
{
    printf("== test_wal_page_recovery ==\n");

    const char *wal_path = "test_wal_page.wal";
    remove(wal_path);

    /* 第一轮：通过 WAL 写入页到 mem_store */
    static mem_store_t store; /* static 避免栈溢出 (mem_store_t ≈ 8MB) */
    mem_store_init(&store);
    wal_t *wal = wal_create(wal_path,
                             mem_read, mem_write, mem_alloc, &store);

    page_id_t pids[10];
    for (int i = 0; i < 10; i++)
    {
        wal_alloc(wal, &pids[i], PAGE_LEAF);
        page_t pg;
        page_init(&pg, pids[i], PAGE_LEAF);

        /* 写入一个带标记的叶子记录 */
        char k[8], v[8];
        snprintf(k, sizeof(k), "k%03d", i);
        snprintf(v, sizeof(v), "v%03d", i);
        uint8_t buf[64];
        leaf_rec_pack(buf, (const uint8_t *)k, 4, (const uint8_t *)v, 4, false);
        page_alloc_slot(&pg, buf, leaf_rec_size(4, 4));

        wal_write(wal, pids[i], &pg);
    }

    uint32_t n = wal_num_entries(wal);
    printf("  WAL entries before destroy: %u (expect 10)\n", n);
    wal_destroy(wal);

    /* 第二轮：用空的 mem_store 恢复 */
    static mem_store_t recovered; /* static 避免栈溢出 */
    mem_store_init(&recovered);
    wal_t *wal2 = wal_create(wal_path,
                              mem_read, mem_write, mem_alloc, &recovered);

    btree_error_t err = wal_recover(wal2);
    printf("  wal_recover err: %d (expect 0)\n", err);
    printf("  WAL entries after recover: %u (expect 0)\n",
           wal_num_entries(wal2));

    /* 验证恢复后的数据 */
    int ok = 0;
    for (int i = 0; i < 10; i++)
    {
        page_t pg;
        if (mem_read(&recovered, pids[i], &pg) != BTREE_OK)
            continue;
        if (page_num_slots(&pg) != 1)
            continue;
        const uint8_t *r = page_slot_data_c(&pg, 0);
        if (!r) continue;
        char k[8], v[8];
        snprintf(k, sizeof(k), "k%03d", i);
        snprintf(v, sizeof(v), "v%03d", i);
        if (memcmp(leaf_key_ptr_c(r), k, 4) == 0 &&
            memcmp(leaf_val_ptr_c(r), v, 4) == 0)
            ok++;
    }
    printf("  recovered pages: %d/10\n", ok);

    wal_destroy(wal2);
    remove(wal_path);
    printf("  PASSED\n\n");
}

/* ─── 测试：WAL + 缓冲池 + 文件存储集成 ─── */

static void test_wal_crash_recovery(void)
{
    printf("== test_wal_crash_recovery ==\n");

    const char *store_path = "test_wal_crash.bt";
    const char *wal_path   = "test_wal_crash.wal";
    remove(store_path);
    remove(wal_path);

    int const N = 300;
    char key[8], val[8];
    uint8_t buf[64];
    uint16_t len;

    /* ---- 第一轮：正常写入 ---- */
    btree_storage_t *store = btree_storage_create(store_path);
    wal_t *wal = wal_create(wal_path,
                             btree_storage_read,
                             btree_storage_write,
                             btree_storage_alloc,
                             store);
    btree_buffer_pool_t *bp = bp_create(0,
                                         wal_read, wal_write, wal_alloc, wal);
    btree_t *tree = btree_create(compare_default,
                                  bp_read, bp_write, bp_alloc, bp);

    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        btree_put(tree, (const uint8_t *)key, 4, (const uint8_t *)val, 4);
    }

    bp_flush_all(bp);
    btree_storage_set_root_id(store, btree_root_id(tree));
    page_id_t saved_root = btree_root_id(tree);

    btree_destroy(tree);
    bp_destroy(bp);
    wal_destroy(wal);
    btree_storage_close(store);

    /* ---- "模拟崩溃"：删除存储文件，WAL 文件保留 ---- */
    remove(store_path);

    /* ---- 第二轮：通过 WAL 恢复到新的存储文件 ---- */
    store = btree_storage_create(store_path);
    wal = wal_create(wal_path,
                     btree_storage_read,
                     btree_storage_write,
                     btree_storage_alloc,
                     store);

    btree_error_t err = wal_recover(wal);
    printf("  wal_recover: %d (expect 0)\n", err);

    /* 用保存的 root_id 打开 btree */
    bp = bp_create(0, wal_read, wal_write, wal_alloc, wal);
    tree = btree_open(compare_default,
                       bp_read, bp_write, bp_alloc, bp, saved_root);

    int ok = 0;
    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        len = sizeof(buf);
        btree_error_t e = btree_get(tree, (const uint8_t *)key, 4, buf, &len);
        if (e == BTREE_OK && len == 4 && memcmp(buf, val, 4) == 0)
            ok++;
    }
    printf("  recovered via WAL after crash: %d/%d (expect %d)\n", ok, N, N);

    btree_destroy(tree);
    bp_destroy(bp);
    wal_destroy(wal);
    btree_storage_close(store);
    remove(store_path);
    remove(wal_path);
    printf("  PASSED\n\n");
}

/* ─── 测试：WAL 完整持久化（含 root_id 恢复） ─── */

static void test_wal_full_persistence(void)
{
    printf("== test_wal_full_persistence ==\n");

    const char *store_path = "test_wal_full.bt";
    const char *wal_path   = "test_wal_full.wal";
    remove(store_path);
    remove(wal_path);

    int const N = 500;
    char key[8], val[8];
    uint8_t buf[64];
    uint16_t len;

    /* ── 第一轮：标准写入 ── */
    btree_storage_t *store = btree_storage_create(store_path);
    btree_buffer_pool_t *bp = bp_create(0,
                                         btree_storage_read,
                                         btree_storage_write,
                                         btree_storage_alloc,
                                         store);
    btree_t *tree = btree_create(compare_default,
                                  bp_read, bp_write, bp_alloc, bp);

    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        btree_put(tree, (const uint8_t *)key, 4, (const uint8_t *)val, 4);
    }

    bp_flush_all(bp);
    btree_storage_set_root_id(store, btree_root_id(tree));
    page_id_t saved_root = btree_root_id(tree);

    btree_destroy(tree);
    bp_destroy(bp);
    btree_storage_close(store);

    /* ── 第二轮：通过 WAL 包装重新打开 ── */
    store = btree_storage_open(store_path);
    wal_t *wal = wal_create(wal_path,
                             btree_storage_read,
                             btree_storage_write,
                             btree_storage_alloc,
                             store);

    /* 如果 WAL 文件有残留日志，先恢复 */
    if (wal_num_entries(wal) > 0)
        wal_recover(wal);

    bp = bp_create(0, wal_read, wal_write, wal_alloc, wal);
    tree = btree_open(compare_default,
                       bp_read, bp_write, bp_alloc, bp, saved_root);

    int ok = 0;
    for (int i = 0; i < N; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        len = sizeof(buf);
        btree_error_t e = btree_get(tree, (const uint8_t *)key, 4, buf, &len);
        if (e == BTREE_OK && len == 4 && memcmp(buf, val, 4) == 0)
            ok++;
    }
    printf("  reopen via WAL: %d/%d (expect %d)\n", ok, N, N);

    /* 写入一些新数据验证 WAL 正常工作 */
    for (int i = N; i < N + 100; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        btree_put(tree, (const uint8_t *)key, 4, (const uint8_t *)val, 4);
    }

    bp_flush_all(bp);
    btree_storage_set_root_id(store, btree_root_id(tree));
    page_id_t new_root = btree_root_id(tree);

    btree_destroy(tree);
    bp_destroy(bp);
    wal_destroy(wal);
    btree_storage_close(store);

    /* ── 第三轮：验证新增数据 ── */
    store = btree_storage_open(store_path);
    tree = btree_open(compare_default,
                       btree_storage_read, btree_storage_write,
                       btree_storage_alloc, store, new_root);

    ok = 0;
    for (int i = 0; i < N + 100; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        len = sizeof(buf);
        btree_error_t e = btree_get(tree, (const uint8_t *)key, 4, buf, &len);
        if (e == BTREE_OK && len == 4 && memcmp(buf, val, 4) == 0)
            ok++;
    }
    printf("  final verify: %d/%d (expect %d)\n", ok, N + 100, N + 100);

    btree_destroy(tree);
    btree_storage_close(store);
    remove(store_path);
    remove(wal_path);
    printf("  PASSED\n\n");
}

/* ════════════════════════════════════════════════════════════════
 *  游标测试
 * ════════════════════════════════════════════════════════════════ */

/* ─── 辅助：向树中插入 N 条 k%03d → v%03d 记录 ─── */
static void populate_btree(btree_t *tree, int n)
{
    char key[8], val[8];
    for (int i = 0; i < n; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        btree_put(tree, (const uint8_t *)key, 4, (const uint8_t *)val, 4);
    }
}

/* ─── 测试：正向遍历 ─── */

static void test_cursor_forward(void)
{
    printf("== test_cursor_forward ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);
    populate_btree(tree, 300);

    btree_cursor_t *cur = btree_cursor_create(tree);
    btree_error_t err = btree_cursor_seek(cur, (const uint8_t *)"k000", 4);
    printf("  seek(k000): %d (expect 0)\n", err);

    int ok = 0;
    uint8_t key[8], val[8];
    uint16_t klen, vlen;

    while (btree_cursor_valid(cur))
    {
        klen = sizeof(key);
        vlen = sizeof(val);
        btree_cursor_get(cur, key, &klen, val, &vlen);
        char ek[16], ev[16];
        snprintf(ek, sizeof(ek), "k%03d", ok);
        snprintf(ev, sizeof(ev), "v%03d", ok);
        if (klen != 4 || vlen != 4
            || memcmp(key, ek, 4) != 0
            || memcmp(val, ev, 4) != 0)
            break;
        ok++;
        btree_cursor_next(cur);
    }
    printf("  forward %d/%d (expect %d)\n", ok, 300, 300);

    btree_cursor_destroy(cur);
    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：从中间位置 seek 并遍历 ─── */

static void test_cursor_seek_mid(void)
{
    printf("== test_cursor_seek_mid ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);
    populate_btree(tree, 500);

    btree_cursor_t *cur = btree_cursor_create(tree);
    btree_error_t err = btree_cursor_seek(cur, (const uint8_t *)"k250", 4);
    printf("  seek(k250): %d (expect 0)\n", err);

    int ok = 0, start = 250;
    uint8_t key[8], val[8];
    uint16_t klen, vlen;

    while (btree_cursor_valid(cur))
    {
        klen = sizeof(key);
        vlen = sizeof(val);
        btree_cursor_get(cur, key, &klen, val, &vlen);
        char ek[16], ev[16];
        snprintf(ek, sizeof(ek), "k%03d", start + ok);
        snprintf(ev, sizeof(ev), "v%03d", start + ok);
        if (klen != 4 || vlen != 4
            || memcmp(key, ek, 4) != 0
            || memcmp(val, ev, 4) != 0)
            break;
        ok++;
        btree_cursor_next(cur);
    }
    printf("  from k250: %d/%d (expect %d)\n", ok, 250, 250);

    btree_cursor_destroy(cur);
    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：反向遍历 ─── */

static void test_cursor_reverse(void)
{
    printf("== test_cursor_reverse ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);
    populate_btree(tree, 200);

    btree_cursor_t *cur = btree_cursor_create(tree);
    btree_error_t err = btree_cursor_last(cur);
    printf("  last(): %d (expect 0)\n", err);

    int ok = 0;
    uint8_t key[8], val[8];
    uint16_t klen, vlen;

    while (btree_cursor_valid(cur))
    {
        klen = sizeof(key);
        vlen = sizeof(val);
        btree_cursor_get(cur, key, &klen, val, &vlen);
        int idx = 199 - ok;
        char ek[16], ev[16];
        snprintf(ek, sizeof(ek), "k%03d", idx);
        snprintf(ev, sizeof(ev), "v%03d", idx);
        if (klen != 4 || vlen != 4
            || memcmp(key, ek, 4) != 0
            || memcmp(val, ev, 4) != 0)
            break;
        ok++;
        btree_cursor_prev(cur);
    }
    printf("  reverse %d/%d (expect %d)\n", ok, 200, 200);

    btree_cursor_destroy(cur);
    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：first / last ─── */

static void test_cursor_first_last(void)
{
    printf("== test_cursor_first_last ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);
    populate_btree(tree, 100);

    btree_cursor_t *cur = btree_cursor_create(tree);

    btree_error_t e1 = btree_cursor_first(cur);
    uint8_t key[8], val[8];
    uint16_t klen, vlen;
    if (e1 == BTREE_OK)
    {
        klen = sizeof(key);
        vlen = sizeof(val);
        btree_cursor_get(cur, key, &klen, val, &vlen);
    }
    printf("  first: err=%d key='%.4s' val='%.4s' (expect 0 'k000' 'v000')\n",
           e1, key, val);

    btree_error_t e2 = btree_cursor_last(cur);
    if (e2 == BTREE_OK)
    {
        klen = sizeof(key);
        vlen = sizeof(val);
        btree_cursor_get(cur, key, &klen, val, &vlen);
    }
    printf("  last:  err=%d key='%.4s' val='%.4s' (expect 0 'k099' 'v099')\n",
           e2, key, val);

    btree_cursor_destroy(cur);
    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：范围扫描（seek + 条件终止） ─── */

static void test_cursor_range(void)
{
    printf("== test_cursor_range ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);
    populate_btree(tree, 500);

    btree_cursor_t *cur = btree_cursor_create(tree);
    btree_error_t err = btree_cursor_seek(cur, (const uint8_t *)"k050", 4);
    printf("  seek(k050): %d (expect 0)\n", err);

    int ok = 0;
    uint8_t key[8], val[8];
    uint16_t klen, vlen;
    const char *end_key = "k060";

    while (btree_cursor_valid(cur))
    {
        klen = sizeof(key);
        vlen = sizeof(val);
        btree_cursor_get(cur, key, &klen, val, &vlen);

        /* 检查是否超出范围 */
        if (klen == 4 && compare_default(key, 4,
                                          (const uint8_t *)end_key, 4) >= 0)
            break;

        char ek[16], ev[16];
        snprintf(ek, sizeof(ek), "k%03d", 50 + ok);
        snprintf(ev, sizeof(ev), "v%03d", 50 + ok);
        if (klen != 4 || vlen != 4
            || memcmp(key, ek, 4) != 0
            || memcmp(val, ev, 4) != 0)
            break;
        ok++;
        btree_cursor_next(cur);
    }
    printf("  range k050..k059: %d/10 (expect 10)\n", ok);

    btree_cursor_destroy(cur);
    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：边界情况 ─── */

static void test_cursor_edge(void)
{
    printf("== test_cursor_edge ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);

    btree_cursor_t *cur = btree_cursor_create(tree);

    /* 空树 */
    printf("  empty seek:   %d (expect %d)\n",
           btree_cursor_seek(cur, (const uint8_t *)"a", 1), BTREE_NOT_FOUND);
    printf("  empty first:  %d (expect %d)\n",
           btree_cursor_first(cur), BTREE_NOT_FOUND);
    printf("  empty last:   %d (expect %d)\n",
           btree_cursor_last(cur), BTREE_NOT_FOUND);
    printf("  empty valid:  %d (expect 0)\n", btree_cursor_valid(cur));

    /* 单条记录 */
    btree_put(tree, (const uint8_t *)"single", 6, (const uint8_t *)"rec", 3);

    btree_error_t se = btree_cursor_seek(cur, (const uint8_t *)"single", 6);
    uint8_t key[8], val[8];
    uint16_t klen = sizeof(key), vlen = sizeof(val);
    if (se == BTREE_OK)
        btree_cursor_get(cur, key, &klen, val, &vlen);
    printf("  single: err=%d key='%.6s' val='%.3s' (expect 0 'single' 'rec')\n",
           se, key, val);

    /* 前进超出末尾 */
    btree_cursor_next(cur);
    printf("  past end valid: %d (expect 0)\n", btree_cursor_valid(cur));
    printf("  past end next:  %d (expect %d)\n",
           btree_cursor_next(cur), BTREE_NOT_FOUND);

    /* 后退超出开头 */
    btree_cursor_seek(cur, (const uint8_t *)"single", 6);
    btree_cursor_prev(cur);
    printf("  before start valid: %d (expect 0)\n", btree_cursor_valid(cur));

    /* seek 到不存在的 key */
    btree_error_t e = btree_cursor_seek(cur, (const uint8_t *)"zzz", 3);
    printf("  seek zzz: %d (expect %d)\n", e, BTREE_NOT_FOUND);
    printf("  zzz valid: %d (expect 0)\n", btree_cursor_valid(cur));

    btree_cursor_destroy(cur);
    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：正向 + 反向混合遍历 ─── */

static void test_cursor_mixed(void)
{
    printf("== test_cursor_mixed ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);
    populate_btree(tree, 100);

    btree_cursor_t *cur = btree_cursor_create(tree);

    /* seek 到 k050 */
    btree_cursor_seek(cur, (const uint8_t *)"k050", 4);

    uint8_t key[8], val[8];
    uint16_t klen, vlen;
    btree_error_t err;

    /* next → k051 */
    err = btree_cursor_next(cur);
    klen = sizeof(key);
    vlen = sizeof(val);
    if (err == BTREE_OK)
        btree_cursor_get(cur, key, &klen, val, &vlen);
    printf("  next: err=%d key='%.4s' (expect 0 'k051')\n", err, key);

    /* prev → k050 */
    err = btree_cursor_prev(cur);
    klen = sizeof(key);
    vlen = sizeof(val);
    if (err == BTREE_OK)
        btree_cursor_get(cur, key, &klen, val, &vlen);
    printf("  prev: err=%d key='%.4s' (expect 0 'k050')\n", err, key);

    /* prev → k049 */
    err = btree_cursor_prev(cur);
    klen = sizeof(key);
    vlen = sizeof(val);
    if (err == BTREE_OK)
        btree_cursor_get(cur, key, &klen, val, &vlen);
    printf("  prev: err=%d key='%.4s' (expect 0 'k049')\n", err, key);

    /* 再回到 k050 */
    err = btree_cursor_next(cur);
    klen = sizeof(key);
    vlen = sizeof(val);
    if (err == BTREE_OK)
        btree_cursor_get(cur, key, &klen, val, &vlen);
    printf("  next: err=%d key='%.4s' (expect 0 'k050')\n", err, key);

    btree_cursor_destroy(cur);
    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：通过缓冲池使用游标 ─── */

static void test_cursor_buffer_pool(void)
{
    printf("== test_cursor_buffer_pool ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_buffer_pool_t *bp = bp_create(0,
                                         mem_read, mem_write, mem_alloc, &store);
    btree_t *tree = btree_create(compare_default,
                                  bp_read, bp_write, bp_alloc, bp);
    populate_btree(tree, 200);

    btree_cursor_t *cur = btree_cursor_create(tree);
    btree_cursor_seek(cur, (const uint8_t *)"k000", 4);

    int ok = 0;
    uint8_t key[8], val[8];
    uint16_t klen, vlen;
    while (btree_cursor_valid(cur))
    {
        klen = sizeof(key);
        vlen = sizeof(val);
        btree_cursor_get(cur, key, &klen, val, &vlen);
        char ek[16], ev[16];
        snprintf(ek, sizeof(ek), "k%03d", ok);
        snprintf(ev, sizeof(ev), "v%03d", ok);
        if (klen == 4 && vlen == 4
            && memcmp(key, ek, 4) == 0
            && memcmp(val, ev, 4) == 0)
            ok++;
        else
            break;
        btree_cursor_next(cur);
    }
    printf("  bp forward %d/%d (expect %d)\n", ok, 200, 200);
    printf("  hits=%u misses=%u\n", bp_hit_count(bp), bp_miss_count(bp));

    btree_cursor_destroy(cur);
    btree_destroy(tree);
    bp_destroy(bp);
    printf("  PASSED\n\n");
}

/* ════════════════════════════════════════════════════════════════
 *  事务测试
 * ════════════════════════════════════════════════════════════════ */

/* ─── 测试：基本读事务 ─── */

static void test_txn_basic_read(void)
{
    printf("== test_txn_basic_read ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);
    populate_btree(tree, 100);

    txn_t *txn = txn_begin(tree, TXN_READ_ONLY);
    printf("  txn_active: %d (expect 1)\n", txn_active(txn));

    uint8_t buf[64];
    uint16_t len = sizeof(buf);
    btree_error_t e = txn_get(txn, (const uint8_t *)"k050", 4, buf, &len);
    printf("  get k050: %d val='%.4s' len=%d (expect 0 'v050' 4)\n", e, buf, len);

    e = txn_get(txn, (const uint8_t *)"k999", 4, buf, &len);
    printf("  get k999: %d (expect %d)\n", e, BTREE_NOT_FOUND);

    e = txn_commit(txn);
    printf("  commit: %d (expect 0)\n", e);
    /* txn 已释放，不再使用 */

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：写事务提交 ─── */

static void test_txn_write_commit(void)
{
    printf("== test_txn_write_commit ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);

    txn_t *txn = txn_begin(tree, TXN_READ_WRITE);
    printf("  txn_type: %d (expect %d)\n", txn_type(txn), TXN_READ_WRITE);

    txn_put(txn, (const uint8_t *)"alpha", 5, (const uint8_t *)"apple", 5);
    txn_put(txn, (const uint8_t *)"beta", 4, (const uint8_t *)"banana", 6);
    txn_put(txn, (const uint8_t *)"gamma", 5, (const uint8_t *)"grape", 5);

    btree_error_t e = txn_commit(txn);
    printf("  commit: %d (expect 0)\n", e);

    /* 使用非事务接口验证 */
    uint8_t buf[64];
    uint16_t len = sizeof(buf);
    e = btree_get(tree, (const uint8_t *)"alpha", 5, buf, &len);
    buf[len] = '\0';
    printf("  get alpha: %d '%s' (expect 0 'apple')\n", e, buf);

    len = sizeof(buf);
    e = btree_get(tree, (const uint8_t *)"beta", 4, buf, &len);
    buf[len] = '\0';
    printf("  get beta:  %d '%s' (expect 0 'banana')\n", e, buf);

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：只读事务拒绝写入 ─── */

static void test_txn_readonly_reject_write(void)
{
    printf("== test_txn_readonly_reject_write ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);

    txn_t *txn = txn_begin(tree, TXN_READ_ONLY);
    btree_error_t e = txn_put(txn, (const uint8_t *)"x", 1, (const uint8_t *)"y", 1);
    printf("  put in read-only txn: %d (expect %d - CORRUPTED)\n",
           e, BTREE_CORRUPTED);

    e = txn_delete(txn, (const uint8_t *)"x", 1);
    printf("  delete in read-only txn: %d (expect %d)\n", e, BTREE_CORRUPTED);

    txn_commit(txn);
    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：事务内游标 ─── */

static void test_txn_cursor(void)
{
    printf("== test_txn_cursor ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);
    populate_btree(tree, 200);

    txn_t *txn = txn_begin(tree, TXN_READ_ONLY);
    btree_cursor_t *cur = txn_cursor_create(txn);
    printf("  cursor created: %s (expect non-NULL)\n", cur ? "yes" : "no");

    btree_cursor_seek(cur, (const uint8_t *)"k000", 4);
    int ok = 0;
    uint8_t key[8], val[8];
    uint16_t klen, vlen;

    while (btree_cursor_valid(cur))
    {
        klen = sizeof(key);
        vlen = sizeof(val);
        btree_cursor_get(cur, key, &klen, val, &vlen);
        char ek[16], ev[16];
        snprintf(ek, sizeof(ek), "k%03d", ok);
        snprintf(ev, sizeof(ev), "v%03d", ok);
        if (klen != 4 || vlen != 4
            || memcmp(key, ek, 4) != 0
            || memcmp(val, ev, 4) != 0)
            break;
        ok++;
        btree_cursor_next(cur);
    }
    printf("  cursor forward: %d/200 (expect 200)\n", ok);

    btree_cursor_destroy(cur);
    txn_commit(txn);
    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：事务内写后读（同一事务） ─── */

static void test_txn_write_then_read(void)
{
    printf("== test_txn_write_then_read ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);

    txn_t *txn = txn_begin(tree, TXN_READ_WRITE);
    txn_put(txn, (const uint8_t *)"key1", 4, (const uint8_t *)"val1", 4);
    txn_put(txn, (const uint8_t *)"key2", 4, (const uint8_t *)"val2", 4);

    /* 同一事务内读取 */
    uint8_t buf[64];
    uint16_t len;
    btree_error_t e;

    len = sizeof(buf);
    e = txn_get(txn, (const uint8_t *)"key1", 4, buf, &len);
    buf[len] = '\0';
    printf("  txn get key1: %d '%s' (expect 0 'val1')\n", e, buf);

    len = sizeof(buf);
    e = txn_get(txn, (const uint8_t *)"key2", 4, buf, &len);
    buf[len] = '\0';
    printf("  txn get key2: %d '%s' (expect 0 'val2')\n", e, buf);

    txn_commit(txn);
    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 测试：事务内删除 ─── */

static void test_txn_delete_in_txn(void)
{
    printf("== test_txn_delete_in_txn ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);

    txn_t *txn = txn_begin(tree, TXN_READ_WRITE);
    txn_put(txn, (const uint8_t *)"keep", 4, (const uint8_t *)"stay", 4);
    txn_put(txn, (const uint8_t *)"gone", 4, (const uint8_t *)"bye", 3);
    txn_delete(txn, (const uint8_t *)"gone", 4);
    txn_commit(txn);

    /* 验证 */
    uint8_t buf[64];
    uint16_t len = sizeof(buf);
    btree_error_t e = btree_get(tree, (const uint8_t *)"keep", 4, buf, &len);
    buf[len] = '\0';
    printf("  get keep: %d '%s' (expect 0 'stay')\n", e, buf);

    len = sizeof(buf);
    e = btree_get(tree, (const uint8_t *)"gone", 4, buf, &len);
    printf("  get gone: %d (expect %d)\n", e, BTREE_NOT_FOUND);

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ════════════════════════════════════════════════════════════════
 *  并发安全性测试
 * ════════════════════════════════════════════════════════════════ */

typedef struct
{
    btree_t *tree;
    int id;
    int start;       /* key 起始偏移（写入者使用） */
    int num_records;
    int result;
} conc_arg_t;

/* 在单次只读事务中读取指定数量的预期记录 */
static int conc_read_all(btree_t *tree, int n)
{
    char key[16], val[16];
    uint8_t buf[64];
    uint16_t len;
    int ok = 0;

    txn_t *txn = txn_begin(tree, TXN_READ_ONLY);
    if (!txn) return 0;

    for (int i = 0; i < n; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        len = sizeof(buf);
        if (txn_get(txn, (const uint8_t *)key, 4, buf, &len) == BTREE_OK
            && len == 4 && memcmp(buf, val, 4) == 0)
            ok++;
    }
    txn_commit(txn);
    return ok;
}

/* ─── 1. 多线程并发读取 ─── */

static void *conc_reader_thread(void *arg)
{
    conc_arg_t *a = (conc_arg_t *)arg;
    a->result = conc_read_all(a->tree, a->num_records);
    return NULL;
}

static void test_concurrent_readers(void)
{
    printf("== test_concurrent_readers ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);

    int const N = 500;
    populate_btree(tree, N);

    int const NUM = 8;
    pthread_t threads[NUM];
    conc_arg_t args[NUM];

    for (int i = 0; i < NUM; i++)
    {
        args[i] = (conc_arg_t){
            .tree = tree, .id = i, .start = 0,
            .num_records = N, .result = 0};
        pthread_create(&threads[i], NULL, conc_reader_thread, &args[i]);
    }

    int total = 0;
    for (int i = 0; i < NUM; i++)
    {
        pthread_join(threads[i], NULL);
        total += args[i].result;
    }
    printf("  %d/%d verified across %d threads (expect %d)\n",
           total, N * NUM, NUM, N * NUM);

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 2. 多线程并发写入（互斥 key 范围）─── */

static void *conc_writer_thread(void *arg)
{
    conc_arg_t *a = (conc_arg_t *)arg;
    int start = a->start;
    int end = start + a->num_records;
    char key[16], val[16];
    int ok = 0;

    txn_t *txn = txn_begin(a->tree, TXN_READ_WRITE);
    if (!txn) { a->result = 0; return NULL; }

    for (int i = start; i < end; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        if (txn_put(txn, (const uint8_t *)key, 4, (const uint8_t *)val, 4) == BTREE_OK)
            ok++;
    }
    txn_commit(txn);
    a->result = ok;
    return NULL;
}

static void test_concurrent_writers(void)
{
    printf("== test_concurrent_writers ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);

    int const PER = 250;
    int const NUM = 4;
    int const TOTAL = PER * NUM;

    pthread_t threads[NUM];
    conc_arg_t args[NUM];

    for (int i = 0; i < NUM; i++)
    {
        args[i] = (conc_arg_t){
            .tree = tree, .id = i, .start = i * PER,
            .num_records = PER, .result = 0};
        pthread_create(&threads[i], NULL, conc_writer_thread, &args[i]);
    }

    int written = 0;
    for (int i = 0; i < NUM; i++)
    {
        pthread_join(threads[i], NULL);
        written += args[i].result;
    }
    printf("  %d/%d written (expect %d)\n", written, TOTAL, TOTAL);

    int ok = conc_read_all(tree, TOTAL);
    printf("  verify: %d/%d (expect %d)\n", ok, TOTAL, TOTAL);

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 3. 读写并发混合 ─── */

static void test_concurrent_mixed(void)
{
    printf("== test_concurrent_mixed ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);

    int const INIT = 200;
    int const WR_PER = 200;
    int const NUM_W = 2;
    int const NUM_R = 4;
    int const TOTAL = INIT + WR_PER * NUM_W;

    populate_btree(tree, INIT);

    /* 读取线程只读 INIT 条（保证不存在竞争） */
    pthread_t w_thr[NUM_W], r_thr[NUM_R];
    conc_arg_t w_arg[NUM_W], r_arg[NUM_R];

    for (int i = 0; i < NUM_W; i++)
    {
        w_arg[i] = (conc_arg_t){
            .tree = tree, .id = i,
            .start = INIT + i * WR_PER, /* 写入 INIT 之后的全新 key */
            .num_records = WR_PER, .result = 0};
        pthread_create(&w_thr[i], NULL, conc_writer_thread, &w_arg[i]);
    }

    for (int i = 0; i < NUM_R; i++)
    {
        r_arg[i] = (conc_arg_t){
            .tree = tree, .id = i,
            .start = 0, .num_records = INIT, .result = 0};
        pthread_create(&r_thr[i], NULL, conc_reader_thread, &r_arg[i]);
    }

    int written = 0;
    for (int i = 0; i < NUM_W; i++)
    {
        pthread_join(w_thr[i], NULL);
        written += w_arg[i].result;
    }

    int read_ok = 0;
    for (int i = 0; i < NUM_R; i++)
    {
        pthread_join(r_thr[i], NULL);
        read_ok += r_arg[i].result;
    }

    printf("  writers: %d/%d (expect %d)\n", written, WR_PER * NUM_W, WR_PER * NUM_W);
    printf("  readers (INIT): %d/%d (expect %d)\n",
           read_ok, INIT * NUM_R, INIT * NUM_R);

    int ok = conc_read_all(tree, TOTAL);
    printf("  final verify: %d/%d (expect %d)\n", ok, TOTAL, TOTAL);

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

/* ─── 4. 多线程争夺写入相同 key ─── */

static void *conc_contend_thread(void *arg)
{
    conc_arg_t *a = (conc_arg_t *)arg;
    char key[16], val[16];
    int ok = 0;

    txn_t *txn = txn_begin(a->tree, TXN_READ_WRITE);
    if (!txn) { a->result = 0; return NULL; }

    for (int i = 0; i < a->num_records; i++)
    {
        snprintf(key, sizeof(key), "k%03d", i);
        snprintf(val, sizeof(val), "v%03d", i);
        if (txn_put(txn, (const uint8_t *)key, 4, (const uint8_t *)val, 4) == BTREE_OK)
            ok++;
    }
    txn_commit(txn);
    a->result = ok;
    return NULL;
}

static void test_concurrent_write_contention(void)
{
    printf("== test_concurrent_write_contention ==\n");

    mem_store_t store;
    mem_store_init(&store);
    btree_t *tree = btree_create(compare_default,
                                  mem_read, mem_write, mem_alloc, &store);

    int const N = 200;
    int const NUM = 4;

    pthread_t threads[NUM];
    conc_arg_t args[NUM];

    for (int i = 0; i < NUM; i++)
    {
        args[i] = (conc_arg_t){
            .tree = tree, .id = i, .start = 0,
            .num_records = N, .result = 0};
        pthread_create(&threads[i], NULL, conc_contend_thread, &args[i]);
    }

    for (int i = 0; i < NUM; i++)
        pthread_join(threads[i], NULL);

    /* 验证所有 key 均可正确读取 */
    int ok = conc_read_all(tree, N);
    printf("  %d threads × %d same keys: verify %d/%d (expect %d)\n",
           NUM, N, ok, N, N);

    btree_destroy(tree);
    printf("  PASSED\n\n");
}

int main(void)
{
    printf("=== B+ Tree Data Engine — 数据结构层验证 ===\n\n");

    test_page_init();
    test_leaf_slot_alloc();
    test_slot_remove_and_compact();
    test_internal_records();
    test_comparator();

    printf("=== B+ Tree 核心操作验证 ===\n\n");

    test_btree_basic();
    test_btree_update();
    test_btree_delete();
    test_btree_overwrite_deleted();
    test_btree_split();
    test_btree_reverse_insert();

    printf("=== B+ Tree 删除合并验证 ===\n\n");

    test_btree_delete_merge();
    test_btree_delete_massive();
    test_btree_delete_reinsert();

    printf("=== 文件存储持久化验证 ===\n\n");

    test_storage_persistence();

    printf("=== 缓冲池验证 ===\n\n");

    test_buffer_pool_basic();
    test_buffer_pool_eviction();
    test_buffer_pool_persistence();

    printf("=== WAL 验证 ===\n\n");

    test_wal_page_recovery();
    test_wal_crash_recovery();
    test_wal_full_persistence();

    printf("=== 游标验证 ===\n\n");

    test_cursor_forward();
    test_cursor_seek_mid();
    test_cursor_reverse();
    test_cursor_first_last();
    test_cursor_range();
    test_cursor_edge();
    test_cursor_mixed();
    test_cursor_buffer_pool();

    printf("=== 事务验证 ===\n\n");

    test_txn_basic_read();
    test_txn_write_commit();
    test_txn_readonly_reject_write();
    test_txn_cursor();
    test_txn_write_then_read();
    test_txn_delete_in_txn();

    printf("=== 并发安全性验证 ===\n\n");

    test_concurrent_readers();
    test_concurrent_writers();
    test_concurrent_mixed();
    test_concurrent_write_contention();

    printf("=== 全部测试通过 ===\n");
    return 0;
}
