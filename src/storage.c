// 文件存储层实现
#include "storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── 文件头格式 ─── */
#pragma pack(push, 1)
typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t num_pages;
    uint32_t root_id;
    uint8_t reserved[48];
} file_header_t;
#pragma pack(pop)

_Static_assert(sizeof(file_header_t) == STORAGE_HEADER_SIZE,
               "file_header_t must be exactly STORAGE_HEADER_SIZE bytes");

/* ─── 存储句柄 ─── */
struct btree_storage
{
    FILE *fp;
    uint32_t num_pages;
    page_id_t root_id;
};

/* ---------- 内部辅助 ---------- */

static uint64_t page_offset(page_id_t pid)
{
    return (uint64_t)STORAGE_HEADER_SIZE + (uint64_t)pid * PAGE_SIZE;
}

static btree_error_t write_header(btree_storage_t *store)
{
    file_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = BTREE_STORAGE_MAGIC;
    hdr.version = BTREE_STORAGE_VERSION;
    hdr.num_pages = store->num_pages;
    hdr.root_id = store->root_id;

    if (fseek(store->fp, 0, SEEK_SET) != 0)
        return BTREE_IO_ERROR;
    if (fwrite(&hdr, sizeof(hdr), 1, store->fp) != 1)
        return BTREE_IO_ERROR;
    if (fflush(store->fp) != 0)
        return BTREE_IO_ERROR;
    return BTREE_OK;
}

static btree_error_t read_header(btree_storage_t *store)
{
    file_header_t hdr;
    if (fseek(store->fp, 0, SEEK_SET) != 0)
        return BTREE_IO_ERROR;
    if (fread(&hdr, sizeof(hdr), 1, store->fp) != 1)
        return BTREE_IO_ERROR;

    if (hdr.magic != BTREE_STORAGE_MAGIC || hdr.version != BTREE_STORAGE_VERSION)
        return BTREE_CORRUPTED;

    store->num_pages = hdr.num_pages;
    store->root_id = hdr.root_id;
    return BTREE_OK;
}

/* ---------- 生命周期 ---------- */

btree_storage_t *btree_storage_create(const char *path)
{
    FILE *fp = fopen(path, "wb+");
    if (!fp)
        return NULL;

    btree_storage_t *store = (btree_storage_t *)calloc(1, sizeof(btree_storage_t));
    if (!store)
    {
        fclose(fp);
        return NULL;
    }

    store->fp = fp;
    store->num_pages = 0;
    store->root_id = INVALID_PAGE_ID;

    if (write_header(store) != BTREE_OK)
    {
        fclose(fp);
        free(store);
        return NULL;
    }

    return store;
}

btree_storage_t *btree_storage_open(const char *path)
{
    FILE *fp = fopen(path, "rb+");
    if (!fp)
        return NULL;

    btree_storage_t *store = (btree_storage_t *)calloc(1, sizeof(btree_storage_t));
    if (!store)
    {
        fclose(fp);
        return NULL;
    }

    store->fp = fp;

    if (read_header(store) != BTREE_OK)
    {
        fclose(fp);
        free(store);
        return NULL;
    }

    return store;
}

void btree_storage_close(btree_storage_t *store)
{
    if (!store)
        return;

    write_header(store);  /* 最后刷一次元数据 */
    fclose(store->fp);
    free(store);
}

/* ---------- root_id ---------- */

btree_error_t btree_storage_set_root_id(btree_storage_t *store, page_id_t root_id)
{
    store->root_id = root_id;
    return write_header(store);
}

btree_error_t btree_storage_get_root_id(btree_storage_t *store, page_id_t *root_id)
{
    *root_id = store->root_id;
    return BTREE_OK;
}

/* ---------- I/O 回调 ---------- */

btree_error_t btree_storage_read(void *ctx, page_id_t pid, page_t *page)
{
    btree_storage_t *store = (btree_storage_t *)ctx;

    if (pid >= store->num_pages)
        return BTREE_IO_ERROR;

    if (fseek(store->fp, page_offset(pid), SEEK_SET) != 0)
        return BTREE_IO_ERROR;
    if (fread(page, PAGE_SIZE, 1, store->fp) != 1)
        return BTREE_IO_ERROR;

    return BTREE_OK;
}

btree_error_t btree_storage_write(void *ctx, page_id_t pid, const page_t *page)
{
    btree_storage_t *store = (btree_storage_t *)ctx;

    if (pid >= store->num_pages)
        return BTREE_IO_ERROR;

    if (fseek(store->fp, page_offset(pid), SEEK_SET) != 0)
        return BTREE_IO_ERROR;
    if (fwrite(page, PAGE_SIZE, 1, store->fp) != 1)
        return BTREE_IO_ERROR;

    return BTREE_OK;
}

btree_error_t btree_storage_alloc(void *ctx, page_id_t *pid, page_type_t type)
{
    btree_storage_t *store = (btree_storage_t *)ctx;
    page_t page;

    *pid = store->num_pages;
    page_init(&page, *pid, type);

    /* 扩展文件：写入新页 */
    if (fseek(store->fp, page_offset(*pid), SEEK_SET) != 0)
        return BTREE_IO_ERROR;
    if (fwrite(&page, PAGE_SIZE, 1, store->fp) != 1)
        return BTREE_IO_ERROR;

    store->num_pages++;

    /* 更新文件头中的页数 */
    if (write_header(store) != BTREE_OK)
        return BTREE_IO_ERROR;

    return BTREE_OK;
}
