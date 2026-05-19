// 基础类型定义、常量、错误码
#ifndef BTREE_TYPES_H
#define BTREE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PAGE_SIZE       4096
#define INVALID_PAGE_ID 0

typedef uint32_t page_id_t;

typedef enum {
    PAGE_UNDEFINED = 0,
    PAGE_INTERNAL  = 1,
    PAGE_LEAF      = 2,
} page_type_t;

typedef enum {
    BTREE_OK = 0,
    BTREE_NOT_FOUND,
    BTREE_PAGE_FULL,
    BTREE_IO_ERROR,
    BTREE_OUT_OF_MEMORY,
    BTREE_CORRUPTED,
    BTREE_DUPLICATE_KEY,
} btree_error_t;

#endif /* BTREE_TYPES_H */
