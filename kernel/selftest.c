/**
 * vim: filetype=c:fenc=utf-8:ts=4:et:sw=4:sts=4
 *
 * L4 整机自检（P11/S04）：QEMU 冷启动时在 hda_init() 之后跑七项（四项
 * 既有 + S04 三个 SMP 用例，selftest_run 前移至 smp_init 之后——AP 已在
 * 线），结果打到串口，由 tests/full_run.py 的 L4 腿断言。
 *
 * 依据 docs/decisions-audio-test.md:93 与 OSDev Unit_Testing 的 Run-Time
 * Self-Tests：分配器往返、信号量握手、tick 前进、先打印测试名。摘录见
 * docs/evidence/2026-11-11/osdev-unit-testing-excerpt.txt；SMP 用例依据
 * docs/evidence/2026-11-14/s04-e2e-research-excerpt.txt §4。
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

#define SELFTEST_CASES 7

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

/*
 * ============ SMP 用例（S04，docs/tech-plan-smp-prototype.md §4.2 E3）============
 *
 * 前置：start_user_task 已把 smp_init 排在 selftest_run 之前，AP 在线
 *（-smp 2 档）。每个用例先查 smp_online_count()——AP offline 时快速
 * FAIL（绑核线程永不运行，不能等 task_wait 挂死，D8 有界）。
 */

/* AP offline 的统一快速失败 */
static int st_smp_ready(void)
{
    if (smp_online_count() >= 2)
        return 1;
    snprintf(g_st_detail, sizeof(g_st_detail), "ap offline (%d)",
             smp_online_count());
    return 0;
}

/* 本核 APIC ID（selftest 侧简化读法，语义同 cpu.h lapic_id） */
static int st_my_cpu(void)
{
    return (int)((*(volatile uint32_t *)0xFEE00020) >> 24);
}

/*
 * 用例 5：双核并行计数 + U5 FPU 跨核断言。
 *
 * 绑 cpu0/cpu1 的两个内核线程各自无锁自增私有计数到 N——-smp 1 下 bind-1
 * 线程永不运行，用例不可能假绿。cpu1 线程内嵌 U5：做一次 x87 浮点乘
 *（switch_to 每次置 TS → 首条 x87 触发 #NM → machdep 处理器恢复），
 * 2.0×2×2×2=16.0 精确可表示，值自洽即 FPU 状态跨核自洽。
 */
#define SMP_ST_N 20000

static volatile int g_st_dual_cnt[2];   /* 各线程私有计数 */
static volatile int g_st_dual_cpu[2];   /* 各线程自报的 APIC ID */
static volatile int g_st_dual_fpu;      /* cpu1 线程的 x87 结果（1=对） */

static void st_smp_dual_thread(void *pv)
{
    int idx = (int)(uint32_t)pv;
    int i;
    volatile float x = 2.0f;

    g_st_dual_cpu[idx] = st_my_cpu();
    if (idx == 1) {
        for (i = 0; i < 3; i++)
            x *= 2.0f;
        g_st_dual_fpu = ((int)x == 16) ? 1 : -1;
    }
    for (i = 0; i < SMP_ST_N; i++)
        g_st_dual_cnt[idx]++;
    sys_task_exit(0);
}

static int st_smp_dual(void)
{
    struct tcb *ta, *tb;
    int c0 = -1, c1 = -1, ok;

    if (!st_smp_ready())
        return 0;

    g_st_dual_cnt[0] = g_st_dual_cnt[1] = 0;
    g_st_dual_cpu[0] = g_st_dual_cpu[1] = -1;
    g_st_dual_fpu = 0;

    ta = sys_task_create_cpu(0, st_smp_dual_thread, (void *)0);
    tb = sys_task_create_cpu(1, st_smp_dual_thread, (void *)1);
    if (ta == NULL || tb == NULL) {
        snprintf(g_st_detail, sizeof(g_st_detail), "task_create_cpu = NULL");
        return 0;
    }

    sys_task_wait(ta->tid, &c0);
    sys_task_wait(tb->tid, &c1);

    ok = (g_st_dual_cnt[0] == SMP_ST_N) &&
         (g_st_dual_cnt[1] == SMP_ST_N) &&
         (g_st_dual_cpu[0] == 0) &&
         (g_st_dual_cpu[1] == 1) &&
         (g_st_dual_fpu == 1) &&
         (c0 == 0 && c1 == 0);
    printk("SMPTEST dual: cpu0=%d n0=%d cpu1=%d n1=%d fpu=%d\r\n",
           g_st_dual_cpu[0], g_st_dual_cnt[0],
           g_st_dual_cpu[1], g_st_dual_cnt[1], g_st_dual_fpu);
    if (!ok)
        snprintf(g_st_detail, sizeof(g_st_detail),
                 "cnt/cpu/fpu mismatch (see SMPTEST line)");
    return ok;
}

/*
 * 用例 6：锁互斥计数。两线程对同一计数器各加 N 次，sem 保护——锁失效时
 * 跨核竞争必然丢更新（counter < 2N），弄坏必红。
 */
static volatile int g_st_mux_cnt;

static void st_smp_mux_thread(void *pv)
{
    void *sem = pv;
    int i;

    for (i = 0; i < SMP_ST_N / 2; i++) {
        sem_wait(sem);
        g_st_mux_cnt++;
        sem_signal(sem);
    }
    sys_task_exit(0);
}

static int st_smp_mux(void)
{
    void *sem;
    struct tcb *ta, *tb;
    int c0 = -1, c1 = -1, ok;
    const int expect = SMP_ST_N;    /* 2 x (N/2) */

    if (!st_smp_ready())
        return 0;

    sem = sem_create(1);
    if (sem == NULL) {
        snprintf(g_st_detail, sizeof(g_st_detail), "sem_create(1) = NULL");
        return 0;
    }
    g_st_mux_cnt = 0;
    ta = sys_task_create_cpu(0, st_smp_mux_thread, sem);
    tb = sys_task_create_cpu(1, st_smp_mux_thread, sem);
    if (ta == NULL || tb == NULL) {
        sem_destroy(sem);
        snprintf(g_st_detail, sizeof(g_st_detail), "task_create_cpu = NULL");
        return 0;
    }

    sys_task_wait(ta->tid, &c0);
    sys_task_wait(tb->tid, &c1);

    ok = (g_st_mux_cnt == expect) && (c0 == 0 && c1 == 0);
    printk("SMPTEST mutex: counter=%d expect=%d\r\n", g_st_mux_cnt, expect);
    if (!ok)
        snprintf(g_st_detail, sizeof(g_st_detail),
                 "counter=%d expect=%d rc=%d/%d",
                 g_st_mux_cnt, expect, c0, c1);
    sem_destroy(sem);
    return ok;
}

/*
 * 用例 7：跨核信号量唤醒。bind-1 线程 sem_wait 睡眠，主线程（cpu0）确认
 * 其就位后 signal——wake_up 置 READY 无 IPI，AP 在其下一 tick（≤10ms）
 * 拾取；断言线程被唤醒且自报 cpu=1。
 */
static volatile int g_st_cw_state;      /* 1=线程已就位, 2=已被唤醒 */
static volatile int g_st_cw_cpu;
static volatile int g_st_cw_rc;

static void st_smp_cw_thread(void *pv)
{
    void *sem = pv;
    int rc;

    g_st_cw_cpu = st_my_cpu();
    g_st_cw_state = 1;                  /* 先置标志再睡（防主线程早 signal） */
    rc = sem_wait(sem);
    g_st_cw_rc = rc;
    g_st_cw_state = 2;
    sys_task_exit(0);
}

static int st_smp_cw(void)
{
    void *sem;
    struct tcb *tsk;
    unsigned long n = SELFTEST_SPIN_MAX;
    int code = -1, ok;

    if (!st_smp_ready())
        return 0;

    sem = sem_create(0);
    if (sem == NULL) {
        snprintf(g_st_detail, sizeof(g_st_detail), "sem_create(0) = NULL");
        return 0;
    }
    g_st_cw_state = 0;
    g_st_cw_cpu = -1;
    g_st_cw_rc = -1;

    tsk = sys_task_create_cpu(1, st_smp_cw_thread, sem);
    if (tsk == NULL) {
        sem_destroy(sem);
        snprintf(g_st_detail, sizeof(g_st_detail), "task_create_cpu = NULL");
        return 0;
    }

    while (g_st_cw_state != 1 && n-- > 0)
        ;                               /* 有界：等线程就位 */
    if (g_st_cw_state != 1) {
        sem_destroy(sem);
        snprintf(g_st_detail, sizeof(g_st_detail), "thread not parked");
        return 0;
    }

    sem_signal(sem);
    sys_task_wait(tsk->tid, &code);

    ok = (g_st_cw_state == 2) && (g_st_cw_rc == 0) &&
         (g_st_cw_cpu == 1) && (code == 0);
    printk("SMPTEST crosswake: cpu=%d rc=%d code=%d\r\n",
           g_st_cw_cpu, g_st_cw_rc, code);
    if (!ok)
        snprintf(g_st_detail, sizeof(g_st_detail),
                 "state=%d cpu=%d rc=%d code=%d",
                 g_st_cw_state, g_st_cw_cpu, g_st_cw_rc, code);
    sem_destroy(sem);
    return ok;
}

/**
 * 跑完七项自检，返回失败用例数（0 = 全通过）。永不 abort 启动。
 */
int selftest_run(void)
{
    static const char *names[SELFTEST_CASES] = {
        "allocator", "semaphore", "tick", "hda",
        "smp-dual", "smp-mutex", "smp-crosswake"
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
        case 3: results[i] = st_hda();       break;
        case 4: results[i] = st_smp_dual();  break;
        case 5: results[i] = st_smp_mux();   break;
        default: results[i] = st_smp_cw();   break;
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
