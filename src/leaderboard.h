// 世界财富排行榜 —— 基于 B+ 树引擎
#ifndef BTREE_LEADERBOARD_H
#define BTREE_LEADERBOARD_H

#include "types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* 不透明排行榜句柄 */
    typedef struct leaderboard leaderboard_t;

    /* 排行榜条目 */
    typedef struct
    {
        char name[64];
        int64_t wealth; /* 单位：亿美元 */
        int rank;
    } lb_entry_t;

    /* ─── 生命周期 ─── */

    /* 创建/打开排行榜。文件不存在则自动创建并初始化种子数据 */
    leaderboard_t *lb_create(const char *path);

    /* 关闭排行榜，持久化 root_id */
    void lb_destroy(leaderboard_t *lb);

    /* ─── 增删改查 ─── */

    /* 添加或更新一个人的财富值 */
    btree_error_t lb_upsert(leaderboard_t *lb, const char *name, int64_t wealth);

    /* 删除 */
    btree_error_t lb_delete(leaderboard_t *lb, const char *name);

    /* 查询个人财富 */
    btree_error_t lb_get(leaderboard_t *lb, const char *name, int64_t *wealth);

    /* ─── 排行查询 ─── */

    /* 查询个人排名（1-based；同财富按姓名升序） */
    int lb_rank(leaderboard_t *lb, const char *name);

    /* 获取前 n 名，按财富降序排列；返回实际写入条数 */
    int lb_top_n(leaderboard_t *lb, int n, lb_entry_t *entries);

    /* 总人数 */
    int lb_count(leaderboard_t *lb);

#ifdef __cplusplus
}
#endif

#endif /* BTREE_LEADERBOARD_H */
