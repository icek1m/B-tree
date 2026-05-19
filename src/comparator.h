// 自定义键比较接口
#ifndef BTREE_COMPARATOR_H
// 比较器接口声明
#define BTREE_COMPARATOR_H

#include "types.h"
#include <stdint.h>

/*
 * 键比较函数类型。
 * 返回负值 → a < b
 * 返回 0   → a == b
 * 返回正值 → a > b
 */
typedef int (*btree_compare_t)(const uint8_t *a, uint16_t a_len,
                               const uint8_t *b, uint16_t b_len);

/* 默认字节序比较器：逐字节 memcmp，长度不等时短者小 */
int compare_default(const uint8_t *a, uint16_t a_len,
                    const uint8_t *b, uint16_t b_len);

#endif /* BTREE_COMPARATOR_H */
