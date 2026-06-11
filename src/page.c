// 页头、内部节点槽位、叶子节点槽位布局
#include "page.h"

/*
 * 页面布局（双向生长模型）：
 *
 *   低地址   ┌──────────┬──────────┬──────────┬──────┬──────────┐  高地址
 *           │ 页头 。32 │ slot[0]  │ slot[1]  │ 空闲。│ records  │
 *           │          │ slot[2]  │  ...     │ 区域。│从尾向前。。│
 *           └──────────┴──────────┴──────────┴──────┴──────────┘
 *           ↑                     ↑                   ↑
 *           header              front_end          rec_off
 *
 *   - 槽位从 PAGE_HEADER_SIZE 开始向前生长
 *   - 记录从 PAGE_SIZE 开始向后生长
 *   - free_offset = 记录区起始偏移（空闲区尾部边界）
 *   - free_size   = rec_off - front_end
 *   - front_end   = PAGE_HEADER_SIZE + num_slots * SLOT_SIZE
 */

/* ── 页头初始化 ── */

void page_init(page_t *page, page_id_t pid, page_type_t type)
{
    memset(page, 0, PAGE_SIZE);
    page->header.page_id = pid;
    page->header.type = (uint8_t)type;
    page->header.num_slots = 0;
    page->header.free_offset = PAGE_SIZE; /* 记录区从页尾开始 */
    page->header.free_size = PAGE_SIZE - PAGE_HEADER_SIZE;
    page->header.parent_id = INVALID_PAGE_ID;
    page->header.prev_id = INVALID_PAGE_ID;
    page->header.next_id = INVALID_PAGE_ID;
    page->header.first_child_id = INVALID_PAGE_ID;
}

/* ── 槽位管理 ── */

int page_alloc_slot(page_t *page, const uint8_t *data, uint16_t len)
{
    uint16_t front_end = (uint16_t)(PAGE_HEADER_SIZE + page->header.num_slots * sizeof(slot_t));

    /* 检查空闲空间是否足够 */
    if (page->header.free_offset < front_end + len)
        return -1;

    /* 1) 将记录写入记录区尾部（从 free_offset 向左增长） */
    uint16_t rec_off = page->header.free_offset - len;
    memcpy(page->bytes + rec_off, data, len);

    /* 2) 槽位写入前端增长区 */
    slot_t *s = (slot_t *)(page->bytes + front_end);
    s->offset = rec_off;
    s->length = len;

    page->header.num_slots++;
    page->header.free_offset = rec_off;
    page->header.free_size = rec_off - front_end - (uint16_t)sizeof(slot_t);

    return (int)(page->header.num_slots - 1);
}

int page_insert_slot(page_t *page, int idx, const uint8_t *data, uint16_t len)
{
    int n = (int)page->header.num_slots;
    if (idx < 0 || idx > n)
        return -1;

    uint16_t front_end = (uint16_t)(PAGE_HEADER_SIZE + n * sizeof(slot_t));

    if (page->header.free_offset < front_end + sizeof(slot_t) + len)
        return -1;

    uint16_t rec_off = page->header.free_offset - len;
    memcpy(page->bytes + rec_off, data, len);

    uint8_t *slot_base = page->bytes + PAGE_HEADER_SIZE;
    memmove(slot_base + (size_t)(idx + 1) * sizeof(slot_t),
            slot_base + (size_t)idx * sizeof(slot_t),
            (size_t)(n - idx) * sizeof(slot_t));

    slot_t *s = (slot_t *)(slot_base + (size_t)idx * sizeof(slot_t));
    s->offset = rec_off;
    s->length = len;

    page->header.num_slots++;
    page->header.free_offset = rec_off;
    page->header.free_size = rec_off - PAGE_HEADER_SIZE - page->header.num_slots * sizeof(slot_t);

    return idx;
}

void page_remove_slot(page_t *page, int idx)
{
    int n = (int)page->header.num_slots;
    if (idx < 0 || idx >= n)
        return;

    /* 1) 移除槽位：将后面的项前移 */
    uint8_t *slot_base = page->bytes + PAGE_HEADER_SIZE;
    if (idx < n - 1)
    {
        memmove(slot_base + (size_t)idx * sizeof(slot_t),
                slot_base + (size_t)(idx + 1) * sizeof(slot_t),
                (size_t)(n - idx - 1) * sizeof(slot_t));
    }
    page->header.num_slots--;
    n--;

    if (n == 0)
    {
        page->header.free_offset = PAGE_SIZE;
        page->header.free_size = PAGE_SIZE - PAGE_HEADER_SIZE;
        return;
    }

    /* 2) 压缩：先拷贝所有记录到临时缓冲区，再按序写回。
     *
     *    为什么不能原地 memmove：
     *      经过多次非顺序插入后，slot 索引与记录在页内的偏移位置不再相关。
     *      按 slot 索引 0→N 顺序处理的原地压缩，可能让前一次写操作覆盖
     *      后一次读操作的源数据（因为低 slot 索引的新写入位置在高地址区，
     *      可能覆盖高 slot 索引的原始数据）。
     *
     *    临时缓冲区方案彻底避免重叠问题：
     *      先完整读出所有记录，再重新写入正确位置。
     */
    uint8_t temp[PAGE_SIZE];
    uint16_t data_pos = 0;

    for (int i = 0; i < n; i++)
    {
        slot_t *s = (slot_t *)(slot_base + (size_t)i * sizeof(slot_t));
        memcpy(temp + data_pos, page->bytes + s->offset, s->length);
        data_pos += s->length;
    }

    uint16_t rec_off = PAGE_SIZE;
    data_pos = 0;
    for (int i = 0; i < n; i++)
    {
        slot_t *s = (slot_t *)(slot_base + (size_t)i * sizeof(slot_t));
        rec_off -= s->length;
        memcpy(page->bytes + rec_off, temp + data_pos, s->length);
        s->offset = rec_off;
        data_pos += s->length;
    }

    page->header.free_offset = rec_off;
    page->header.free_size = rec_off - PAGE_HEADER_SIZE - (uint16_t)(n * sizeof(slot_t));
}

slot_t page_get_slot(const page_t *page, int idx)
{
    int n = (int)page->header.num_slots;
    if (idx < 0 || idx >= n)
    {
        slot_t empty = {0, 0};
        return empty;
    }
    const slot_t *s = (const slot_t *)(page->bytes + PAGE_HEADER_SIZE + (size_t)idx * sizeof(slot_t));
    return *s;
}

uint8_t *page_slot_data(page_t *page, int idx)
{
    slot_t s = page_get_slot(page, idx);
    if (s.length == 0)
        return NULL;
    return page->bytes + s.offset;
}

const uint8_t *page_slot_data_c(const page_t *page, int idx)
{
    slot_t s = page_get_slot(page, idx);
    if (s.length == 0)
        return NULL;
    return page->bytes + s.offset;
}
