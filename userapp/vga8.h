/* VGA 8bpp（VBE 256 色）显示支持：进模式、写调色板、取 BIOS 字库、放大画字。
 *
 * 从 main.c 的 SORT_DEMO 段落抽出来，供两条路径共用：
 *   - SORT_DEMO：柱状图演示进模式 + 调色板
 *   - TEST_CAPTURE：判定结果大字（vga8_font_load 必须在 vga8_enter 之前调）
 *
 * 字库来源：int 10h AX=1130h BH=6，BIOS 返回 ES:BP 指向 8x16 字库（256 字形 x 16 字节）。
 * 为什么要先取：切到 VBE 之后 VGA 平面 2 与 ROM 字库指针都不保证可读（OSDev wiki VGA Fonts）。
 */
#ifndef VGA8_H
#define VGA8_H

/* 调色板索引，与 vga8_palette() 写入的表一一对应 */
#define COL_BG         0   /* 黑 */
#define COL_WHITE      1   /* 白 */
#define COL_GREEN      2   /* 绿 */
#define COL_RED        3   /* 红 */
#define COL_YELLOW     4   /* 黄 */
#define COL_BLUE       5   /* 蓝 */
#define COL_GRAD_START 6   /* 渐变起始索引 */
#define COL_GRAD_END   250 /* 渐变结束索引 */

/* 进 VGA 8bpp 模式并写好调色板。0 成功，-1 所有候选模式都失败。 */
int vga8_enter(void);

/* 写 256 项调色板（含 6..250 渐变）。失败只打印，不影响调用方。 */
void vga8_palette(void);

/* 取 BIOS 8x16 字库到内部缓冲。0 成功，-1 失败。必须在 vga8_enter 之前调用。 */
int vga8_font_load(void);

/* 放大画字符串：8x16 字形按 scale 倍逐像素铺。fg/bg 是调色板索引。
   字库未加载或参数非法返回 -1，不画任何像素。
   返回 0 时绘制范围为 (x, y) 起，宽 8*scale*字符数，高 16*scale。 */
int vga8_text(const char *s, int x, int y, int scale,
              unsigned char fg, unsigned char bg);

/* 字库是否可用（1/0），用于调用方决定要不要画。 */
int vga8_font_ready(void);

#endif /* VGA8_H */
