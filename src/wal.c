// WAL 实现 —— 页级 REDO 日志，先写日志再写数据
#define _POSIX_C_SOURCE 200809L  /* 用于 strdup, fileno, fsync */
#include "wal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ════════════════════════════════════════════════════════════════
 *  内部常量 & 类型
 * ════════════════════════════════════════════════════════════════ */

#define WAL_MAGIC        0x314C4157u  /* "WAL1" */
#define WAL_VERSION      1
#define WAL_HEADER_SIZE  32

/* WAL 文件头 */
#pragma pack(push, 1)
typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint8_t  reserved[24];
} wal_file_header_t;
#pragma pack(pop)

_Static_assert(sizeof(wal_file_header_t) == WAL_HEADER_SIZE,
               "wal_file_header_t must be exactly 32 bytes");

/* 每个日志条目 = 页号(4) + 页数据(PAGE_SIZE) */
#define WAL_ENTRY_SIZE  ((uint32_t)(sizeof(page_id_t) + PAGE_SIZE))

/* WAL 句柄 */
struct wal
{
    FILE *fp;
    char *path;

    btree_read_page_t  storage_read;
    btree_write_page_t storage_write;
    btree_alloc_page_t storage_alloc;
    void *storage_ctx;
};

/* ════════════════════════════════════════════════════════════════
 *  内部工具
 * ════════════════════════════════════════════════════════════════ */

static btree_error_t write_header(wal_t *wal)
{
    wal_file_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic   = WAL_MAGIC;
    hdr.version = WAL_VERSION;

    if (fseek(wal->fp, 0, SEEK_SET) != 0)
        return BTREE_IO_ERROR;
    if (fwrite(&hdr, sizeof(hdr), 1, wal->fp) != 1)
        return BTREE_IO_ERROR;
    if (fflush(wal->fp) != 0)
        return BTREE_IO_ERROR;
    if (fsync(fileno(wal->fp)) != 0)
        return BTREE_IO_ERROR;
    return BTREE_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  生命周期
 * ════════════════════════════════════════════════════════════════ */

wal_t *wal_create(const char *wal_path,
                  btree_read_page_t  storage_read,
                  btree_write_page_t storage_write,
                  btree_alloc_page_t storage_alloc,
                  void *storage_ctx)
{
    wal_t *wal = (wal_t *)calloc(1, sizeof(wal_t));
    if (!wal)
        return NULL;

    wal->path = strdup(wal_path ? wal_path : "btree.wal");
    if (!wal->path)
    {
        free(wal);
        return NULL;
    }

    /* "ab+"：追加+读，文件存在则打开，不存在则创建，不截断 */
    wal->fp = fopen(wal->path, "ab+");
    if (!wal->fp)
    {
        free(wal->path);
        free(wal);
        return NULL;
    }

    /* 新文件写文件头 */
    fseek(wal->fp, 0, SEEK_END);
    long size = ftell(wal->fp);
    if (size == 0)
    {
        if (write_header(wal) != BTREE_OK)
        {
            fclose(wal->fp);
            free(wal->path);
            free(wal);
            return NULL;
        }
    }

    wal->storage_read  = storage_read;
    wal->storage_write = storage_write;
    wal->storage_alloc = storage_alloc;
    wal->storage_ctx   = storage_ctx;

    return wal;
}

void wal_destroy(wal_t *wal)
{
    if (!wal)
        return;
    if (wal->fp)
        fclose(wal->fp);
    free(wal->path);
    free(wal);
}

/* ════════════════════════════════════════════════════════════════
 *  I/O 回调
 * ════════════════════════════════════════════════════════════════ */

btree_error_t wal_read(void *ctx, page_id_t pid, page_t *page)
{
    wal_t *wal = (wal_t *)ctx;
    return wal->storage_read(wal->storage_ctx, pid, page);
}

btree_error_t wal_write(void *ctx, page_id_t pid, const page_t *page)
{
    wal_t *wal = (wal_t *)ctx;

    /* 1. 写 WAL 日志（追加到文件尾部） */
    if (fseek(wal->fp, 0, SEEK_END) != 0)
        return BTREE_IO_ERROR;
    if (fwrite(&pid, sizeof(pid), 1, wal->fp) != 1)
        return BTREE_IO_ERROR;
    if (fwrite(page, PAGE_SIZE, 1, wal->fp) != 1)
        return BTREE_IO_ERROR;

    /* 2. fsync WAL（日志先于数据落盘） */
    if (fflush(wal->fp) != 0)
        return BTREE_IO_ERROR;
    if (fsync(fileno(wal->fp)) != 0)
        return BTREE_IO_ERROR;

    /* 3. 写下层存储 */
    return wal->storage_write(wal->storage_ctx, pid, page);
}

btree_error_t wal_alloc(void *ctx, page_id_t *pid, page_type_t type)
{
    wal_t *wal = (wal_t *)ctx;
    return wal->storage_alloc(wal->storage_ctx, pid, type);
}

/* ════════════════════════════════════════════════════════════════
 *  恢复
 * ════════════════════════════════════════════════════════════════ */

btree_error_t wal_recover(wal_t *wal)
{
    /* 定位到数据区起始 */
    if (fseek(wal->fp, WAL_HEADER_SIZE, SEEK_SET) != 0)
        return BTREE_IO_ERROR;

    page_id_t pid;
    page_t page;
    uint32_t recovered = 0;

    while (fread(&pid, sizeof(pid), 1, wal->fp) == 1)
    {
        if (fread(page.bytes, PAGE_SIZE, 1, wal->fp) != 1)
            return BTREE_CORRUPTED;

        btree_error_t err = wal->storage_write(wal->storage_ctx, pid, &page);
        if (err != BTREE_OK)
            return err;
        recovered++;
    }

    /* 清空 WAL 文件 */
    fclose(wal->fp);
    wal->fp = fopen(wal->path, "wb+");
    if (!wal->fp)
        return BTREE_IO_ERROR;

    btree_error_t err = write_header(wal);
    if (err != BTREE_OK)
        return err;

    printf("  WAL recover: %u pages replayed\n", recovered);
    return BTREE_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  统计
 * ════════════════════════════════════════════════════════════════ */

uint32_t wal_num_entries(wal_t *wal)
{
    if (fseek(wal->fp, 0, SEEK_END) != 0)
        return 0;
    long size = ftell(wal->fp);
    if (size <= (long)WAL_HEADER_SIZE)
        return 0;
    return (uint32_t)((size - WAL_HEADER_SIZE) / WAL_ENTRY_SIZE);
}
