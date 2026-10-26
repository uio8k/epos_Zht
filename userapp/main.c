/*
 * vim: filetype=c:fenc=utf-8:ts=4:et:sw=4:sts=4
 *
 * 两种演示模式（构建时选择，见 userapp/Makefile）：
 *   默认      -> 整曲流式播放（audio_play_file_stream_ms 分块读 song.wav）
 *   SORT_DEMO -> VGA 8-bit 图形模式 + 希尔排序柱状图动态演示（旧演示，保留）
 *
 * 通过 serial.log 判断程序是否成功：
 *   [SONG]/[AUDIO] 行 -> 歌曲播放；underruns: 0 -> 流式播放无断供
 *   [VGA]/[SORT] 行   -> SORT_DEMO 旧演示的图形与排序判读依据
 */
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <syscall.h>
#include <stdlib.h>
#include <time.h>
#ifdef TEST_CAPTURE
#include <audio_capture.h>
#endif
#include "graphics.h"
#include "melody_player.h"

extern int vm86call(int fintr, uint32_t n, struct vm86_context *vm86ctx);

extern void *tlsf_create_with_pool(void *mem, size_t bytes);
extern void *g_heap;

#ifdef SORT_DEMO
/* ================= 排序/显示参数 ================= */
#define N           64          /* 待排序元素个数（扩充以支撑完整旋律长度） */
#define BAR_MIN     20          /* 数值下限 */
#define BAR_MAX     200         /* 数值上限 */
/* 柱高缩放改为动态计算 bar_scale()（见下方），保证不同分辨率下柱子填满屏幕 */

/* VBE 8-bit(256色) 模式候选 */
static const int g_modes[] = { 0x101, /* 640x480x8 */
                               0x103, /* 800x600x8 */
                               0x100, /* 640x400x8 */
                               0x105  /* 1024x768x8 */
                             };
#define N_MODES (int)(sizeof(g_modes)/sizeof(g_modes[0]))

/* 调色板索引 */
#define COL_BG      0   /* 黑 */
#define COL_WHITE   1   /* 白 */
#define COL_GREEN   2   /* 绿 */
#define COL_RED     3   /* 红 */
#define COL_YELLOW  4   /* 黄 */
#define COL_BLUE    5   /* 蓝 */
#define COL_GRAD_START 6   /* 渐变起始索引 */
#define COL_GRAD_END   250 /* 渐变结束索引 */

static int g_gfx_ok = 0;    /* 图形模式是否进入成功 */
static int a[N];            /* 待排序数组 */
static int g_steps = 0;     /* 已执行的操作数 */

/* ================= 简单伪随机数 ================= */
static uint32_t g_seed = 0x12345678;
static uint32_t next_rand(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 16) & 0x7fff;
}

/* ================= 256 色渐变调色板 ================= */
/* t in [0,1000): 蓝->青->绿->黄->红 */
static void rgb_from_t(int t, int *r, int *g, int *b)
{
    if (t < 200)      { *r=0;   *g=255*t/200;      *b=255; }
    else if (t < 400) { *r=0;   *g=255;            *b=255-255*(t-200)/200; }
    else if (t < 600) { *r=255*(t-400)/200; *g=255; *b=0; }
    else if (t < 800) { *r=255; *g=255-255*(t-600)/200; *b=0; }
    else              { *r=255; *g=0;              *b=255*(t-800)/200; }
}

static void set_palette(void)
{
    /* 把 256 项 RGB 表写到 vm86_init() 已映射为可写的低内存 0xE8000
       （QEMU 中 0xE8000-0xEFFFF 是 pc.ram，且 vm86.c 已 L2E_V|L2E_W|L2E_U 映射），
       再通过 vm86 int 0x10 AX=0x1002 写入 VGA DAC。
       注意：不能用 mmap(0x60000)，因为 vm86 仿真器按线性地址访问，
       0x60000 未映射会导致页错误。 */
    uint8_t *pal = (uint8_t *)0xE8000;
    for (int i = 0; i < 256; i++) {
        int r = 0, g = 0, b = 0;
        if (i == COL_BG)         { r=0;   g=0;   b=0; }
        else if (i == COL_WHITE) { r=255; g=255; b=255; }
        else if (i == COL_GREEN) { r=0;   g=255; b=0; }
        else if (i == COL_RED)   { r=255; g=0;   b=0; }
        else if (i == COL_YELLOW){ r=255; g=255; b=0; }
        else if (i == COL_BLUE)  { r=0;   g=0;   b=255; }
        else {
            int t = (i - COL_GRAD_START) * 1000 /
                    (COL_GRAD_END - COL_GRAD_START);
            rgb_from_t(t, &r, &g, &b);
        }
        pal[i*3+0] = (uint8_t)(r >> 2);   /* VGA DAC 是 6-bit */
        pal[i*3+1] = (uint8_t)(g >> 2);
        pal[i*3+2] = (uint8_t)(b >> 2);
    }
    struct vm86_context vm86ctx = { .ss = 0x0000, .esp = 0x1000 };
    vm86ctx.eax = 0x1002;      /* 设置全部 256 个调色板寄存器 */
    vm86ctx.es  = 0xE800;
    vm86ctx.edx = 0x0000;
    if (vm86call(1, 0x10, &vm86ctx) != 0)
        printf("[VGA] palette set via vm86 FAILED\r\n");
    else
        printf("[VGA] 256-color palette set OK\r\n");
}

/* ================= 进入 VGA 8-bit 图形模式 ================= */
static int enter_vga8(void)
{
    printf("[VGA] Entering VGA 8-bit (256-color) graphics mode...\r\n");
    for (int m = 0; m < N_MODES; m++) {
        int mode = g_modes[m];
        printf("[VGA] trying VBE mode 0x%04x ... ", mode);
        if (init_graphic(mode) == 0) {
            printf("OK (%dx%dx%d)\r\n",
                   g_graphic_dev.XResolution,
                   g_graphic_dev.YResolution,
                   g_graphic_dev.BitsPerPixel);
            set_palette();
            printf("[VGA] FrameBuffer=0x%08x size=%u bytes\r\n",
                   (uint32_t)g_graphic_dev.FrameBuffer,
                   g_graphic_dev.FrameBufferSize);
            return 0;
        }
        printf("failed\r\n");
    }
    printf("[VGA] No 8-bit graphics mode available!\r\n");
    return -1;
}

/* ================= 柱状图绘制 ================= */
static int bar_width(void)
{
    return g_graphic_dev.XResolution / N;
}

/* 动态柱高缩放：让最高柱（BAR_MAX）约占屏幕高度的 90%，适配不同分辨率 */
static int bar_scale(void)
{
    int s = (g_graphic_dev.YResolution - 24) / BAR_MAX;
    return s < 1 ? 1 : s;
}

/* 用指定颜色画第 i 根柱子 */
static void draw_bar(int i, COLORREF cr)
{
    int w = bar_width();
    int x0 = i * w;
    int x1 = (i + 1) * w - 1;
    int h  = a[i] * bar_scale();
    int y0 = g_graphic_dev.YResolution - 1 - h;
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y < g_graphic_dev.YResolution; y++)
            setPixel(x, y, cr);
}

/* 清空第 i 根柱子的列（画背景色） */
static void clear_bar(int i)
{
    int w = bar_width();
    int x0 = i * w;
    int x1 = (i + 1) * w - 1;
    for (int x = x0; x <= x1; x++)
        for (int y = 0; y < g_graphic_dev.YResolution; y++)
            setPixel(x, y, COL_BG);
}

/* 按数值给柱子一个渐变颜色 */
static COLORREF bar_color(int value)
{
    int t = (value - BAR_MIN) * (COL_GRAD_END - COL_GRAD_START) /
            (BAR_MAX - BAR_MIN);
    return (COLORREF)(COL_GRAD_START + t);
}

/* 重画第 i 根柱子（先清后画，正常颜色） */
static void redraw_bar(int i)
{
    clear_bar(i);
    draw_bar(i, bar_color(a[i]));
}

/* 画全部柱子（初始画面） */
static void draw_all_bars(void)
{
    for (int i = 0; i < N; i++)
        draw_bar(i, bar_color(a[i]));
}

/* ================= 数组打印（serial.log 判读依据） ================= */
static void print_array(const char *tag)
{
    printf("[SORT] %-10s step=%3d : ", tag, g_steps);
    for (int i = 0; i < N; i++)
        printf("%d ", a[i]);
    printf("\r\n");
}

/* ================= 希尔排序 ================= */
static void shell_sort(void)
{
    printf("[SORT] Shell sort: N=%d, value range [%d,%d]\r\n", N, BAR_MIN, BAR_MAX);
    print_array("initial");

    if (g_gfx_ok)
        draw_all_bars();

    for (int gap = N / 2; gap > 0; gap /= 2) {
        printf("[SORT] ---- gap = %d ----\r\n", gap);
        for (int i = gap; i < N; i++) {
            int tmp = a[i];
            int j = i;
            while (j >= gap && a[j - gap] > tmp) {
                /* 前移大数：a[j] = a[j-gap] */
                a[j] = a[j - gap];
                g_steps++;
                if (g_gfx_ok) {
                    clear_bar(j);
                    draw_bar(j, COL_YELLOW);   /* 高亮被移动的位置 */
                }
                print_array("move");
                trigger_next_sort_note();      /* 每次 swap 播放下一个音符（停留==播放时长） */
                if (g_gfx_ok)
                    redraw_bar(j);
                j -= gap;
            }
            if (a[j] != tmp) {
                /* 把 tmp 放到正确位置 */
                a[j] = tmp;
                g_steps++;
                if (g_gfx_ok) {
                    clear_bar(j);
                    draw_bar(j, COL_RED);      /* 高亮新放置的位置 */
                }
                print_array("insert");
                if (g_gfx_ok)
                    redraw_bar(j);
            }
        }
    }

    printf("[SORT] ---- done in %d steps ----\r\n", g_steps);
    print_array("sorted");

    /* 校验是否升序 */
    int ok = 1;
    for (int i = 1; i < N; i++)
        if (a[i] < a[i - 1]) { ok = 0; break; }
    printf("[SORT] verify: array is ascending = %s\r\n", ok ? "OK" : "FAIL");
}
#endif /* SORT_DEMO */

/* ================= 测试信号（音频质量评估用） ================= */
#ifdef TEST_MODE
/* 生成已知参考信号 -> 走完整 HDA DMA 路径 -> 由 tools/eval_audio.py 做 FFT/
   THD/SNR/click 分析。信号在编译期选择（TEST_SIGNAL_*）：
     sine     : 440Hz 纯音，THD/SNR/频率准确度测量的核心信号
     impulse  : 单样本冲激，检测频率响应平坦度与缓冲边界 click
     step     : 中段 DC 阶跃，检测增益变化处的瞬态
     sequence : 8 段 200ms 短音（独立调用），暴露音符边界 click
   满音量(100 = Q8 1.0)播放，避免软件增益缩放引入测量误差。 */

/* 生成 440Hz 带淡入淡出的立体声短音（tempo/underrun/sequence 共用）：
   返回 malloc 的 PCM（bn*2*2 字节），失败返回 NULL。 */
static int16_t *make_tone(int rate, int note_ms, int fade_ms)
{
    const int bn = rate * note_ms / 1000;
    const int fade = rate * fade_ms / 1000;
    int16_t *pcm = (int16_t *)malloc(bn * 2 * sizeof(int16_t));

    if (pcm == NULL)
        return NULL;

    for (int i = 0; i < bn; i++) {
        double env = 1.0;
        if (i < fade)
            env = (double)i / fade;
        else if (i >= bn - fade)
            env = (double)(bn - 1 - i) / fade;
        double s = sin(2.0 * M_PI * 440.0 * (double)i / rate) * 30000.0 * env;
        pcm[i * 2]     = (int16_t)s;
        pcm[i * 2 + 1] = (int16_t)s;
    }
    return pcm;
}

static void run_test_signal(void)
{
    const int rate = 44100, dur_ms = 2000;

#ifdef TEST_SIGNAL_tempo
    /* 节拍抖动测试：连续播放等长标准音符（每次独立提交），打印每次提交的
       相对时刻与间隔。改造前：play 返回时机 = 音符播完 + 用户程序开销，
       间隔偏大且抖动大；改造后：驱动预留安全余量、音符严格首尾相接，
       间隔应稳定在「音符时值」附近。eval_audio.py 据此计算间隔方差。 */
    {
        const int note_ms = 200;
        const int bn = rate * note_ms / 1000;
        const int reps = 40;
        int16_t *pcm = make_tone(rate, note_ms, 5);

        if (pcm == NULL) {
            printf("[TEMPO] pcm alloc failed\r\n");
            return;
        }

        audio_set_volume(100);
        printf("[TEST] tempo %d notes x %d ms\r\n", reps, note_ms);

        struct timeval t0, tv, prev;
        gettimeofday(&t0, NULL);
        prev = t0;
        for (int b = 0; b < reps; b++) {
            audio_play_pcm_ms(pcm, bn * 4, rate, 2, 16, note_ms);
            gettimeofday(&tv, NULL);
            printf("[TEMPO] #%d t=%u interval=%u\r\n", b,
                   (uint32_t)((tv.tv_sec - t0.tv_sec) * 1000 +
                              (tv.tv_usec - t0.tv_usec) / 1000),
                   (uint32_t)((tv.tv_sec - prev.tv_sec) * 1000 +
                              (tv.tv_usec - prev.tv_usec) / 1000));
            prev = tv;
        }

        free(pcm);
        audio_stop();   /* 内部自动导出 [TRACE] 调用链日志 */
        printf("[TEMPO] done, underruns=%u\r\n", audio_get_underruns(0));

        printf("========================================\r\n");
        printf("=== Test tone finished. Check serial.log\r\n");
        printf("========================================\r\n");
        return;
    }
#endif /* TEST_SIGNAL_tempo */

#ifdef TEST_SIGNAL_underrun
    /* underrun 注入测试：音符之间人为停顿超过驱动安全余量（约 93ms）的
       时间，迫使 DMA 越过真实数据末尾。验证三件事：
         ① underrun 被计数（audio_get_underruns）
         ② 越界区间输出为静音（预填静音生效）而非重复旧数据
         ③ [TRACE] 日志含 UNDERRUN 事件（stop 时自动导出） */
    {
        const int note_ms = 200;
        const int bn = rate * note_ms / 1000;
        const int reps = 6;
        int16_t *pcm = make_tone(rate, note_ms, 5);

        if (pcm == NULL) {
            printf("[UNDERRUN] pcm alloc failed\r\n");
            return;
        }

        audio_set_volume(100);
        printf("[TEST] underrun injection, %d notes with stall\r\n", reps);

        for (int b = 0; b < reps; b++) {
            audio_play_pcm_ms(pcm, bn * 4, rate, 2, 16, note_ms);
            if (b == 1) {
                /* 停顿 800ms：超过安全余量（约 93ms）且超过预填前瞻（500ms），
                   覆盖 fill_ahead 在 underrun 后必须持续预填静音的路径 */
                struct timeval t0, tv;
                printf("[UNDERRUN] injecting 800 ms stall\r\n");
                gettimeofday(&t0, NULL);
                do {
                    gettimeofday(&tv, NULL);
                } while ((tv.tv_sec - t0.tv_sec) * 1000 +
                         (tv.tv_usec - t0.tv_usec) / 1000 < 800);
            }
        }

        free(pcm);
        audio_stop();
        printf("[UNDERRUN] detected=%u\r\n", audio_get_underruns(0));

        printf("========================================\r\n");
        printf("=== Test tone finished. Check serial.log\r\n");
        printf("========================================\r\n");
        return;
    }
#endif /* TEST_SIGNAL_underrun */


    const int n = rate * dur_ms / 1000;             /* 总采样数 */
    int16_t *pcm = (int16_t *)malloc(n * 2 * sizeof(int16_t));

    if (pcm == NULL) {
        printf("[TEST] pcm alloc failed\r\n");
        return;
    }

    audio_set_volume(100);
    printf("[TEST] volume set to 100\r\n");

#ifdef TEST_SIGNAL_sequence
    /* 音符序列：8 次独立调用播放 200ms 短音。与音符播放共用同一连续流追加
       路径，用于暴露音符边界 click。每段自带 5ms 淡入淡出，故参考信号
       本身不含阶跃。 */
    {
        const int bn = rate * 200 / 1000;
        printf("[TEST] sequence 8x200ms bursts\r\n");
        int16_t *tone = make_tone(rate, 200, 5);
        if (tone == NULL) {
            printf("[TEST] tone alloc failed\r\n");
        } else {
            for (int b = 0; b < 8; b++)
                audio_play_pcm_ms(tone, bn * 4, rate, 2, 16, 200);
            free(tone);
        }
    }
#else
#  if defined(TEST_SIGNAL_impulse)
    printf("[TEST] impulse, %dms\r\n", dur_ms);
    memset(pcm, 0, n * 2 * sizeof(int16_t));
    pcm[0] = 30000;
    pcm[1] = 30000;
#  elif defined(TEST_SIGNAL_step)
    printf("[TEST] step response, %dms\r\n", dur_ms);
    memset(pcm, 0, n * 2 * sizeof(int16_t));
    {
        /* 中段 1/3 为 DC 平台，边沿加 5ms 线性斜坡 —— 参考信号本身不含阶跃，
           这样在边沿附近检出的 click 才能归因于驱动而非测试信号。 */
        const int ramp = rate * 5 / 1000;
        const int s0 = n / 3;
        const int s1 = 2 * (n / 3);
        for (int i = s0; i < s1; i++) {
            int dist = i - s0;
            if (s1 - 1 - i < dist)
                dist = s1 - 1 - i;
            int16_t v = (dist >= ramp) ? 16000 : (int16_t)(16000 * dist / ramp);
            pcm[i * 2]     = v;
            pcm[i * 2 + 1] = v;
        }
    }
#  else
    printf("[TEST] sine 440Hz, %dms\r\n", dur_ms);
    for (int i = 0; i < n; i++) {
        double s = sin(2.0 * M_PI * 440.0 * (double)i / rate) * 30000.0;
        pcm[i * 2]     = (int16_t)s;
        pcm[i * 2 + 1] = (int16_t)s;
    }
#  endif
    audio_play_pcm_ms(pcm, n * 4, rate, 2, 16, dur_ms);
#endif

    free(pcm);
    audio_stop();

    uint32_t errs = audio_get_errors();
    uint32_t pos  = audio_get_position_ms();
    printf("[AUDIO] errors: fifo=%u desc=%u  position: %u ms\r\n",
           errs & 0xFFFF, errs >> 16, pos);

    printf("========================================\r\n");
    printf("=== Test tone finished. Check serial.log\r\n");
    printf("========================================\r\n");
}
#endif /* TEST_MODE */

#ifdef TEST_CAPTURE
/* ================= 麦克风采集自测 =================
   三个场景一次跑完，覆盖计划里要求的验收面：
     A 1s 采集   —— 通路是否在搬数据，且期间由客户机自己播 400ms 1kHz 正弦，
                    用 Goertzel 判 1kHz 能量比确认「采到的就是自己放的那段声音」
     B 3s 采集   —— 数据量 529KB > 256KB 基准环，触发 512KB 缓冲与 LPIB 绕回
     C 重复 start —— 同参数幂等返回 0，异参数被拒绝返回 -2
   激励从客户机侧发出（DAC -> 板载声卡 -> 立体声混音 -> ADC），整条回环在
   QEMU 内部闭环，不依赖宿主播放器（winsound 那一路实测不稳定）；宿主只需把
   默认输出/输入分别指向板载声卡与立体声混音。 */
#define CAP_RATE    44100
#define CAP_CH      2
#define CAP_BITS    16
#define CAP_CHUNK   8192
#define CAP_POLL_MS 20

/* 包络分块与有声判定：50ms 一块（44100Hz → 2205 帧）。
   peak >= CAP_ACTIVE_PEAK（约 -44dBFS）算「有声块」，用于区分
   宿主喂了声音 / 只有底噪两种情形。
   频率判据两条并用：
     - 过零率估频（廉价、对噪声敏感，仅作参考）：freq = zc×10；
     - Goertzel 单频能量比（主判据）：tone = 块内 1kHz 分量能量 / 总能量，
       纯 1kHz 正弦 ≈ 100%，底噪或其它声音显著偏低。与增益无关，
       不受宿主音量、Stereo Mix 电平影响。 */
#define CAP_BLOCK_MS    50
#define CAP_BLOCK_FRM   (CAP_RATE / 1000 * CAP_BLOCK_MS)
/* 有声门限：实测「板载渲染 -> 立体声混音 -> dsound 采集」这条回环链路衰减
   约 50dB（满刻度激励只落到 ±100~200 LSB），所以门限取 20 而不是常规的 -44dBFS。
   真正的判据是与增益无关的 Goertzel 能量比，这个门限只用来排除纯零块。 */
#define CAP_ACTIVE_PEAK 20
#define CAP_TONE_HZ     1000
/* 2·cos(2π·1000/44100) = 1.9797526，Goertzel 系数 */
#define CAP_TONE_COEF   1.9797526

struct cap_meter {
    uint32_t frames;        /* 累计帧数（一帧 = 左右各一个 16bit 样本） */
    uint32_t zc;            /* 累计过零次数（只取左声道） */
    int32_t  prev;          /* 上一个左声道样本 */
    uint32_t blk_frames;    /* 当前块帧数 */
    uint32_t blk_peak;      /* 当前块峰值 */
    uint32_t blk_zc;        /* 当前块过零次数 */
    uint32_t blocks;        /* 已输出块数 */
    uint32_t active_blocks; /* 有声块数 */
    uint32_t tone_blocks;   /* 1kHz 成分达标的块数 */
    double   s1, s2;        /* Goertzel 状态 */
    double   energy;        /* 块内 Σx² */
    int16_t  snap[32];      /* 最近 32 个左声道样本（诊断波形用） */
    uint32_t snap_pos;
};

static void cap_meter_flush(const char *tag, struct cap_meter *m)
{
    /* freq = (zc / 2) / (CAP_BLOCK_MS/1000) = zc × 1000 / (2×CAP_BLOCK_MS) */
    uint32_t freq = m->blk_zc * 1000 / (2 * CAP_BLOCK_MS);
    /* Goertzel 功率：P = s1² + s2² − c·s1·s2；纯正弦 A·sin 时
       P ≈ (A·N/2)²，而 Σx² = A²N/2，故比值 P /(Σx²·N/2) ≈ 1。 */
    double p  = m->s1 * m->s1 + m->s2 * m->s2 - CAP_TONE_COEF * m->s1 * m->s2;
    double en = m->energy;
    uint32_t tone = 0;

    if (en > 0.0 && m->blk_frames > 0) {
        double r = p / (en * (m->blk_frames / 2.0));
        if (r < 0.0)
            r = 0.0;
        if (r > 2.0)
            r = 2.0;
        tone = (uint32_t)(r * 100.0);
    }

    printf("[CAP-ENV-%s] blk=%u frames=%u peak=%u freq=%u tone=%u\r\n",
           tag, m->blocks, m->blk_frames, m->blk_peak, freq, tone);

    /* 波形快照：只打 1 个块，用于肉眼判断采到的到底是不是正弦 */
    if (m->blocks == 13) {
        printf("[CAP-SNAP-%s] blk=%u samples=", tag, m->blocks);
        for (uint32_t i = 0; i < 32; i++)
            printf("%d,", (int)m->snap[(m->snap_pos + i) & 31]);
        printf("\r\n");
    }

    if (m->blk_peak >= CAP_ACTIVE_PEAK)
        m->active_blocks++;
    if (tone >= 50 && m->blk_peak >= CAP_ACTIVE_PEAK)
        m->tone_blocks++;
    m->blocks++;
    m->blk_frames = 0;
    m->blk_peak   = 0;
    m->blk_zc     = 0;
    m->s1 = m->s2 = 0.0;
    m->energy = 0.0;
}

/* 统计只看左声道（立体声两遍会重复计数），浮点只做 1kHz 能量比；
   采集自测是用户态程序，FPU 可用（TEST_MODE 的信号生成同样用它）。 */
static void cap_meter_feed(const char *tag, struct cap_meter *m,
                           const uint8_t *buf, uint32_t n)
{
    uint32_t step = 2 * CAP_CH;

    for (uint32_t off = 0; off + step <= n; off += step) {
        int32_t s = (int32_t)(int16_t)(buf[off] | (buf[off + 1] << 8));
        uint32_t a = (s < 0) ? (uint32_t)(-s) : (uint32_t)s;
        double   s0;

        if (a > m->blk_peak)
            m->blk_peak = a;
        if ((m->prev < 0 && s >= 0) || (m->prev >= 0 && s < 0)) {
            m->blk_zc++;   /* 块内计数（flush 时清零） */
            m->zc++;       /* 全局计数（不被 flush 影响） */
        }
        m->prev = s;
        m->frames++;

        s0 = (double)s + CAP_TONE_COEF * m->s1 - m->s2;
        m->s2 = m->s1;
        m->s1 = s0;
        m->energy += (double)s * (double)s;
        m->snap[m->snap_pos & 31] = (int16_t)s;
        m->snap_pos++;

        if (++m->blk_frames >= CAP_BLOCK_FRM)
            cap_meter_flush(tag, m);
    }
}

/* 从已启动的采集里取满 expect 字节；返回实际字节数，统计由内核回填到 st。
   每 20ms 读一次，轮次上限按「每次最多拿到的量」留一倍余量，避免录不到时死等。 */
static uint32_t cap_drain(const char *tag, uint32_t expect,
                          struct audio_capture_stats *st, struct cap_meter *m)
{
    struct timespec ts;
    uint8_t *buf = (uint8_t *)malloc(CAP_CHUNK);
    uint32_t total = 0;

    if (buf == NULL)
        return 0;

    int rounds = (int)(expect / (CAP_CHUNK / 2)) + 100;
    for (int i = 0; i < rounds; i++) {
        int n = audio_capture_read(buf, CAP_CHUNK, st);
        if (n > 0) {
            total += (uint32_t)n;
            cap_meter_feed(tag, m, buf, (uint32_t)n);
        }
        if (total >= expect)
            break;
        ts.tv_sec  = 0;
        ts.tv_nsec = CAP_POLL_MS * 1000 * 1000;
        nanosleep(&ts, NULL);
    }
    free(buf);
    return total;
}

static void cap_report(const char *tag, uint32_t total, uint32_t expect,
                       const struct audio_capture_stats *st,
                       const struct cap_meter *m)
{
    printf("[CAP-%s] bytes=%u expect=%u diff=%d lpib=%u wraps=%u overruns=%u peak=%u nonzero=%u\r\n",
           tag, total, expect, (int)total - (int)expect,
           st->lpib, st->wraps, st->overruns, st->peak, st->nonzero);
    printf("[CAP-%s] VERDICT: len=%s path=%s signal=%s\r\n", tag,
           (total > 0 && (int)total >= (int)(expect - expect / 20)) ? "OK" : "FAIL",
           (st->lpib > 0 && st->overruns == 0) ? "OK" : "FAIL",
           (st->peak > 0 && st->nonzero > 0) ? "PRESENT" : "SILENT");
    /* 全局口径：总过零率 ÷ 2 ÷ 采集时长（秒）= 等效频率 */
    uint32_t freq = (m->frames > 0)
                    ? (uint32_t)((uint64_t)m->zc * CAP_RATE / (2 * (uint64_t)m->frames))
                    : 0;
    printf("[CAP-%s] METER: frames=%u blocks=%u active=%u freq=%u duty=%u%% toneblocks=%u toneduty=%u%%\r\n",
           tag, m->frames, m->blocks, m->active_blocks, freq,
           (m->blocks > 0) ? m->active_blocks * 100 / m->blocks : 0,
           m->tone_blocks,
           (m->blocks > 0) ? m->tone_blocks * 100 / m->blocks : 0);
}

/* 客户机侧激励：播一段 1kHz 正弦（阻塞到播完，走既有播放 API，不改驱动）。
   幅度取接近满刻度：实测「板载渲染 -> 立体声混音 -> dsound 采集」这条链路
   衰减约 50dB（12000 幅度只落到 ±50 LSB），幅度太小会被底噪吃掉。 */
#define CAP_TONE_MS 700
#define CAP_TONE_AMP 30000.0

static void cap_sleep_ms(uint32_t ms)
{
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000 * 1000;
    nanosleep(&ts, NULL);
}

static void cap_play_tone(uint32_t ms)
{
    uint32_t n = CAP_RATE * ms / 1000;
    int16_t *pcm = (int16_t *)malloc(n * 2 * sizeof(int16_t));

    if (pcm == NULL) {
        printf("[CAP] tone alloc failed\r\n");
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        int16_t v = (int16_t)(sin(2.0 * M_PI * CAP_TONE_HZ * (double)i / CAP_RATE) * CAP_TONE_AMP);
        pcm[i * 2]     = v;
        pcm[i * 2 + 1] = v;
    }
    audio_set_volume(100);
    audio_play_pcm_ms(pcm, n * 4, CAP_RATE, CAP_CH, CAP_BITS, ms);
    free(pcm);
}

static void run_capture_test(void)
{
    struct audio_capture_stats st;
    struct cap_meter m;
    uint32_t rc, total;
    uint32_t bps = CAP_RATE * CAP_CH * (CAP_BITS / 8);

    printf("=== Capture self-test: %uHz %uch %ubit, bps=%u ===\r\n",
           CAP_RATE, CAP_CH, CAP_BITS, bps);

    /* A：1 秒，基准环 256KB；采集开始 300ms 后自己播 400ms 1kHz 正弦。
       播放是阻塞调用，期间不取数——256KB 环可存 1.48s，不会覆盖。 */
    memset(&st, 0, sizeof(st));
    memset(&m, 0, sizeof(m));
    rc = audio_capture_start(CAP_RATE, CAP_CH, CAP_BITS, 1000);
    printf("[CAP-A] start rc=%d\r\n", (int)rc);
    if (rc == 0) {
        cap_sleep_ms(200);
        cap_play_tone(CAP_TONE_MS);
        total = cap_drain("A", bps, &st, &m);
        printf("[CAP-A] stop rc=%d\r\n", audio_capture_stop());
        cap_report("A", total, bps, &st, &m);
    }

    /* B：3 秒，529200 字节 > 256KB → 缓冲升到 512KB，LPIB 应绕回 1 次 */
    memset(&st, 0, sizeof(st));
    memset(&m, 0, sizeof(m));
    rc = audio_capture_start(CAP_RATE, CAP_CH, CAP_BITS, 3000);
    printf("[CAP-B] start rc=%d\r\n", (int)rc);
    if (rc == 0) {
        total = cap_drain("B", bps * 3, &st, &m);
        printf("[CAP-B] stop rc=%d\r\n", audio_capture_stop());
        cap_report("B", total, bps * 3, &st, &m);
    }

    /* C：幂等与参数冲突（期望 0 / 0 / -2 / 0） */
    int rc_first = audio_capture_start(CAP_RATE, CAP_CH, CAP_BITS, 1000);
    int rc_same  = audio_capture_start(CAP_RATE, CAP_CH, CAP_BITS, 1000);
    int rc_diff  = audio_capture_start(48000, CAP_CH, CAP_BITS, 1000);
    int rc_stop  = audio_capture_stop();
    printf("[CAP-C] first=%d same=%d diff=%d stop=%d (expect 0/0/-2/0)\r\n",
           rc_first, rc_same, rc_diff, rc_stop);
}
#endif /* TEST_CAPTURE */

/* ================= 主函数 ================= */
void __main()
{
    size_t heap_size = 32*1024*1024;
    void  *heap_base = mmap(NULL, heap_size, PROT_READ|PROT_WRITE,
                            MAP_PRIVATE|MAP_ANON, -1, 0);
    g_heap = tlsf_create_with_pool(heap_base, heap_size);
}

void main(void *pv)
{
#ifdef TEST_CAPTURE
    /* 麦克风采集自测：录固定时长、打印统计后停在此处等 watcher 退出 */
    run_capture_test();
    while (1)
        ;
#elif defined(TEST_MODE)
    /* 音频质量评估：播放单一测试信号后停在此处，等待 watcher 优雅退出 */
    run_test_signal();
    while (1)
        ;
#else
#ifdef SORT_DEMO
    printf("========================================\r\n");
    printf("=== VGA 8-bit Shell Sort + Piano Demo =\r\n");
    printf("========================================\r\n\r\n");

    /* 设置初始音量（功能完善：音量控制） */
    audio_set_volume(80);
    printf("[AUDIO] volume set to 80\r\n");

    /* 1. 生成初始数据 */
    for (int i = 0; i < N; i++)
        a[i] = BAR_MIN + (int)(next_rand() % (BAR_MAX - BAR_MIN + 1));

    /* 2. 进入 VGA 8-bit 图形模式 */
    if (enter_vga8() == 0) {
        g_gfx_ok = 1;
        printf("[VGA] graphics mode entered, drawing bar chart\r\n");
    } else {
        printf("[VGA] graphics FAILED - running sort without visualization\r\n");
    }

    /* 3. 希尔排序 + 柱状图动态展示：每次 swap 触发一个音符，
       音符阻塞播完 -> 排序动画与音乐节奏严格同步。 */
    reset_score_playback();
    printf("[MELODY] Shell sort driving melody, one note per swap\r\n");
    shell_sort();

    /* 排序结束后补齐剩余音符，保证整首乐谱只播一遍、完整播完（含左手伴奏） */
    play_score_to_end();

    /* 播放结束：停止连续流（追加尾静音吸收截断） */
    audio_stop();
    printf("[AUDIO] continuous stream stopped\r\n");

    /* 功能完善：流状态查询（错误计数 + 播放位置） */
    {
        uint32_t errs = audio_get_errors();
        uint32_t pos  = audio_get_position_ms();
        printf("[AUDIO] errors: fifo=%u desc=%u  position: %u ms\r\n",
               errs & 0xFFFF, errs >> 16, pos);
    }

    printf("========================================\r\n");
    printf("=== Demo finished. Check serial.log =====\r\n");
    printf("===   [VGA] ... OK   (graphics entered) =\r\n");
    printf("===   [SORT] verify OK (sort correct)  ==\r\n");
    printf("===   [MELODY] notes played (piano)    ==\r\n");
    printf("========================================\r\n");

    while(1)
        ;
    task_exit(0);
#else /* !SORT_DEMO：默认演示——整曲流式播放 */
    printf("========================================\r\n");
    printf("=== EPOS HDA streaming audio demo ======\r\n");
    printf("=== Enya - We Wish You a Merry Christmas\r\n");
    printf("=== (mono 44.1kHz 16-bit PCM, ~220s)   =\r\n");
    printf("========================================\r\n\r\n");

    /* 设置初始音量（功能完善：音量控制） */
    audio_set_volume(80);
    printf("[AUDIO] volume set to 80\r\n");

    /* 整曲流式播放：驱动分块读盘（256KB/块）追加进连续流，
       内存占用恒定、分块之间无缝衔接（见 sys_audio_play_file_stream_ms）。 */
    printf("[SONG] streaming song.wav ...\r\n");
    int rc = audio_play_file_stream_ms("song.wav", 0);
    printf("[SONG] play returned %d\r\n", rc);

    /* 播放结束：停止连续流（追加尾静音吸收截断） */
    audio_stop();
    printf("[AUDIO] continuous stream stopped\r\n");

    /* 功能完善：流状态查询（错误计数 + 播放位置 + underrun 计数） */
    {
        uint32_t errs = audio_get_errors();
        uint32_t pos  = audio_get_position_ms();
        uint32_t ud   = audio_get_underruns(0);
        printf("[AUDIO] errors: fifo=%u desc=%u  position: %u ms  underruns: %u\r\n",
               errs & 0xFFFF, errs >> 16, pos, ud);
    }

    printf("========================================\r\n");
    printf("=== Demo finished. Check serial.log =====\r\n");
    printf("===   [SONG] streamed song.wav (audio)  =\r\n");
    printf("===   [AUDIO] underruns: 0 (seamless)   =\r\n");
    printf("========================================\r\n");

    while(1)
        ;
    task_exit(0);
#endif /* SORT_DEMO */
#endif /* TEST_MODE */
}

