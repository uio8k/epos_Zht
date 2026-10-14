#include <stddef.h>
#include <string.h>
#include "kernel.h"
#include "dosfs.h"
#include "hda.h"
#include "hda_internal.h"

extern VOLINFO g_volinfo;

/* 截断处线性淡出时长（防止爆音） */
#define HDA_FADE_MS 80

/* 多路混音最大并发声道数 */
#define HDA_MAX_VOICES 4

/* WAV 文件头结构 */
struct wav_header {
    uint8_t  riff[4];       /* "RIFF" */
    uint32_t file_size;     /* 文件总长 - 8 */
    uint8_t  wave[4];       /* "WAVE" */
};

struct wav_chunk {
    uint8_t  id[4];         /* "fmt " 或 "data" */
    uint32_t size;          /* chunk 数据大小 */
};

struct wav_fmt {
    uint16_t audio_format;  /* 1 = PCM */
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
};

/* 打开 WAV 并解析到 data 区起点：校验 RIFF/WAVE、扫描 fmt/data chunk，
   输出 PCM 格式与数据长度；fi 停在 data 数据起点（供整块或分块读取）。
   scratch 由调用方提供（SECTOR_SIZE，dosfs 需要）。失败返回负值。 */
static int hda_wav_open(const char *filename, FILEINFO *fi, uint8_t *scratch,
                        uint32_t *data_size_out, uint32_t *rate_out,
                        uint32_t *ch_out, uint32_t *bits_out)
{
    struct wav_header hdr;
    struct wav_chunk   chunk;
    struct wav_fmt     fmt;
    uint32_t read, result;

    /* 逐音播放时 g_hda_quiet=1，不打印（否则每个音符都在串口刷一行） */
    hda_log("HDA: Opening %s...\r\n", filename);

    /* 1. 打开文件 */
    if (DFS_OpenFile(&g_volinfo, (char *)filename, DFS_READ, scratch, fi) != DFS_OK) {
        printk("HDA: Failed to open %s\r\n", filename);
        return -1;
    }
    hda_log("HDA: File opened, size=%u bytes\r\n", fi->filelen);

    /* 2. 读取 RIFF 头 */
    result = DFS_ReadFile(fi, scratch, (uint8_t *)&hdr, &read, sizeof(hdr));
    if (result != DFS_OK || read != sizeof(hdr)) {
        printk("HDA: Failed to read RIFF header\r\n");
        return -1;
    }
    if (memcmp(hdr.riff, "RIFF", 4) != 0 ||
        memcmp(hdr.wave, "WAVE", 4) != 0) {
        printk("HDA: Not a valid WAV file\r\n");
        return -1;
    }
    hda_log("HDA: Valid RIFF/WAVE header\r\n");

    /* 3. 扫描 chunk，找 "fmt " 和 "data" */
    int found_fmt = 0;
    uint32_t data_size = 0;

    while (fi->pointer < fi->filelen) {
        result = DFS_ReadFile(fi, scratch, (uint8_t *)&chunk, &read, sizeof(chunk));
        if (result != DFS_OK || read != sizeof(chunk))
            break;

        if (memcmp(chunk.id, "fmt ", 4) == 0) {
            /* 读取 fmt chunk */
            if (chunk.size < sizeof(fmt)) {
                printk("HDA: fmt chunk too small (%u)\r\n", chunk.size);
                return -1;
            }
            result = DFS_ReadFile(fi, scratch, (uint8_t *)&fmt, &read, sizeof(fmt));
            if (result != DFS_OK || read != sizeof(fmt)) {
                printk("HDA: Failed to read fmt chunk\r\n");
                return -1;
            }
            /* 跳过 fmt chunk 剩余字节（如果有扩展） */
            if (chunk.size > sizeof(fmt))
                DFS_Seek(fi, fi->pointer + chunk.size - sizeof(fmt), scratch);

            hda_log("HDA: fmt: PCM=%u ch=%u rate=%u bits=%u\r\n",
                    fmt.audio_format, fmt.num_channels,
                    fmt.sample_rate, fmt.bits_per_sample);
            found_fmt = 1;
        } else if (memcmp(chunk.id, "data", 4) == 0) {
            data_size = chunk.size;
            hda_log("HDA: data chunk: %u bytes\r\n", data_size);
            break;  /* data 总是在最后 */
        } else {
            /* 跳过未知 chunk */
            hda_log("HDA: Skipping chunk '%.4s' (%u bytes)\r\n", chunk.id, chunk.size);
            DFS_Seek(fi, fi->pointer + chunk.size, scratch);
        }
    }

    if (!found_fmt) {
        printk("HDA: No fmt chunk found!\r\n");
        return -1;
    }
    if (data_size == 0) {
        printk("HDA: No data chunk found!\r\n");
        return -1;
    }
    if (fmt.audio_format != 1) {
        printk("HDA: Unsupported audio format %u (only PCM=1 supported)\r\n",
               fmt.audio_format);
        return -1;
    }

    *data_size_out = data_size;
    *rate_out      = fmt.sample_rate;
    *ch_out        = fmt.num_channels;
    *bits_out      = fmt.bits_per_sample;
    return 0;
}

/* 从磁盘读取 WAV 文件，把 PCM 数据整块读入 kmalloc 缓冲区（适合几百 KB 的
   采样；18MB 级大文件用 sys_audio_play_file_stream_ms 分块流式播放）。
   成功返回 0 并填充各输出参数（*pcm_out 需调用方 kfree），失败返回负值。 */
static int hda_read_wav(const char *filename, uint8_t **pcm_out, uint32_t *len_out,
                        uint32_t *rate_out, uint32_t *ch_out, uint32_t *bits_out)
{
    FILEINFO fi;
    uint8_t scratch[SECTOR_SIZE];
    uint32_t data_size = 0, rate = 0, ch = 0, bits = 0;
    uint32_t read = 0;

    if (hda_wav_open(filename, &fi, scratch, &data_size, &rate, &ch, &bits) != 0)
        return -1;

    uint8_t *pcm_buf = (uint8_t *)kmalloc(data_size);
    if (pcm_buf == NULL) {
        printk("HDA: Failed to alloc %u bytes for PCM\r\n", data_size);
        return -1;
    }

    uint32_t result = DFS_ReadFile(&fi, scratch, pcm_buf, &read, data_size);
    if (result != DFS_OK && result != DFS_EOF) {
        printk("HDA: Failed to read PCM data (result=%u)\r\n", result);
        kfree(pcm_buf);
        return -1;
    }
    hda_log("HDA: Read %u bytes of PCM data\r\n", read);

    *pcm_out = pcm_buf;
    *len_out = read;
    *rate_out = rate;
    *ch_out   = ch;
    *bits_out = bits;
    return 0;
}

/* ===================== 采样文件缓存 ===================== */

/* 每个音符播放前都要从 FAT 磁盘读取 sNN.wav（几十 ms），是音符间间隙的
   可变部分。把解码后的 PCM 缓存起来：命中时直接播放，消除读盘开销
   （乐谱反复使用同一批 ~29 个采样）。条目数覆盖全部采样；
   内核堆已相应扩大到 12MiB（见 startup.c）。 */
#define HDA_WAV_CACHE_MAX  32

typedef struct {
    char      name[16];     /* 文件名，如 "s36.wav" */
    uint8_t  *pcm;          /* PCM 数据（归缓存所有，kmalloc） */
    uint32_t  len, rate, ch, bits;
    uint32_t  last_used;    /* LRU 序号 */
    int       valid;
} hda_wav_cache_t;

static hda_wav_cache_t g_wav_cache[HDA_WAV_CACHE_MAX];
static uint32_t g_wav_cache_seq = 0;

static hda_wav_cache_t *hda_wav_cache_find(const char *name)
{
    for (int i = 0; i < HDA_WAV_CACHE_MAX; i++) {
        if (g_wav_cache[i].valid &&
            strcmp(g_wav_cache[i].name, name) == 0) {
            g_wav_cache[i].last_used = ++g_wav_cache_seq;
            return &g_wav_cache[i];
        }
    }
    return NULL;
}

/* 把新读取的 PCM 放入缓存（缓存从此拥有 pcm，调用者不再释放）。
   槽位满时按 LRU 淘汰并释放被淘汰条目。 */
static hda_wav_cache_t *hda_wav_cache_insert(const char *name, uint8_t *pcm,
                                             uint32_t len, uint32_t rate,
                                             uint32_t ch, uint32_t bits)
{
    int free_slot = -1, lru_slot = -1;
    uint32_t lru_seq = 0xFFFFFFFF;

    for (int i = 0; i < HDA_WAV_CACHE_MAX; i++) {
        if (!g_wav_cache[i].valid) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (g_wav_cache[i].last_used < lru_seq) {
            lru_seq  = g_wav_cache[i].last_used;
            lru_slot = i;
        }
    }

    int slot = (free_slot >= 0) ? free_slot : lru_slot;
    if (slot < 0)
        return NULL;

    if (g_wav_cache[slot].valid)
        kfree(g_wav_cache[slot].pcm);

    strncpy(g_wav_cache[slot].name, name,
            sizeof(g_wav_cache[slot].name) - 1);
    g_wav_cache[slot].name[sizeof(g_wav_cache[slot].name) - 1] = '\0';

    g_wav_cache[slot].pcm       = pcm;
    g_wav_cache[slot].len       = len;
    g_wav_cache[slot].rate      = rate;
    g_wav_cache[slot].ch        = ch;
    g_wav_cache[slot].bits      = bits;
    g_wav_cache[slot].last_used = ++g_wav_cache_seq;
    g_wav_cache[slot].valid     = 1;
    return &g_wav_cache[slot];
}

/* 播放 WAV 文件的前 max_ms 毫秒（max_ms == 0 表示不限时长）。
   用于音符播放：88keyVoice 采样已裁剪为 s{N}.wav，这里按音符时值截断，
   并在截断处做短线性淡出防止爆音。
   - 采样经 hda_wav_cache 缓存：命中免读盘（消除音符间读盘间隙）；
   - 截断+淡出在临时缓冲上进行，不修改缓存（同一采样会以不同时值多次播放）；
   - 使用连续流专用短哨兵（HDA_CONT_SILENCE_*），音符几乎无缝衔接。 */
static int sys_audio_play_file_ms_impl(const char *filename, uint32_t max_ms)
{
    uint8_t *pcm;
    uint32_t len, rate, ch, bits;
    int rc;

    hda_wav_cache_t *ce = hda_wav_cache_find(filename);
    int cached = (ce != NULL);
    if (cached) {
        pcm  = ce->pcm;
        len  = ce->len;
        rate = ce->rate;
        ch   = ce->ch;
        bits = ce->bits;
    } else {
        rc = hda_read_wav(filename, &pcm, &len, &rate, &ch, &bits);
        if (rc != 0) {
            return rc;
        }
        ce = hda_wav_cache_insert(filename, pcm, len, rate, ch, bits);
        cached = (ce != NULL);
        if (cached) {
            pcm  = ce->pcm;
            len  = ce->len;
            rate = ce->rate;
            ch   = ce->ch;
            bits = ce->bits;
        }
    }

    /* 连续流要求 16-bit（环形流格式固定；88keyVoice 样本均为 16-bit） */
    if (bits != 16) {
        printk("HDA: cont mode requires 16-bit PCM\r\n");
        if (!cached)
            kfree(pcm);
        return -2;
    }

    if (max_ms != 0) {
        uint32_t bytes_per_sample = (bits / 8) * ch;
        uint32_t max_samples = (uint32_t)(((uint64_t)rate * max_ms) / 1000);
        if (max_samples == 0)
            max_samples = 1;
        uint32_t keep = max_samples * bytes_per_sample;
        if (keep < len) {
            /* 截断 + ~80ms 线性淡出。缓存缓冲不可原地修改（同一采样会
               以不同时值多次播放），复制到临时缓冲再处理。 */
            uint8_t *work = (uint8_t *)kmalloc(keep);
            if (work == NULL) {
                printk("HDA: cont work alloc failed (%u bytes)\r\n", keep);
                if (!cached)
                    kfree(pcm);
                return -2;
            }
            memcpy(work, pcm, keep);

            uint32_t fade_samples = (uint32_t)((uint64_t)rate * HDA_FADE_MS / 1000);
            if (fade_samples > max_samples)
                fade_samples = max_samples;
            uint32_t start = max_samples - fade_samples;
            int16_t *s = (int16_t *)work;
            for (uint32_t k = 0; k < fade_samples; k++) {
                /* Q8 定点淡出：gain 从 256(=1.0) 线性递减到 0。
                   内核不保存 FPU 状态，禁止用 double/float。 */
                uint32_t gain = 256 - (256 * k / fade_samples);
                for (uint32_t c = 0; c < ch; c++)
                    s[(start + k) * ch + c] =
                        (int16_t)((s[(start + k) * ch + c] * (int32_t)gain) >> 8);
            }

            rc = hda_cont_play_note(rate, ch, bits, work, keep,
                                    HDA_CONT_SILENCE_MS,
                                    HDA_CONT_SILENCE_TAIL_MS);
            kfree(work);
            if (!cached)
                kfree(pcm);
            return rc;
        }
        len = keep;   /* keep >= len：无需截断 */
    }

    /* 无截断：直接播放（hda_cont_play_note 只读 pcm，不改写） */
    rc = hda_cont_play_note(rate, ch, bits, pcm, len,
                            HDA_CONT_SILENCE_MS, HDA_CONT_SILENCE_TAIL_MS);
    if (!cached)
        kfree(pcm);
    return rc;
}

/* 播放 WAV 并在期间抑制逐音进度日志。用 save/restore 而非硬置 0，
   确保所有 return 路径都恢复调用方原值（防止未来新增 return 忘记重置）。 */
int sys_audio_play_file_ms(const char *filename, uint32_t max_ms)
{
    int saved_quiet = g_hda_quiet;
    g_hda_quiet = 1;
    int rc = sys_audio_play_file_ms_impl(filename, max_ms);
    g_hda_quiet = saved_quiet;
    return rc;
}

/* ===================== 流式播放（大文件，syscall 198） =====================
   需求：整曲 WAV（18MB 级）远超内核堆（12MiB），不能整块 kmalloc 载入。
   做法：解析 WAV 头后循环「从 FAT 读 256KB → 追加进连续流环形缓冲」。
   无隙原理：环形缓冲 512KB + 安全余量，追加后缓冲始终领先 DMA 读位置
   1.4s（256KB）以上，而单块读盘耗时仅数百 ms → 读盘被完全吸收，
   分块之间与单音符追加一样首尾相接、无缝衔接。
   内存占用：单个 256KB 读缓冲（进出各一次 kmalloc/kfree），与文件长度无关。
   音量：与单音符路径一致做驱动侧 Q8 增益（QEMU 下 codec amp 不生效）。
   收尾：hda_cont_drain 等待数据末尾进入 MARGIN 后返回，由调用方
   audio_stop() 追加尾静音并停流（保证最后一段完整播放）。
   max_ms=0 播放全曲；非 0 只播放前 max_ms 毫秒（测试用）。 */
#define HDA_STREAM_CHUNK  (256 * 1024)

static int sys_audio_play_file_stream_ms_impl(const char *filename, uint32_t max_ms)
{
    FILEINFO fi;
    uint8_t scratch[SECTOR_SIZE];
    uint32_t data_size = 0, rate = 0, ch = 0, bits = 0;

    if (hda_wav_open(filename, &fi, scratch, &data_size, &rate, &ch, &bits) != 0)
        return -1;
    if (bits != 16) {
        printk("HDA: stream mode requires 16-bit PCM\r\n");
        return -2;
    }
    if (ch < 1 || ch > 2) {
        printk("HDA: stream mode requires 1 or 2 channels\r\n");
        return -2;
    }

    uint32_t total = data_size;
    if (max_ms != 0) {
        uint32_t bps = (bits / 8) * ch;
        uint32_t max_bytes = (uint32_t)(((uint64_t)rate * max_ms) / 1000) * bps;
        if (max_bytes < total)
            total = max_bytes;
    }

    int rc = hda_cont_start(rate, ch, bits, HDA_CONT_BUF_SIZE_STREAM);
    if (rc != 0)
        return rc;

    uint8_t *buf = (uint8_t *)kmalloc(HDA_STREAM_CHUNK);
    if (buf == NULL) {
        printk("HDA: stream buf alloc failed (%u bytes)\r\n", HDA_STREAM_CHUNK);
        return -2;
    }

    /* 时长用整数运算（内核不保存 FPU 状态，禁止 float/double） */
    uint32_t dur_ms = (uint32_t)(((uint64_t)total * 1000) /
                                 ((uint64_t)rate * (bits / 8) * ch));
    printk("HDA: streaming %s: %u bytes (%u ms), %uch %uHz\r\n",
           filename, total, dur_ms, ch, rate);

    uint32_t remaining = total;
    uint32_t chunks = 0;
    int err = 0;
    while (remaining > 0) {
        uint32_t want = (remaining > HDA_STREAM_CHUNK)
                        ? HDA_STREAM_CHUNK : remaining;
        uint32_t got = 0;
        uint32_t res = DFS_ReadFile(&fi, scratch, buf, &got, want);
        if ((res != DFS_OK && res != DFS_EOF) || got == 0) {
            printk("HDA: stream read failed at %u/%u (result=%u)\r\n",
                   total - remaining, total, res);
            err = -3;
            break;
        }

        /* 驱动侧 Q8 增益（与 hda_cont_play_note 一致；就地在读缓冲上缩放） */
        if (g_audio_gain_q8 == 0) {
            memset(buf, 0, got);
        } else if (g_audio_gain_q8 < 256) {
            int16_t *s = (int16_t *)buf;
            for (uint32_t i = 0; i < got / 2; i++)
                s[i] = (int16_t)((s[i] * (int32_t)g_audio_gain_q8) >> 8);
        }

        rc = hda_cont_append(buf, got);
        if (rc != 0) {
            printk("HDA: stream append failed (%d)\r\n", rc);
            err = rc;
            break;
        }
        remaining -= got;
        chunks++;
    }

    kfree(buf);

    /* 收尾：等最后一块播完（剩余进 MARGIN）再返回，避免 stop 截断尾部 */
    hda_cont_drain();

    printk("HDA: stream %s (%u chunks, %u bytes, underruns=%u)\r\n",
           err ? "aborted" : "done", chunks, total - remaining,
           sys_audio_get_underruns(0));
    return err;
}

/* 流式播放入口：播放期间抑制逐音日志（同 play_file_ms 的 save/restore） */
int sys_audio_play_file_stream_ms(const char *filename, uint32_t max_ms)
{
    int saved_quiet = g_hda_quiet;
    g_hda_quiet = 1;
    int rc = sys_audio_play_file_stream_ms_impl(filename, max_ms);
    g_hda_quiet = saved_quiet;
    return rc;
}

/* 同时播放多个采样（旋律 + 左手伴奏），把各路 PCM 解码后逐样本相加
   （每路 /n 防止削波、int16 钳位），混合成一路写入连续流。
   - 复用 wav 缓存（命中免读盘）；所有采样须同 rate/ch/bits（均为 44.1k 立体声 16-bit）。
   - files 为用户态指针数组（最多 4 路），每个元素指向 "sNN.wav" 字符串。
   - ms 为混合后的音符时长，按最长采样截断前 ms 毫秒并做短淡出防爆音。 */
static int sys_audio_play_voices_ms_impl(const char **files, uint32_t n, uint32_t ms)
{
    uint32_t max_voices = HDA_MAX_VOICES;
    const char *ufiles[HDA_MAX_VOICES];
    uint8_t  *pcm[HDA_MAX_VOICES];
    uint32_t  len[HDA_MAX_VOICES], rate[HDA_MAX_VOICES], ch[HDA_MAX_VOICES], bits[HDA_MAX_VOICES];

    if (n == 0)
        return 0;
    if (n > max_voices)
        n = max_voices;

    /* 把用户态指针数组拷进内核并校验每个文件名指针合法 */
    if (!IN_USER_VM(files, n * sizeof(const char *)))
        return -1;
    memcpy(ufiles, files, n * sizeof(const char *));
    for (uint32_t i = 0; i < n; i++) {
        if (!IN_USER_VM(ufiles[i], 0))
            return -1;
    }

    uint32_t rate0 = 0, ch0 = 0, bits0 = 0;
    int ok = 1;
    for (uint32_t i = 0; i < n; i++) {
        hda_wav_cache_t *ce = hda_wav_cache_find(ufiles[i]);
        if (ce == NULL) {
            uint8_t *p; uint32_t l, r, c, b;
            if (hda_read_wav(ufiles[i], &p, &l, &r, &c, &b) != 0) {
                ok = 0; break;
            }
            ce = hda_wav_cache_insert(ufiles[i], p, l, r, c, b);
            if (ce == NULL) {
                kfree(p); ok = 0; break;
            }
        }
        pcm[i]  = ce->pcm;
        len[i]  = ce->len;
        rate[i] = ce->rate;
        ch[i]   = ce->ch;
        bits[i] = ce->bits;
        if (i == 0) {
            rate0 = rate[i]; ch0 = ch[i]; bits0 = bits[i];
        } else if (rate[i] != rate0 || ch[i] != ch0 || bits[i] != bits0) {
            ok = 0; break;   /* 多路格式须一致 */
        }
    }
    if (!ok) {
        return -2;
    }
    if (bits0 != 16) {
        printk("HDA: cont mode requires 16-bit PCM\r\n");
        return -2;
    }

    uint32_t bytes_per_sample = (bits0 / 8) * ch0;
    uint32_t max_samples = (uint32_t)(((uint64_t)rate0 * ms) / 1000);
    if (max_samples == 0)
        max_samples = 1;
    uint32_t total = max_samples * bytes_per_sample;

    uint8_t *mixed = (uint8_t *)kmalloc(total);
    if (mixed == NULL) {
        printk("HDA: voices mix alloc failed (%u bytes)\r\n", total);
        return -2;
    }
    memset(mixed, 0, total);

    int16_t *dst = (int16_t *)mixed;
    for (uint32_t s = 0; s < max_samples; s++) {
        for (uint32_t c = 0; c < ch0; c++) {
            /* 旋律(第 0 路)保持原音量，伴奏(后续各路)取一半，
               再 /2 归约：melody + 0.5*accomp，避免削波且旋律突出 */
            int32_t sum = 0;
            for (uint32_t v = 0; v < n; v++) {
                uint32_t vsamples = len[v] / bytes_per_sample;
                if (s < vsamples) {
                    int16_t sv = ((int16_t *)pcm[v])[s * ch0 + c];
                    sum += (v == 0) ? ((int32_t)sv * 2) : (int32_t)sv;
                }
            }
            sum /= 2;
            if (sum > 32767) sum = 32767;
            if (sum < -32768) sum = -32768;
            dst[s * ch0 + c] = (int16_t)sum;
        }
    }

    /* 末尾线性淡出防爆音 */
    uint32_t fade_samples = (uint32_t)((uint64_t)rate0 * HDA_FADE_MS / 1000);
    if (fade_samples > max_samples)
        fade_samples = max_samples;
    uint32_t fstart = max_samples - fade_samples;
    for (uint32_t k = 0; k < fade_samples; k++) {
        /* Q8 定点淡出：内核不保存 FPU 状态，禁止用 double/float */
        uint32_t gain = 256 - (256 * k / fade_samples);
        for (uint32_t c = 0; c < ch0; c++)
            dst[(fstart + k) * ch0 + c] =
                (int16_t)((dst[(fstart + k) * ch0 + c] * (int32_t)gain) >> 8);
    }

    int rc = hda_cont_play_note(rate0, ch0, bits0, mixed, total,
                                HDA_CONT_SILENCE_MS, HDA_CONT_SILENCE_TAIL_MS);
    kfree(mixed);
    return rc;
}

/* 多路混音播放并抑制逐音进度日志。save/restore 确保所有 return 路径恢复原值。 */
int sys_audio_play_voices_ms(const char **files, uint32_t n, uint32_t ms)
{
    int saved_quiet = g_hda_quiet;
    g_hda_quiet = 1;
    int rc = sys_audio_play_voices_ms_impl(files, n, ms);
    g_hda_quiet = saved_quiet;
    return rc;
}

/* 播放用户态提供的原始 PCM（音频质量评估用的测试信号：正弦/冲激/阶跃）。
   与音符播放路径的区别仅在于数据来源：这里 PCM 由用户态直接给出，
   而不是从 FAT 磁盘读 WAV。走同一连续流，DMA 传输链路完全一致，
   因此 FFT/THD/SNR 测得的失真即驱动链路的失真。 */
int sys_audio_play_pcm_ms(const void *pcm, uint32_t len,
                          uint32_t rate, uint32_t ch,
                          uint32_t bits, uint32_t ms)
{
    if (pcm == NULL || len == 0)
        return -1;

    /* 连续流要求 16-bit（环形流格式固定，同音符路径） */
    if (bits != 16) {
        printk("HDA: cont mode requires 16-bit PCM\r\n");
        return -2;
    }

    /* 若给了时长，按 rate/ms 折算期望样本数并截断（调用方通常已生成精确长度） */
    if (ms != 0) {
        uint32_t bytes_per_sample = (bits / 8) * ch;
        uint32_t max_samples = (uint32_t)(((uint64_t)rate * ms) / 1000);
        uint32_t keep = max_samples * bytes_per_sample;
        if (keep != 0 && keep < len)
            len = keep;
    }

    uint8_t *kbuf = (uint8_t *)kmalloc(len);
    if (kbuf == NULL) {
        printk("HDA: test pcm alloc failed (%u bytes)\r\n", len);
        return -2;
    }
    memcpy(kbuf, pcm, len);

    int saved_quiet = g_hda_quiet;
    g_hda_quiet = 1;
    int rc = hda_cont_play_note(rate, ch, bits, kbuf, len,
                                HDA_CONT_SILENCE_MS, HDA_CONT_SILENCE_TAIL_MS);
    g_hda_quiet = saved_quiet;

    kfree(kbuf);
    return rc;
}
