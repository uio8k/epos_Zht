# HDA 规范补齐技术方案：P06 / P07

基线 `HEAD = 209a1a6`（播放侧设备探测改为 DFS）。文中 `文件:行号` 按该提交核对，换提交后行号会漂移，结论不变。

本轮只写方案，不改内核代码、不改测试、不跑构建。实现按第 9 章顺序在下一轮排期。

## 1 范围

| 项 | 内容 |
|---|---|
| P06 | BDL 对齐、条目格式、条目数与 CBL 的合规断言（现无任何断言，靠人眼） |
| P07 | GCAP 流数读取、EAPD、mic 的 VrefEn、Unsol 取舍 |
| 产出 | 本文档 + `docs/evidence/2026-10-28/wiki-hda-excerpt.txt`（wiki 原文摘录） |
| 不做 | 内核/测试代码改动、镜像重建、QEMU 运行；插拔检测、CORB/RIRB 化 |

涉及文件（下一轮要动的）：`kernel/hda.c`、`kernel/hda.h`、`kernel/hda_stream.c`、`kernel/hda_capture.c`、`kernel/hda_topo.c`、`kernel/hda_profile.c`、`kernel/Makefile`，新增 `kernel/hda_regs.c/.h` 与 `tests/test_hda_regs.c`。

## 2 依据等级与采信规则

| 等级 | 来源 | 用法 |
|---|---|---|
| 一级 | Intel HDA 规范 1.0a | 位段、payload 编码、时序的最终口径 |
| 二级 | OSDev wiki，oldid=30627（2026-05-26 最后编辑） | 流程顺序、常见陷阱、字段表的交叉校验 |
| 三级 | 本仓库实测日志 | 判断 wiki/规范在 QEMU 3.1 上是否落得了地 |

取证明细见 `docs/evidence/2026-10-28/wiki-hda-excerpt.txt`：URL、oldid、抓取时间、六处原文逐字摘录都在里面。该页页头自标 "work in progress and may thus be incomplete"，所以它只作二级依据。

采信规则：

1. wiki 与规范冲突时以规范为准，并在文中标出，不静默改口径。
2. wiki 没有原文的条目，写「wiki 无原文」，改标规范依据与置信度（高/中/低）。
3. 只信能落到 QEMU 或真机上的说法；测不出来的写进「不确定项」，不写成结论。
4. 每条结论后附出处，格式 `[wiki: 段落名]`、`[spec: 表号/章节]`、`[实测: 文件:行号]`。

### 2.1 wiki 的问题清单

| # | 位置 | wiki 写的 | 实际 | 处理 |
|---|---|---|---|---|
| W1 | 流描述符表 `0x0C` | LVI = 条目数 + 1 | 规范为条目数 - 1；代码 `hda_stream.c:522`、`hda_capture.c:314` 都是 n-1，QEMU 也按 n-1 跑 | 判为 wiki 笔误，不采信 |
| W2 | GCAP 表 | BSS 占 bits 7:3（5 位） | 规范常见表述为 bits 5:3（3 位） | 只用 ISS/OSS 两段（两处一致）；BSS 仅在算双向流基址时用，实施前复核 |
| W3 | GCAP 表 | 未列 NSDO（bits 2:1） | 规范有该字段 | 本方案不读 NSDO，多串行数据线不支持 |
| W4 | 初始化第 (9) 条 | "read up on VrefEn ... I'm still searching for it" | 规范里 `707` 的 payload bits2:0 有完整编码 | 见第 7 章，标规范依据 |
| W5 | verb 表 | 只有 `70C Set EAPD/BTL`，无 payload 位定义 | 规范：`70C` 的 data bit1 = EAPD、bit0 = BTL | 见第 6 章，标规范依据 |
| W6 | 全文 | 未出现 per-widget unsolicited 的 verb 号 | 规范有 SET_UNSOLICITED_RESPONSE_ENABLE | 见第 8 章；本方案不实现 |
| W7 | "Initalizing sound card" | 整节正文只有 `TODO` | — | 控制器初始化以寄存器表 + 规范为准 |

## 3 现状核对（HEAD 209a1a6）

| 项 | 依据口径 | 代码现状 | 判定 |
|---|---|---|---|
| GCAP 解码 | OSS=bits15:12、ISS=11:8、BSS=7:3、64OK=bit0 | `hda.c:165-167` 用 `gcap&0xF`、`>>4`、`>>8` | 解码错。QEMU 实测 GCAP=0x4401，串口打成 `OSS=1 ISS=0 BSS=4`，正确是 OSS=4/ISS=4/BSS=0 |
| 流描述符基址 | `0x80 + 0x20×n`，输入在前、输出在后、双向最后 | `hda.h:58` 公式通用；`hda.h:66` 把播放写死在第 4 号 | 仅当 ISS=4 成立。真机 ISS≠4 时第 4 号不是输出流 |
| BDL 对齐 | 1KB 对齐 | `hda_alloc_dma`（`hda.c:33-45`，起始地址 `hda.c:28` 的 `HDA_DMA_BASE 0x400000`）走页分配 | 事实合规（4KB 对齐），零断言 |
| 条目数与 LVI | ≥2 条、≤256 条、LVI=n-1 | 播放 128/256 条（`hda_stream.c:494-508`），采集 64/128 条（`hda_capture.c:299-305`），LVI 均 n-1 | 合规，零断言 |
| LVI 注释 | LVI 是 16 位字段，256 上限来自条目数上限 | `hda.h:122-124` 写「LVI 为 8 位（0-255）」 | 注释错，需订正 |
| wbinvd | BDL 写完刷缓存再 RUN | `hda_stream.c:536`、`hda_capture.c:381` | 合规 |
| INTCTL | bit31 GIE + bits29:0 逐流使能（位号 = 绝对索引） | `hda.c:185` 只写 `0x80000000` | 真机可能不上报流中断 |
| EAPD | verb `0x70C`，payload bit1 | 全仓无该 verb | 缺 |
| VrefEn | pin ctrl `0x707`，bits2:0 | `hda.h:232-234` 注释写「bits 4:2 是 VREF」，播放只写 `0x707 \| 0x40` | 位段注释错，且没有 mic 侧处理 |
| Unsol | GCTL bit8 + per-widget verb，响应经 RIRB 回送 | `hda.c:178` 把 GCTL bit8 置上了（实测 GCTL=0x101），驱动全走 ICW/IRR，无 RIRB | 开了也收不到 |

## 4 P07-a：GCAP 解码与流描述符索引

### 4.1 依据

wiki GCAP 表（`[wiki: 0x00 (4 bytes) Global Capabilities]`）：bits15:12 = OSS（输出流条数），bits11:8 = ISS（输入流条数），bits7:3 = BSS，bit0 = 64 位寻址支持。流描述符基址（`[wiki: Registers of Stream Descriptors]`）：每条占 0x20，输入在前、输出居中、双向最后，条数从 GCAP 读。

`[实测: docs/evidence/2026-10-28/qemu-serial.log:31]` 给出 QEMU 3.1 的 GCAP = 0x4401，即 OSS=4、ISS=4、BSS=0、64OK=1。

### 4.2 现码错在哪

```165:167:kernel/hda.c
    printk("HDA: GCAP=0x%04x v%d.%d (OSS=%u ISS=%u BSS=%u)\r\n",
           gcap, vmaj, vmin,
           gcap & 0xF, (gcap >> 4) & 0xF, (gcap >> 8) & 0xF);
```

三个字段整体错位 8 位。目前这段只用于打印，没有参与基址计算，所以 QEMU 播放没受影响；一旦在真机上按这个值推 SD 基址，会直接写到不存在的流描述符上。

### 4.3 索引公式

| 用途 | 绝对索引 | QEMU 取值 |
|---|---|---|
| 输入流第 k 条 | k | 0 |
| 输出流第 k 条 | ISS + k | 4 |
| 双向流第 k 条 | ISS + OSS + k | — |

`HDA_SD_BASE(n) = 0x80 + 0x20*n`（`hda.h:58`）对绝对索引本来就是对的：三类流在寄存器空间里连续排列，公式不需要分类判断。要改的只有「第 4 号必是输出流」这一个假设。

两条等价性要求：

- QEMU 下 ISS=4，算出的输出索引仍是 4，寄存器写入序列与现在逐字节相同，录音/播放基线不应有任何变化。这是本次改动唯一的 QEMU 侧验证点。
- stream tag 与索引解耦：tag 是 CTL byte2 的 bits7:4（`hda_stream.c:538` 现写 `HDA_OUTPUT_STREAM << 4`），取值 1–15、0 保留。改造后新增 `HDA_OUTPUT_TAG 4`、保留 `HDA_INPUT_TAG 1`，byte2 仍写 0x40 / 0x10。

### 4.4 INTCTL 逐流使能

wiki（`[wiki: 0x20 (4 bytes) Interrupt Control]`）：bit31 = GIE，bits29:0 每流一位，位序按输入流、输出流、双向流排，位号即绝对索引。现码只写 bit31。

补法：`INTCTL = (1u<<31) | (1u<<sd_in) | (1u<<sd_out)`，其中 sd_in/sd_out 由 GCAP 算出。QEMU 下现在只写 GIE 也能跑通播放，但中断模式与轮询回退共用同一套指针逻辑，日志里分不出走的是哪条，所以「补上逐流位后 QEMU 行为不变」只能靠回归确认，不能靠推断。

### 4.5 接口

纯函数放进新文件 `kernel/hda_regs.c` / `hda_regs.h`，不碰 MMIO，宿主 L1 可直接编进测试（沿用 `tests/test_topo.c` 把源文件复制进 `tests/build/` 的做法）：

```c
struct hda_gcap { uint8_t oss, iss, bss, ok64; };

struct hda_gcap hda_gcap_decode(uint16_t raw);
uint8_t hda_sd_index_in (const struct hda_gcap *c, uint8_t k);
uint8_t hda_sd_index_out(const struct hda_gcap *c, uint8_t k);
```

`struct hda_dev`（`hda.h:150-173`）加三个字节：`uint8_t sd_in, sd_out, gcap_ok64`，在 `hda.c` 探测到 GCAP 之后算好，后续所有 `HDA_SD_*(HDA_OUTPUT_STREAM)` / `HDA_SD_*(HDA_INPUT_STREAM)` 换成 `HDA_SD_*(g_hda.sd_out)` / `(g_hda.sd_in)`。

### 4.6 落点

| 文件 | 改动 |
|---|---|
| `hda.c:161-167` | 改用 `hda_gcap_decode` 打印，输出 `OSS=4 ISS=4 BSS=0 64OK=1 out_sd=4 in_sd=0` |
| `hda.c:185` | INTCTL 补逐流位 |
| `hda.h:58-72` | `HDA_OUTPUT_STREAM` 降级为「QEMU profile 的默认输出条序号」，新增 tag 宏；`HDA_SD_BASE` 宏改收变量 |
| `hda_stream.c`、`hda_capture.c` | 全部流寄存器访问改用 `g_hda.sd_out` / `g_hda.sd_in` |
| `hda.h:150-173` | `struct hda_dev` 增字段 |

## 5 P06：BDL 合规断言

### 5.1 依据

`[wiki: Create Buffer Descriptor List]` 五条硬要求：1KB 对齐（低 10 位为 0）、至少 2 条、每 16 字节（8B 地址 + 4B 长度 + 4B 属性，bit0 = IOC）、写完必须 `wbinvd`、CBL 等于所有条目长度之和（`[wiki: 0x08 Cyclic Buffer Length]`）。条目长度上限 64KB 来自 `hda.h:130` 的既有注释与规范一致，wiki 未写。

### 5.2 现状对照

| 要求 | 现状 | 结论 |
|---|---|---|
| 1KB 对齐 | `hda_alloc_dma` 走 `frame_alloc_in_addr`，页（4KB）对齐，起点 4MB | 满足，无断言 |
| ≥2 条 | 播放 512KB/4KB=128 条、4MB/16KB=256 条；采集 256KB/4KB=64 条、512KB=128 条 | 满足，无断言 |
| 16B/条 | `struct hda_bdl_entry` packed（`hda.h:127-132`） | 满足，无编译期断言 |
| bit0=IOC | 播放全条目 `flags=0x01`（`hda_stream.c:504`），采集 `flags=0`（`hda_capture.c:304`） | 合规 |
| CBL=求和 | 播放 `g_cont_size`、采集 `g_cap_size`，与分段求和相等 | 满足，无断言 |
| wbinvd | 两条路径 RUN 前都调 `hda_flush_cache()` | 合规 |
| LVI=n-1 | `hda_stream.c:522`、`hda_capture.c:314` | 合规 |

也就是说：现在**事实合规，但没有一条能证明**。改段长、改缓冲大小的那个人（大概率是三个月后的自己）没有任何提示。

### 5.3 校验函数

```c
#define HDA_BDL_OK              0
#define HDA_BDL_ERR_ALIGN      -1   /* bdl_pa 未 1KB 对齐 */
#define HDA_BDL_ERR_TOO_FEW    -2   /* 条目数 < 2 */
#define HDA_BDL_ERR_TOO_MANY   -3   /* 条目数 > 256 */
#define HDA_BDL_ERR_SEG        -4   /* 某条长度 0 或 > 64KB */
#define HDA_BDL_ERR_ADDR       -5   /* 某条地址为 0（未初始化） */
#define HDA_BDL_ERR_CBL        -6   /* 长度为和 ≠ cbl */

int hda_bdl_validate(const struct hda_bdl_entry *bdl, uint16_t entries,
                     uint32_t cbl, uint32_t bdl_pa);
```

行为约定：

- 只读遍历，O(n)，n ≤ 256，µs 级，只在 start 前调一次，不进热路径。
- 失败时打印 `HDA: BDL reject rc=%d entries=%u cbl=%u pa=0x%x` 并返回负码，调用方据此放弃启动流。不降级、不「尽量跑」，因为错的对齐或错的 CBL 在 QEMU 上表现为爆音，在真机上表现为 DMA 越界。
- 编译期再加一条 `_Static_assert(sizeof(struct hda_bdl_entry) == 16, "BDL entry must be 16 bytes")`，放在 `hda.h` 结构体后面，把「16 字节」这件事从注释变成编译约束。

### 5.4 调用点

| 位置 | 插入点 |
|---|---|
| `hda_stream.c` | 写 `HDA_SD_BDPL` 之前（现 519 行之前），拿 `bdl_phys`、`bdl_entries`、`g_cont_size` 校验 |
| `hda_capture.c` | 写 `HDA_SD_BDPL` 之前（现 311 行之前），拿 `g_cap_bdl_pa`、`entries`、`g_cap_size` 校验 |

### 5.5 L1 向量（宿主，无硬件）

| # | 输入 | 期望 |
|---|---|---|
| T1 | `hda_gcap_decode(0x4401)` | OSS=4 ISS=4 BSS=0 ok64=1 |
| T2 | `hda_gcap_decode(0x1801)` | OSS=1 ISS=8 BSS=0 ok64=1 |
| T3 | T1 结果 → `sd_index_out(0)` / `sd_index_in(0)` | 4 / 0 |
| T4 | T2 结果 → `sd_index_out(0)` / `sd_index_out(2)` | 8 / 10 |
| T5 | 正常 128 条、CBL 匹配、地址 1KB 对齐 | 0 |
| T6 | 同上，`bdl_pa += 0x200` | -1 |
| T7 | 1 条 | -2 |
| T8 | 257 条 | -3 |
| T9 | 单条长度 0x10001 | -4 |
| T10 | 长度和比 CBL 少 4 字节 | -6 |

`make test` 现有 18 条（三段 5/5/8）应保持全绿，新增本表 10 条后总时长仍应 <2s。

### 5.6 顺带订正

`hda.h:122-124` 的注释「LVI 为 8 位（0-255）」改成「LVI 是 16 位字段；256 条上限来自规范的 BDL 条目数上限，不是字段位宽」。这条不影响行为，但下次有人按注释推边界就会推错。

## 6 P07-b：EAPD

### 6.1 依据

wiki 只在 verb 表里留了 `70C Set EAPD/BTL` 一行，第 (11) 条写「Set EAPD to enable the external amp for the speakers」，没有 payload 位定义（`[wiki: Node Commands]`、`[wiki: Setting up the AFG codec]`）。

规范口径（`[spec: SET_EAPD_BTL_ENABLE 0x70C]`，置信度 中高）：data 的 bit1 = EAPD 使能、bit0 = BTL。回读用 `0xF0C`（GET_EAPD_BTL_ENABLE）。能力位在 widget 能力参数 `09h` 的 bit16（EAPD 支持，置信度 中）。

### 6.2 策略

QEMU 的 hda-duplex 没有外部功放，EAPD 位大概率读回 0 或不实现，这里能验的只有「verb 发出去了、回读值打出来了」。所以策略按真机可用性设计，QEMU 侧只保证不破坏现有链路。

- 只在拓扑解析出的输出 pin（`g_hda.output_pin`）上发，不发到 DAC 和 mixer。
- 门控两条，满足其一才发：widget 能力 `09h` 的 bit16 为 1；或 `hda_profile.c` 的条目显式标了 `eapd = 1`（真机 dump 到位后按机型补）。
- 载荷固定 `0x02`（EAPD=1、BTL=0）。先读 `0xF0C`，已经置位就跳过，避免每轮重复写。
- 回读不一致只告警，不进 `HDA_CAP_ERR_*` 那套致命分级。理由与 `docs/decisions-adc.md` 的 D3 相同：能力位不支持的项做成致命，会把可用通路判死。

### 6.3 落点与打印

| 文件 | 改动 |
|---|---|
| `hda.h` | 新增 `HDA_VERB_SET_EAPD_BTL 0x70C00`、`HDA_VERB_GET_EAPD_BTL 0xF0C00`、`HDA_EAPD_ENABLE 0x02`，`HDA_PARAM_AUDIO_WIDGET_CAP` 已有（`hda.h:209`），补 `HDA_WIDGET_CAP_EAPD (1<<16)` |
| `hda.c:220-246`（配置输出路径处） | pin 使能、amp 取消静音之后，按门控发 EAPD 并回读 |
| `hda_profile.c` | 条目结构增 `uint8_t eapd, vref`，QEMU 条目填 `eapd=0, vref=0`（保持现状） |

串口行：`HDA: EAPD pin=0x07 req=0x02 rd=0x02 (cap=1 profile=0)`。真机无声时这行是第一个要看的东西。

## 7 P07-c：VrefEn

### 7.1 依据

`PIN_WIDGET_CONTROL 0x707` 的 payload：bit7 HPHN_AMP、bit6 OUT_EN、bit5 IN_EN、bits2:0 VREFEN（`[spec: Pin Widget Control]`，置信度 中高）。本仓库现在用 `0x40` 表示输出使能（`hda.c:223`）、`0x20` 表示输入使能（`hda.h:234`），与此一致。

VREFEN 取值：0 = Hi-Z（关）、1 = 50% Vdd、2 = Gnd（0V）、4 = 80%、5 = 100%（置信度 中高）。

能力位不是单个 bit8，而是 pin 能力参数 `0Ch` 的 bits13:8 五位掩码，判断写成 `(cap & (0x37 << 8))`，每一位对应一档可用偏置。依据：`[实测: gitee 58291/onix src/kernel/hda.c:756]` 写作 `AC_PINCAP_VREF (0x37<<8)`，与 Linux 同名常量一致，置信度 中高。只判 bit8 会把「仅支持 80%/100%」的 pin 误判为不支持，也会把只支持 Hi-Z 的 pin 当成可加偏置。

> 订正记录（2026-10-29）：本文初版写「能力位 = `0Ch` bit8」，范围偏窄，按上句改。

`hda.h:232-234` 的注释写「bits 4:2 是 VREF」，比实际位段高一位，需订正为 bits2:0。

### 7.2 策略

- 默认值 Hi-Z（0），即与现状一致，不动 QEMU 行为。
- 只有同时满足三条才写非零：pin 能力 `0Ch` 的 `(cap & (0x37 << 8))` 非零，且 profile 请求的那一档落在掩码允许的位里；该 pin 的 default config 设备类型是 Mic In（`0xA`，`hda.h:230` 已有宏）；`hda_profile.c` 条目给了 `vref` 值。
- 写的时候与 IN_EN 合成一次写：`0x20 | (vref & 0x7)`，不单独发一次 verb，避免中间态无偏置。
- 回读 `0x707` 比对 bits2:0，不符告警一行。

不做「1→4→5 自动扫描」。自动扫会把麦克风偏置电平来回改，测出来的电平变化分不清是偏置还是音源，反而污染排查。逐级试写进真机排查手册（EAPD → VrefEn 1/4/5 → GCAP → pin sense）。

## 8 P07-d：Unsol 不做

### 8.1 事实

- `hda.c:178` 把 `HDA_GCTL_UNSOL`（bit8）置上了，实测 `GCTL=0x00000101`（`[实测: docs/evidence/2026-10-28/qemu-serial.log:33]`）。
- 驱动全程用 Immediate Command（ICW/IRR）发 verb，CORB/RIRB 从未启用（`hda.c:79-82` 注释写明绕开 QEMU 3.1 的 CORB DMA bug）。
- unsolicited response 的回送通道是 RIRB。没有 RIRB，就没有投递路径（`[spec: unsolicited response 经 RIRB 通道]`，置信度 高；wiki 只写了 GCTL bit8 与「记得给 jack 设 Unsol」，没写投递机制）。

### 8.2 结论与理由

不实现。三条理由，按分量排：

1. 开了也收不到。GCTL bit8 只是「允许 codec 主动发」，没有 RIRB 缓冲区的读指针推进，事件落不到 guest。
2. 为它启用 CORB/RIRB 会推翻既有决策。整套驱动绕开 CORB DMA 就是为了躲 QEMU 3.1 的 bug，为了插拔通知把这个坑重新挖开，收益和代价不成比例。
3. 插拔检测不需要它。`0xF09`（GET_PIN_WIDGET_SENSE）轮询就够，wiki verb 表明写「but you can poll them if you prefer」。真要做，放在 syscall 层做 100ms 轮询，插进现有 timer 钩子，不碰中断。

### 8.3 处置

`hda.c:178` 去掉 `HDA_GCTL_UNSOL`，只留 `HDA_GCTL_CRST`。留着比去掉更糟：后来人看到 GCTL bit8 = 1 会以为插拔通知已经在工作。

副作用是 GCTL 从 `0x101` 变 `0x001`，串口日志第 33 行会变。这是预期变化，验收时按新值核对，不算回归。

## 9 实施清单与顺序（下一轮）

每步做完都能独立验证，不攒到最后一起测。

| 步 | 内容 | 完成标志 |
|---|---|---|
| 1 | `hda_regs.c/.h` + `tests/test_hda_regs.c` 10 条向量 + `run_tests.bat` 新增一段（现第 4 段是已停用的计划自洽校验，接在它后面） | `make test` 28 条全绿、<2s |
| 2 | `hda.c` GCAP 解码、`sd_in/sd_out`、INTCTL 逐流位；`hda.h` 宏与注释订正；两条流路径改用运行时索引 | 串口 GCAP 行数值正确、`out_sd=4`；QEMU 播放与改动前听感一致 |
| 3 | EAPD（门控 + 回读 + 打印）、VrefEn（同上）、profile 增 `eapd/vref` 字段 | 串口出现 EAPD/VREF 两行，QEMU 上回读值可解释 |
| 4 | 两条 start 路径接 `hda_bdl_validate`；`_Static_assert` 进 `hda.h` | 串口出现 `BDL ok`；故意改坏段长能打出 `BDL reject` |
| 5 | 去掉 GCTL.UNSOL；跑回归 | `run_all.bat --suite fast` 全绿、`run_capture_all.bat` 4/4 PASS |

第 2 步是唯一能碰到 QEMU 基线的地方，放在 EAPD/VrefEn 之前，出了问题好定位。

## 10 验收判据

| 层 | 判据 |
|---|---|
| 文档 | 每个结论都有出处标记；不确定项单列在第 11 章，不混进结论 |
| 引用一致性 | 文内所有 `文件:行号` 对 HEAD 209a1a6 逐个 grep 核对，核对记录附在提交说明里 |
| L1 | 新增 10 条（5.5 表）全绿；原 18 条不退化；`make test` <2s |
| 串口 | 出现 `HDA: GCAP=0x4401 v1.0 OSS=4 ISS=4 BSS=0 64OK=1 in_sd=0 out_sd=4`、`HDA: INTCTL set to 0x80000011`、`HDA: EAPD ...`、`HDA: BDL ok entries=... cbl=...` |
| 回归 | `hda_tests\run_all.bat --suite fast` 全绿；`run_capture_all.bat` 两轮 4/4 PASS；播放 LPIB 推进与 underrun 计数不劣于 209a1a6 基线 |
| 真机（受 P01 阻塞） | EAPD 回读一致；VrefEn 按 profile 生效；无声排查按 EAPD → VrefEn → GCAP → pin sense 顺序 |

`INTCTL=0x80000011` 的算法：bit31 GIE，bit4 = 输出流绝对索引 4，bit0 = 输入流绝对索引 0。

## 11 风险、回退与不确定项

回退面很小，三处都能单独撤销：

| 改动 | 风险 | 回退 |
|---|---|---|
| 运行时流索引 | 唯一可能影响 QEMU 基线的地方 | 恢复 `HDA_OUTPUT_STREAM 4` 常量，一处宏 |
| INTCTL 逐流位 | 若 QEMU 因某些未明原因开始丢 BCIS 中断 | 回到只写 `0x80000000` |
| 去掉 GCTL.UNSOL | 无功能影响（本来收不到） | 重新置 bit8，但要在注释里写明无效 |
| EAPD / VrefEn | 真机上写错偏置会把 line-in 电平拉偏 | profile 里 `eapd=0, vref=0` 即可全关 |

不确定项（实施前用规范原文复核，别按本文推断）：

| # | 题 | 本文取值 | 置信度 |
|---|---|---|---|
| U1 | 条目指向的 PCM 缓冲区是否另有对齐要求（BDL 自身 1KB 是确定的） | 未做要求 | 低，wiki 未提 |
| U2 | BSS 字段位宽 5 位还是 3 位（wiki 7:3 与规范常见 5:3 不一致） | 只用来算双向流基址，本机不涉及 | 中 |
| U3 | `70C` payload 的 EAPD/BTL 位序 | bit1=EAPD、bit0=BTL | 中高 |
| U4 | VREFEN 在真机上该选哪一档（编码 bits2:0 与能力掩码 `(0x37<<8)` 已定） | 0/1/2/4/5 五档，默认 0 | 编码 高 / 选档 待真机 |
| U5 | per-widget unsolicited 的 verb 号与 tag 字段布局 | 不实现，未取值 | 低 |

本机无法验证的项：EAPD 的实际功放效果、VrefEn 的偏置效果、真机 ISS≠4 的索引路径。这三项只能等 P01 治具到位，因此代码里只做「门控 + 回读 + 打印」，不做自动调参。

