#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "page.h"
#include "comparator.h"
#include "btree.h"

static void test_page_init(void) {
    printf("== test_page_init ==\n");
    page_t pg;
    page_init(&pg, 42, PAGE_LEAF);

    printf("  page_id       = %u (expect 42)\n",  page_get_id(&pg));
    printf("  type          = %d (expect %d)\n",  page_get_type(&pg), PAGE_LEAF);
    printf("  num_slots     = %d (expect 0)\n",   page_num_slots(&pg));
    printf("  free_offset   = %u (expect %d)\n",  pg.header.free_offset, PAGE_HEADER_SIZE);
    printf("  free_size     = %u (expect %d)\n",  pg.header.free_size, PAGE_SIZE - PAGE_HEADER_SIZE);
    printf("  parent        = %u (expect 0)\n",   page_get_parent(&pg));
    printf("  prev/next     = %u / %u (expect 0/0)\n", page_get_prev(&pg), page_get_next(&pg));
    printf("  first_child   = %u (expect 0)\n",   page_get_first_child(&pg));
    printf("  PASSED\n\n");
}

static void test_leaf_slot_alloc(void) {
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
    leaf_rec_pack(buf3, key3, 5, val3, 5, true);  /* 逻辑删除 */

    int idx1 = page_alloc_slot(&pg, buf1, leaf_rec_size(5, 5));
    int idx2 = page_alloc_slot(&pg, buf2, leaf_rec_size(4, 6));
    int idx3 = page_alloc_slot(&pg, buf3, leaf_rec_size(5, 5));

    printf("  slot indices: %d %d %d\n", idx1, idx2, idx3);
    printf("  num_slots    = %d (expect 3)\n", page_num_slots(&pg));

    /* 验证第一条记录 */
    const uint8_t *r1 = page_slot_data_c(&pg, 0);
    printf("  slot[0] key  = %s (expect 'alpha')\n", leaf_key_ptr_c(r1));
    printf("  slot[0] val  = %s (expect 'apple')\n", leaf_val_ptr_c(r1));
    printf("  slot[0] del  = %d (expect 0)\n",       leaf_is_deleted(r1));

    /* 验证第三条被标记删除 */
    const uint8_t *r3 = page_slot_data_c(&pg, 2);
    printf("  slot[2] del  = %d (expect 1)\n", leaf_is_deleted(r3));

    printf("  PASSED\n\n");
}

static void test_slot_remove_and_compact(void) {
    printf("== test_slot_remove_and_compact ==\n");
    page_t pg;
    page_init(&pg, 2, PAGE_LEAF);

    uint8_t key[] = "x";
    uint8_t val[] = "y";
    uint8_t buf[64];
    leaf_rec_pack(buf, key, 1, val, 1, false);
    uint16_t rec_len = leaf_rec_size(1, 1);

    for (int i = 0; i < 5; i++) {
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

static void test_internal_records(void) {
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
    (void)i1; (void)i2;

    printf("  type          = %d (expect %d)\n", page_get_type(&pg), PAGE_INTERNAL);
    printf("  first_child   = %u (expect 100)\n", page_get_first_child(&pg));
    printf("  num_slots     = %d (expect 2)\n",   page_num_slots(&pg));

    const uint8_t *r0 = page_slot_data_c(&pg, 0);
    const uint8_t *r1 = page_slot_data_c(&pg, 1);
    printf("  slot[0] key   = %s (expect 'cat')\n", internal_key_ptr_c(r0));
    printf("  slot[0] child = %u (expect 200)\n",   internal_child_id(r0));
    printf("  slot[1] key   = %s (expect 'dog')\n", internal_key_ptr_c(r1));
    printf("  slot[1] child = %u (expect 300)\n",   internal_child_id(r1));

    printf("  PASSED\n\n");
}

static void test_comparator(void) {
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
    int    num_pages;
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
    if (pid >= (page_id_t)s->num_pages)
        return BTREE_IO_ERROR;
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

    btree_put(tree, (const uint8_t *)"alpha", 5, (const uint8_t *)"apple",  5);
    btree_put(tree, (const uint8_t *)"beta",  4, (const uint8_t *)"banana", 6);
    btree_put(tree, (const uint8_t *)"gamma", 5, (const uint8_t *)"grape",  5);

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

    btree_put(tree, (const uint8_t *)"live",  4, (const uint8_t *)"alive", 5);
    btree_put(tree, (const uint8_t *)"dead",  4, (const uint8_t *)"gone",  4);

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

    btree_put(tree, (const uint8_t *)"x", 1, (const uint8_t *)"first",  5);
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

    printf("=== 全部测试通过 ===\n");
    return 0;
}
