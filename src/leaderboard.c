// 世界财富排行榜实现
#include "leaderboard.h"
#include "btree.h"
#include "storage.h"
#include "comparator.h"
#include "cursor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════
 *  内部结构
 * ════════════════════════════════════════════════════════════════ */

struct leaderboard
{
    btree_storage_t *store;
    btree_t *tree;
};

/* ════════════════════════════════════════════════════════════════
 *  种子数据 —— 世界财富前 100 名（数据来源：公开资料估算，单位：亿美元）
 * ════════════════════════════════════════════════════════════════ */

static const struct
{
    const char *name;
    int64_t wealth;
} seed_data[] = {
    {"Bernard Arnault & family", 2330},
    {"Elon Musk", 1950},
    {"Jeff Bezos", 1940},
    {"Mark Zuckerberg", 1770},
    {"Larry Ellison", 1410},
    {"Warren Buffett", 1330},
    {"Bill Gates", 1280},
    {"Steve Ballmer", 1210},
    {"Mukesh Ambani", 1160},
    {"Larry Page", 1140},
    {"Sergey Brin", 1100},
    {"Michael Bloomberg", 1050},
    {"Steve Jobs (estate)", 1020},  /* 继承人家族合计 */
    {"Jim Walton", 980},
    {"Rob Walton", 970},
    {"Alice Walton", 970},
    {"David Thomson & family", 960},
    {"Julia Koch & family", 950},
    {"Changpeng Zhao", 940},
    {"Zhong Shanshan", 920},
    {"Francoise Bettencourt Meyers", 910},
    {"Mackenzie Scott", 900},
    {"Gautam Adani", 890},
    {"Zhang Yiming", 880},
    {"Zhuang Ziyuan", 870},
    {"Ma Huateng", 860},
    {"Jack Ma", 850},
    {"Phil Knight & family", 840},
    {"Michael Dell", 830},
    {"Colin Huang", 820},
    {"Ken Griffin", 810},
    {"Li Ka-shing", 800},
    {"He Xiangjian & family", 790},
    {"Daniel Gilbert", 780},
    {"Jensen Huang", 770},
    {"Jim Ratcliffe", 760},
    {"Thomas Peterffy", 750},
    {"Gina Rinehart", 740},
    {"Miriam Adelson & family", 730},
    {"Leonardo Del Vecchio (family)", 720},
    {"Reinhold Wuerth", 710},
    {"Klaus-Michael Kuehne", 700},
    {"Tadashi Yanai & family", 690},
    {"John Menard Jr.", 680},
    {"David Cheriton", 670},
    {"James Simons", 660},
    {"Dustin Moskovitz", 650},
    {"Eric Schmidt", 640},
    {"Vladimir Potanin", 630},
    {"Wang Wei", 620},
    {"Liu Qiangdong", 610},
    {"Lei Jun", 600},
    {"Robin Zeng", 590},
    {"Carlos Slim Helu & family", 580},
    {"Vladimir Lisin", 570},
    {"Leonid Mikhelson", 560},
    {"Amancio Ortega", 550},
    {"Alexey Mordashov", 540},
    {"Gennady Timchenko", 530},
    {"Pavel Durov", 520},
    {"Alisher Usmanov", 510},
    {"Mikhail Fridman", 500},
    {"Viktor Vekselberg", 490},
    {"Andrey Melnichenko", 480},
    {"German Khan", 470},
    {"Peter Kellogg", 460},
    {"Abigail Johnson", 450},
    {"Charles Koch", 440},
    {"David Koch (estate)", 430},
    {"Stephen Ross", 420},
    {"Donald Bren", 410},
    {"Samuel Newhouse Jr. (family)", 400},
    {"Rupert Murdoch & family", 390},
    {"Laurene Powell Jobs", 380},
    {"Marc Benioff", 370},
    {"Jack Dorsey", 360},
    {"Brian Armstrong", 350},
    {"Sam Altman", 340},
    {"Patrick Collison", 330},
    {"John Collison", 320},
    {"Nirav Tolia", 310},
    {"Travis Kalanick", 300},
    {"Garrett Camp", 290},
    {"Markus Persson", 280},
    {"Palmer Luckey", 270},
    {"Bobby Murphy", 260},
    {"Evan Spiegel", 250},
    {"Arash Ferdowsi", 240},
    {"Drew Houston", 230},
    {"Daniel Ek", 220},
    {"Martin Lorentzon", 210},
    {"Niklas Zennstrom", 200},
    {"Matt Mullenweg", 195},
    {"David Baszucki", 190},
    {"Alexandre Arnault", 185},
    {"Frederic Arnault", 180},
    {"Ernest Arnault", 175},
    {"Jean Arnault", 170},
    {"Wang Xing", 165},
    {"Huang Zheng", 160},
    {"Ding Lei", 155},
    {"Shen Nanpeng", 150},
};

#define SEED_COUNT ((int)(sizeof(seed_data) / sizeof(seed_data[0])))

/* ════════════════════════════════════════════════════════════════
 *  比较器：按 qsort 需求，财富降序，同财富姓名升序
 * ════════════════════════════════════════════════════════════════ */

static int entry_cmp_desc(const void *a, const void *b)
{
    const lb_entry_t *ea = (const lb_entry_t *)a;
    const lb_entry_t *eb = (const lb_entry_t *)b;
    if (ea->wealth > eb->wealth) return -1;
    if (ea->wealth < eb->wealth) return  1;
    return strcmp(ea->name, eb->name);
}

/* ════════════════════════════════════════════════════════════════
 *  生命周期
 * ════════════════════════════════════════════════════════════════ */

static void seed_wealth_data(btree_t *tree)
{
    for (int i = 0; i < SEED_COUNT; i++)
    {
        btree_put(tree,
                  (const uint8_t *)seed_data[i].name,
                  (uint16_t)strlen(seed_data[i].name),
                  (const uint8_t *)&seed_data[i].wealth,
                  sizeof(int64_t));
    }
}

leaderboard_t *lb_create(const char *path)
{
    btree_storage_t *store = btree_storage_open(path);
    int is_new = 0;

    if (!store)
    {
        store = btree_storage_create(path);
        if (!store)
            return NULL;
        is_new = 1;
    }

    btree_t *tree;
    if (is_new)
    {
        tree = btree_create(compare_default,
                            btree_storage_read,
                            btree_storage_write,
                            btree_storage_alloc,
                            store);
        if (!tree)
        {
            btree_storage_close(store);
            return NULL;
        }
        seed_wealth_data(tree);
        btree_storage_set_root_id(store, btree_root_id(tree));
    }
    else
    {
        page_id_t root;
        if (btree_storage_get_root_id(store, &root) != BTREE_OK)
        {
            btree_storage_close(store);
            return NULL;
        }
        tree = btree_open(compare_default,
                          btree_storage_read,
                          btree_storage_write,
                          btree_storage_alloc,
                          store, root);
        if (!tree)
        {
            btree_storage_close(store);
            return NULL;
        }
    }

    leaderboard_t *lb = (leaderboard_t *)malloc(sizeof(leaderboard_t));
    if (!lb)
    {
        btree_destroy(tree);
        btree_storage_close(store);
        return NULL;
    }

    lb->store = store;
    lb->tree  = tree;
    return lb;
}

void lb_destroy(leaderboard_t *lb)
{
    if (!lb) return;
    btree_storage_set_root_id(lb->store, btree_root_id(lb->tree));
    btree_destroy(lb->tree);
    btree_storage_close(lb->store);
    free(lb);
}

/* ════════════════════════════════════════════════════════════════
 *  增删改
 * ════════════════════════════════════════════════════════════════ */

btree_error_t lb_upsert(leaderboard_t *lb, const char *name, int64_t wealth)
{
    btree_error_t err = btree_put(lb->tree,
                                   (const uint8_t *)name, (uint16_t)strlen(name),
                                   (const uint8_t *)&wealth, sizeof(int64_t));
    if (err == BTREE_OK)
        btree_storage_set_root_id(lb->store, btree_root_id(lb->tree));
    return err;
}

btree_error_t lb_delete(leaderboard_t *lb, const char *name)
{
    btree_error_t err = btree_delete(lb->tree,
                                      (const uint8_t *)name, (uint16_t)strlen(name));
    if (err == BTREE_OK)
        btree_storage_set_root_id(lb->store, btree_root_id(lb->tree));
    return err;
}

btree_error_t lb_get(leaderboard_t *lb, const char *name, int64_t *wealth)
{
    uint8_t buf[sizeof(int64_t)];
    uint16_t len = sizeof(buf);
    btree_error_t err = btree_get(lb->tree,
                                   (const uint8_t *)name, (uint16_t)strlen(name),
                                   buf, &len);
    if (err == BTREE_OK && len == sizeof(int64_t))
        memcpy(wealth, buf, sizeof(int64_t));
    return err;
}

/* ════════════════════════════════════════════════════════════════
 *  排行查询
 * ════════════════════════════════════════════════════════════════ */

int lb_count(leaderboard_t *lb)
{
    btree_cursor_t *cur = btree_cursor_create(lb->tree);
    if (!cur) return 0;

    int count = 0;
    btree_error_t err = btree_cursor_first(cur);
    while (err == BTREE_OK && btree_cursor_valid(cur))
    {
        count++;
        err = btree_cursor_next(cur);
    }
    btree_cursor_destroy(cur);
    return count;
}

static int collect_all(lb_entry_t *buf, int cap, btree_t *tree)
{
    btree_cursor_t *cur = btree_cursor_create(tree);
    if (!cur) return 0;

    int n = 0;
    uint8_t key[256], val[sizeof(int64_t)];
    uint16_t klen, vlen;

    btree_error_t err = btree_cursor_first(cur);
    while (err == BTREE_OK && btree_cursor_valid(cur) && n < cap)
    {
        klen = sizeof(key);
        vlen = sizeof(val);
        if (btree_cursor_get(cur, key, &klen, val, &vlen) == BTREE_OK
            && klen < (uint16_t)sizeof(((lb_entry_t *)0)->name))
        {
            memcpy(buf[n].name, key, klen);
            buf[n].name[klen] = '\0';
            if (vlen == sizeof(int64_t))
                memcpy(&buf[n].wealth, val, sizeof(int64_t));
            else
                buf[n].wealth = 0;
            n++;
        }
        err = btree_cursor_next(cur);
    }
    btree_cursor_destroy(cur);
    return n;
}

int lb_top_n(leaderboard_t *lb, int n, lb_entry_t *entries)
{
    int total = lb_count(lb);
    if (total == 0) return 0;

    lb_entry_t *all = (lb_entry_t *)malloc((size_t)total * sizeof(lb_entry_t));
    if (!all) return 0;

    int got = collect_all(all, total, lb->tree);
    (void)got;

    qsort(all, (size_t)total, sizeof(lb_entry_t), entry_cmp_desc);

    int ret = n < total ? n : total;
    for (int i = 0; i < ret; i++)
    {
        memcpy(&entries[i], &all[i], sizeof(lb_entry_t));
        entries[i].rank = i + 1;
    }

    free(all);
    return ret;
}

int lb_rank(leaderboard_t *lb, const char *name)
{
    int64_t target_wealth;
    if (lb_get(lb, name, &target_wealth) != BTREE_OK)
        return -1;

    int total = lb_count(lb);
    if (total == 0) return -1;

    lb_entry_t *all = (lb_entry_t *)malloc((size_t)total * sizeof(lb_entry_t));
    if (!all) return -1;

    collect_all(all, total, lb->tree);
    qsort(all, (size_t)total, sizeof(lb_entry_t), entry_cmp_desc);

    int rank = -1;
    for (int i = 0; i < total; i++)
    {
        if (all[i].wealth == target_wealth && strcmp(all[i].name, name) == 0)
        {
            rank = i + 1;
            break;
        }
    }

    free(all);
    return rank;
}
