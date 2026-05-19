#include <stdio.h>
#include "page.h"
#include "comparator.h"

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

int main(void) {
    printf("=== B+ Tree Data Engine — 数据结构层验证 ===\n\n");

    test_page_init();
    test_leaf_slot_alloc();
    test_slot_remove_and_compact();
    test_internal_records();
    test_comparator();

    printf("=== 全部测试通过 ===\n");
    return 0;
}
