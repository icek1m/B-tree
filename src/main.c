// 世界财富排行榜 —— B+ 树引擎应用
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "leaderboard.h"
#include "test.h"

#define DB_PATH "wealth_leaderboard.bt"

/* ─── 辅助：去除字符串末尾换行 ─── */
static void chomp(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
}

/* ─── 辅助：读取一行输入 ─── */
static char *read_line(char *buf, size_t size)
{
    if (!fgets(buf, (int)size, stdin))
        return NULL;
    chomp(buf);
    return buf;
}

/* ─── 打印整数千分位格式 ─── */
static void print_wealth(int64_t wealth)
{
    /* 递归打印千分位 */
    if (wealth < 0) { putchar('-'); wealth = -wealth; }
    if (wealth >= 1000)
    {
        print_wealth(wealth / 1000);
        printf(",%03ld", (long)(wealth % 1000));
    }
    else
    {
        printf("%ld", (long)wealth);
    }
}

/* ════════════════════════════════════════════════════════════════
 *  功能菜单
 * ════════════════════════════════════════════════════════════════ */

static void show_top_n(leaderboard_t *lb)
{
    printf("\n请输入要查看的名次数量 (1-200，默认 10): ");
    char input[16];
    if (!read_line(input, sizeof(input)) || strlen(input) == 0)
        return;

    int n = atoi(input);
    if (n <= 0) n = 10;
    if (n > 200) n = 200;

    lb_entry_t *entries = (lb_entry_t *)malloc((size_t)n * sizeof(lb_entry_t));
    if (!entries)
    {
        printf("内存不足\n");
        return;
    }

    int count = lb_top_n(lb, n, entries);
    if (count == 0)
    {
        printf("排行榜暂无数据\n");
        free(entries);
        return;
    }

    int total = lb_count(lb);

    printf("\n");
    printf("  ╔══════════════════════════════════════════════════════════╗\n");
    printf("  ║              世界财富排行 Top %d                        ║\n", count);
    printf("  ║         共 %d 人  (单位：亿美元)                        ║\n", total);
    printf("  ╚══════════════════════════════════════════════════════════╝\n");
    printf("  %-5s  %-35s  %s\n", "排名", "姓名", "财富");
    printf("  %-5s  %-35s  %s\n", "────", "───────────────────────────────────", "──────────────");
    for (int i = 0; i < count; i++)
    {
        printf("  #%-4d", entries[i].rank);
        /* 姓名左对齐，最多 35 字符 */
        char name_buf[64];
        snprintf(name_buf, sizeof(name_buf), "%s", entries[i].name);
        int pad = 35 - (int)strlen(name_buf);
        if (pad < 0) pad = 0;
        printf("  %s%*s", name_buf, pad, "");
        printf("  ");
        print_wealth(entries[i].wealth);
        printf(" 亿\n");
    }

    free(entries);
}

static void lookup_person(leaderboard_t *lb)
{
    printf("\n请输入姓名: ");
    char name[128];
    if (!read_line(name, sizeof(name)) || strlen(name) == 0)
        return;

    int64_t wealth;
    btree_error_t err = lb_get(lb, name, &wealth);
    if (err != BTREE_OK)
    {
        printf("未找到 \"%s\"\n", name);
        return;
    }

    int rank = lb_rank(lb, name);
    printf("\n  %s\n", name);
    printf("  财富: ");
    print_wealth(wealth);
    printf(" 亿美元\n");
    if (rank > 0)
        printf("  排名: #%d\n", rank);
}

static void add_or_update(leaderboard_t *lb)
{
    printf("\n请输入姓名: ");
    char name[128];
    if (!read_line(name, sizeof(name)) || strlen(name) == 0)
        return;

    printf("请输入财富 (亿美元): ");
    char input[32];
    if (!read_line(input, sizeof(input)) || strlen(input) == 0)
        return;

    char *end = NULL;
    long long val = strtoll(input, &end, 10);
    if (end == input || val < 0)
    {
        printf("无效的财富值\n");
        return;
    }

    int64_t old_wealth;
    btree_error_t err = lb_get(lb, name, &old_wealth);
    if (err == BTREE_OK)
    {
        printf("  %s 原财富: ", name);
        print_wealth(old_wealth);
        printf("亿美元\n");
    }

    lb_upsert(lb, name, (int64_t)val);
    printf("  已更新为: ");
    print_wealth((int64_t)val);
    printf("亿美元\n");
}

static void delete_person(leaderboard_t *lb)
{
    printf("\n请输入要删除的姓名: ");
    char name[128];
    if (!read_line(name, sizeof(name)) || strlen(name) == 0)
        return;

    btree_error_t err = lb_delete(lb, name);
    if (err == BTREE_OK)
        printf("已删除 \"%s\"\n", name);
    else
        printf("未找到 \"%s\"\n", name);
}

static void show_menu(void)
{
    printf("\n");
    printf("  ╔══════════════════════════════╗\n");
    printf("  ║    世界财富排行榜系统        ║\n");
    printf("  ║    World Wealth Top 100      ║\n");
    printf("  ╠══════════════════════════════╣\n");
    printf("  ║  1. 查看前 N 名排行          ║\n");
    printf("  ║  2. 查询个人财富与排名       ║\n");
    printf("  ║  3. 添加 / 更新              ║\n");
    printf("  ║  4. 删除                     ║\n");
    printf("  ║  5. 退出                     ║\n");
    printf("  ╚══════════════════════════════╝\n");
    printf("  请选择 (1-5): ");
}

/* ════════════════════════════════════════════════════════════════
 *  主入口
 * ════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    /* --test 参数运行测试 */
    if (argc > 1 && strcmp(argv[1], "--test") == 0)
    {
        run_all_tests();
        return 0;
    }

    printf("\n正在加载排行榜数据...\n");
    leaderboard_t *lb = lb_create(DB_PATH);
    if (!lb)
    {
        fprintf(stderr, "错误：无法初始化排行榜数据库\n");
        return 1;
    }

    int count = lb_count(lb);
    printf("已加载 %d 条数据\n\n", count);

    while (1)
    {
        show_menu();

        char choice[16];
        if (!read_line(choice, sizeof(choice)))
            continue;

        switch (choice[0])
        {
        case '1':
            show_top_n(lb);
            break;
        case '2':
            lookup_person(lb);
            break;
        case '3':
            add_or_update(lb);
            break;
        case '4':
            delete_person(lb);
            break;
        case '5':
            printf("\n再见！\n");
            lb_destroy(lb);
            return 0;
        default:
            printf("无效选项，请重新选择\n");
            break;
        }
    }
}
