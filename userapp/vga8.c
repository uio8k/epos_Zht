/* VGA 8bpp 显示支持，见 vga8.h。 */
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>

#include "graphics.h"
#include "vga8.h"

extern int vm86call(int fintr, uint32_t n, struct vm86_context *vm86ctx);

#define LADDR(seg, off) ((uint32_t)(((uint16_t)(seg) << 4) + (uint16_t)(off)))

/* VBE 8bpp 模式候选：640x480 优先，屏幕小时退到 640x400 */
static const int g_vga8_modes[] = {
    0x101,   /* 640x480x8 */
    0x103,   /* 800x600x8 */
    0x100,   /* 640x400x8 */
    0x105    /* 1024x768x8 */
};

static unsigned char g_font8x16[256 * 16];
static int g_font_ok = 0;

/* t in [0,1000): 蓝->青->绿->黄->红 */
static void rgb_from_t(int t, int *r, int *g, int *b)
{
    if (t < 200)      { *r = 0;              *g = 255 * t / 200;              *b = 255; }
    else if (t < 400) { *r = 0;              *g = 255;                        *b = 255 - 255 * (t - 200) / 200; }
    else if (t < 600) { *r = 255 * (t - 400) / 200; *g = 255;                  *b = 0; }
    else if (t < 800) { *r = 255;            *g = 255 - 255 * (t - 600) / 200; *b = 0; }
    else              { *r = 255;            *g = 0;                          *b = 255 * (t - 800) / 200; }
}

void vga8_palette(void)
{
    /* 把 256 项 RGB 表写到 vm86_init() 已映射为可写的低内存 0xE8000
       （QEMU 中 0xE8000-0xEFFFF 是 pc.ram，且 vm86.c 已 L2E_V|L2E_W|L2E_U 映射），
       再通过 vm86 int 0x10 AX=0x1002 写入 VGA DAC。
       注意：不能用 mmap(0x60000)，因为 vm86 仿真器按线性地址访问，
       0x60000 未映射会导致页错误。 */
    uint8_t *pal = (uint8_t *)0xE8000;

    for (int i = 0; i < 256; i++) {
        int r = 0, g = 0, b = 0;
        if (i == COL_BG)          { r = 0;   g = 0;   b = 0; }
        else if (i == COL_WHITE)  { r = 255; g = 255; b = 255; }
        else if (i == COL_GREEN)  { r = 0;   g = 255; b = 0; }
        else if (i == COL_RED)    { r = 255; g = 0;   b = 0; }
        else if (i == COL_YELLOW) { r = 255; g = 255; b = 0; }
        else if (i == COL_BLUE)   { r = 0;   g = 0;   b = 255; }
        else {
            int t = (i - COL_GRAD_START) * 1000 /
                    (COL_GRAD_END - COL_GRAD_START);
            rgb_from_t(t, &r, &g, &b);
        }
        pal[i * 3 + 0] = (uint8_t)(r >> 2);   /* VGA DAC 是 6-bit */
        pal[i * 3 + 1] = (uint8_t)(g >> 2);
        pal[i * 3 + 2] = (uint8_t)(b >> 2);
    }

    struct vm86_context vm86ctx = { .ss = 0x0000, .esp = 0x1000 };

    /* 注意：AX=1002h（set all palette registers）在 VBE 模式下会被 SeaVGABIOS
       忽略——实测回读仍是 6x6x6 默认立方体。改用 1012h（set block of DAC
       registers），它是 VBE 支持的那一支。 */
    vm86ctx.eax = 0x1012;
    vm86ctx.ebx = 0x0000;      /* 起始寄存器 0 */
    vm86ctx.ecx = 0x0100;      /* 写 256 个 */
    vm86ctx.es  = 0xE800;
    vm86ctx.edx = 0x0000;
    if (vm86call(1, 0x10, &vm86ctx) != 0) {
        printf("[VGA] palette set via vm86 FAILED\r\n");
        return;
    }

    /* 回读第 1 项（白）作为一次性自检：写入无效时吐一行告警，
       免得后面画出一屏看不懂的颜色还要从头查。 */
    {
        struct vm86_context rd = { .ss = 0x0000, .esp = 0x1000 };

        rd.eax = 0x1015;       /* 读单个 DAC 寄存器 */
        rd.ebx = 0x0001;
        vm86call(1, 0x10, &rd);
        if (((rd.edx >> 8) & 0xFF) != 0x3F ||
            ((rd.ecx >> 8) & 0xFF) != 0x3F ||
            (rd.ecx & 0xFF) != 0x3F)
            printf("[VGA] palette write ignored (dac[1]=%02x%02x%02x)\r\n",
                   (rd.edx >> 8) & 0xFF, (rd.ecx >> 8) & 0xFF, rd.ecx & 0xFF);
        else
            printf("[VGA] 256-color palette set OK (dac[1]=3f3f3f)\r\n");
    }
}

int vga8_font_load(void)
{
    struct vm86_context vm86ctx = { .ss = 0x0000, .esp = 0x1000 };
    const unsigned char *src;
    uint32_t addr;
    int nonzero = 0;

    vm86ctx.eax = 0x1130;      /* int 10h: 取字库指针 */
    vm86ctx.ebx = 0x0600;      /* BH=6 -> 8x16 */
    if (vm86call(1, 0x10, &vm86ctx) != 0) {
        printf("[VGA] font 1130h call failed, big text disabled\r\n");
        return -1;
    }

    addr = LADDR(vm86ctx.es, (uint32_t)(vm86ctx.ebp & 0xFFFF));
    src  = (const unsigned char *)addr;

    /* 粗校验：字库前 512 字节不可能全零（空格也只有少量位是 0）。
       全零说明 BIOS 没填指针或指向了坏地址，宁可退化也不要画一屏黑块。 */
    for (int i = 0; i < 512; i++) {
        if (src[i] != 0) {
            nonzero = 1;
            break;
        }
    }
    if (!nonzero) {
        printf("[VGA] font at %08x looks empty, big text disabled\r\n", addr);
        return -1;
    }

    memcpy(g_font8x16, src, sizeof(g_font8x16));
    g_font_ok = 1;
    printf("[VGA] font 8x16 loaded from %04x:%04x\r\n",
           (uint16_t)vm86ctx.es, (uint16_t)(vm86ctx.ebp & 0xFFFF));
    return 0;
}

int vga8_font_ready(void)
{
    return g_font_ok;
}

int vga8_text(const char *s, int x, int y, int scale,
              unsigned char fg, unsigned char bg)
{
    int cx = x;

    if (!g_font_ok || s == NULL || scale <= 0)
        return -1;

    for (const char *p = s; *p != '\0'; p++) {
        const unsigned char *glyph = g_font8x16 + (unsigned char)(*p) * 16;

        for (int row = 0; row < 16; row++) {
            unsigned char bits = glyph[row];

            for (int col = 0; col < 8; col++) {
                /* VGA 字库是 MSB 在左（bit7 = 最左一列）。按 (1<<col) 取会画成镜像，
                   第一版就是这么错的，截图里 "FAIL" 左右反了。 */
                COLORREF color = (bits & (0x80 >> col)) ? (COLORREF)fg : (COLORREF)bg;

                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        setPixel(cx + col * scale + dx, y + row * scale + dy, color);
            }
        }
        cx += 8 * scale;
    }
    return 0;
}

int vga8_enter(void)
{
    int n = (int)(sizeof(g_vga8_modes) / sizeof(g_vga8_modes[0]));

    printf("[VGA] Entering VGA 8-bit (256-color) graphics mode...\r\n");
    for (int m = 0; m < n; m++) {
        int mode = g_vga8_modes[m];

        printf("[VGA] trying VBE mode 0x%04x ... ", mode);
        if (init_graphic(mode) == 0) {
            printf("OK (%dx%dx%d)\r\n",
                   g_graphic_dev.XResolution,
                   g_graphic_dev.YResolution,
                   g_graphic_dev.BitsPerPixel);
            vga8_palette();
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
