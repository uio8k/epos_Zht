/**
 * vim: filetype=c:fenc=utf-8:ts=4:et:sw=4:sts=4
 *
 * L4 整机自检（P11）：QEMU 冷启动时在 hda_init() 之后跑四项，结果打到串口，
 * 由 tests/full_run.py 的 L4 腿断言。
 *
 * 依据 docs/decisions-audio-test.md:93 与 OSDev Unit_Testing 的 Run-Time
 * Self-Tests：分配器往返、信号量握手、tick 前进、先打印测试名。摘录见
 * docs/evidence/2026-11-11/osdev-unit-testing-excerpt.txt。
 *
 * 三条硬约束（docs/tech-plan-selftest-l4.md §5）：
 *   D6 每行 < 100 字符 —— kernel/printk.c:9 是 buf[128]，且 :17 按
 *      vsnprintf 的「应写长度」循环打印，超长会越界打出垃圾；
 *   D8 自检永不 abort 启动，只打印 FAIL 并计入 RESULT；
 *      每个用例必须有界 —— OSDev 原文提醒这类自检可能挂死。
 */
#include <stddef.h>
#include "kernel.h"
#include "hda_internal.h"

#define SELFTEST_CASES 4

/* tick 用例的自旋上限：HZ=100，一次跳变 10ms；5e7 次读远大于此，
   超限即 FAIL，不让自检挂死。 */
#define SELFTEST_SPIN_MAX  50000000UL

int snprintf(char *str, size_t count, const char *fmt, ...);

static int          g_st_case;
static int          g_st_failed;
static const char  *g_st_name;
static char         g_st_detail[96];

static void st_begin(void)
{
    printk("SELFTEST [%d/%d] %s\r\n", g_st_case, SELFTEST_CASES, g_st_name);
}

static void st_end(int ok)
{
    if (ok)
        printk("SELFTEST [%d/%d] %s: PASS\r\n",
               g_st_case, SELFTEST_CASES, g_st_name);
    else {
        g_st_failed++;
        printk("SELFTEST [%d/%d] %s: FAIL %s\r\n",
               g_st_case, SELFTEST_CASES, g_st_name, g_st_detail);
    }
}

/*
 * 用例 1：分配器往返（OSDev: "ask it for memory, record the address, free
 * the memory and ask for memory again"）。
 *
 * 前半段查「释放后同尺寸再申请拿回同一地址」；后半段是有界的合并冒烟：
 * 连开 8 块 512B、按奇偶分两轮全部释放，再申请 2048B —— 单块只有 512B，
 * 只有相邻块真的合并了才可能满足，且地址必须落回那片被释放的区间，
 * 否则说明用的是别处的空白堆、这条断言就白写了。
 */
static int st_allocator(void)
{
    void *a, *b, *m;
    void *blocks[8];
    uint32_t lo, hi;
    int i, ok, n = 0;

    a = kmalloc(64);
    if (a == NULL) {
        snprintf(g_st_detail, sizeof(g_st_detail), "kmalloc(64) = NULL");
        return 0;
    }
    *(uint32_t *)a = 0xa5a5a5a5;   /* 写一遍：释放不能把堆搞坏 */
    kfree(a);

    b = kmalloc(64);
    ok = (b == a);
    if (!ok)
        snprintf(g_st_detail, sizeof(g_st_detail),
                 "64B: a=0x%08x b=0x%08x", (uint32_t)a, (uint32_t)b);
    if (b != NULL)
        kfree(b);
    if (!ok)
        return 0;

    for (i = 0; i < 8; i++) {
        blocks[i] = kmalloc(512);
        if (blocks[i] != NULL)
            n++;
    }
    if (n != 8) {
        snprintf(g_st_detail, sizeof(g_st_detail), "only %d/8 x 512B", n);
        for (i = 0; i < 8; i++)
            if (blocks[i] != NULL)
                kfree(blocks[i]);
        return 0;
    }
    lo = (uint32_t)blocks[0];
    hi = (uint32_t)blocks[7] + 512;
    for (i = 0; i < 8; i += 2)
        kfree(blocks[i]);
    for (i = 1; i < 8; i += 2)
        kfree(blocks[i]);

    m = kmalloc(2048);
    ok = (m != NULL) && ((uint32_t)m >= lo) && ((uint32_t)m < hi);
    if (!ok)
        snprintf(g_st_detail, sizeof(g_st_detail),
                 "merge: m=0x%08x not in [0x%08x,0x%08x)",
                 (uint32_t)m, lo, hi);
    if (m != NULL)
        kfree(m);

    return ok;
}

/*
 * 用例 2：信号量握手（OSDev: "launch a thread that is expected to signal a
 * semaphore and then wait on that semaphore"）。
 *
 * sys_task_create(0, ...) 建内核线程（tos=0），线程 signal 后 sys_task_exit，
 * 主线程 sem_wait 等到后再 sys_task_wait 回收 tcb。后半段用初值 2 验计数
 * 语义（两次 wait 都不阻塞），不测「第三次阻塞」——那需要构造超时。
 */
static volatile int g_st_sem_rc;

static void st_sem_thread(void *pv)
{
    void *sem = pv;

    /* 先写标志再 signal：signal 之后本线程随时可能被抢占，主线程一旦被
       唤醒就要能读到已经写好的值，否则这条断言会时好时坏。 */
    g_st_sem_rc = 1;
    if (sem_signal(sem) != 0)
        g_st_sem_rc = -1;
    sys_task_exit(0);
}

static int st_semaphore(void)
{
    void *sem;
    struct tcb *tsk;
    int code = -1, rc, ok;

    sem = sem_create(0);
    if (sem == NULL) {
        snprintf(g_st_detail, sizeof(g_st_detail), "sem_create(0) = NULL");
        return 0;
    }
    tsk = sys_task_create(0, st_sem_thread, sem);
    if (tsk == NULL) {
        sem_destroy(sem);
        snprintf(g_st_detail, sizeof(g_st_detail), "task_create = NULL");
        return 0;
    }

    g_st_sem_rc = 0;
    rc = sem_wait(sem);
    ok = (rc == 0) && (g_st_sem_rc == 1);
    sys_task_wait(tsk->tid, &code);
    ok = ok && (code == 0);
    sem_destroy(sem);
    if (!ok)
        snprintf(g_st_detail, sizeof(g_st_detail),
                 "wait rc=%d sig=%d code=%d", rc, g_st_sem_rc, code);

    sem = sem_create(2);
    if (sem == NULL) {
        snprintf(g_st_detail, sizeof(g_st_detail), "sem_create(2) = NULL");
        return 0;
    }
    if (sem_wait(sem) != 0 || sem_wait(sem) != 0) {
        ok = 0;
        snprintf(g_st_detail, sizeof(g_st_detail), "counting wait failed");
    }
    sem_destroy(sem);

    return ok;
}

/*
 * 用例 3：tick 前进（OSDev: "poll the tick counter and don't go on until it
 * advanced"）。有界自旋，不用 sleep —— task0 睡下去只剩 idle，路径更绕。
 */
static int st_tick(void)
{
    unsigned t0 = g_timer_ticks;
    unsigned long n = SELFTEST_SPIN_MAX;

    while (n-- > 0)
        if (g_timer_ticks != t0)
            return 1;

    snprintf(g_st_detail, sizeof(g_st_detail),
             "no tick in %lu spins", SELFTEST_SPIN_MAX);
    return 0;
}

/*
 * 用例 4：HDA 探测结果（P11 的覆盖项之一）。只看 hda_init() 填进 g_hda 的
 * 事实：MMIO 映射到位、codec 地址合法、DAC 与 Pin 都找到。
 */
static int st_hda(void)
{
    int ok = (g_hda.mmio_base != 0) &&
             (g_hda.codec_cad <= 14) &&
             (g_hda.output_dac != 0) &&
             (g_hda.output_pin != 0);

    if (!ok)
        snprintf(g_st_detail, sizeof(g_st_detail),
                 "mmio=0x%08x cad=%d dac=%d pin=%d",
                 g_hda.mmio_base, (int)g_hda.codec_cad,
                 (int)g_hda.output_dac, (int)g_hda.output_pin);
    return ok;
}

/**
 * 跑完四项自检，返回失败用例数（0 = 全通过）。永不 abort 启动。
 */
int selftest_run(void)
{
    static const char *names[SELFTEST_CASES] = {
        "allocator", "semaphore", "tick", "hda"
    };
    int results[SELFTEST_CASES];
    int i, passed = 0;

    printk("SELFTEST: begin (%d cases)\r\n", SELFTEST_CASES);

    for (i = 0; i < SELFTEST_CASES; i++) {
        g_st_case = i + 1;
        g_st_name = names[i];
        g_st_detail[0] = 0;
        st_begin();
        switch (i) {
        case 0: results[i] = st_allocator(); break;
        case 1: results[i] = st_semaphore(); break;
        case 2: results[i] = st_tick();      break;
        default: results[i] = st_hda();      break;
        }
        if (results[i])
            passed++;
        st_end(results[i]);
    }

    printk("SELFTEST: RESULT %s %d/%d\r\n",
           (passed == SELFTEST_CASES) ? "PASS" : "FAIL",
           passed, SELFTEST_CASES);

    return g_st_failed;
}
