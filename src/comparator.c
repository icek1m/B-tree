// 自定义键比较接口
#include "comparator.h"
#include <string.h>

int compare_default(const uint8_t *a, uint16_t a_len,
                    const uint8_t *b, uint16_t b_len)
{
    uint16_t min_len = a_len < b_len ? a_len : b_len;
    int cmp = memcmp(a, b, min_len);
    if (cmp != 0)
        return cmp;
    /* 公共前缀相等，较短的键视为较小 */
    if (a_len < b_len)
        return -1;
    if (a_len > b_len)
        return 1;
    return 0;
}
