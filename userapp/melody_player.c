/*
 * vim: filetype=c:fenc=utf-8:ts=4:et:sw=4:sts=4
 */
#include <stdio.h>
#include <syscall.h>
#include <time.h>
#include <stddef.h>
#include "melody_player.h"

/* 全局索引：指向 alicia_score 中下一个待播放的音符 */
static uint32_t current_note_idx = 0;

/* 当前乐谱（NoteSequence.c）为 Oh! Susanna：2/4 拍 120 BPM */
#define MELODY_BPM 120
static uint32_t current_bpm(void)
{
    return MELODY_BPM;
}

/* 当前绝对位置（十六分音符），用于判断拍点 / 乐句呼吸 */
static uint32_t g_pos16 = 0;

/* 把 MIDI 音高格式化为 "sNN.wav" 文件名（写入调用者提供的缓冲，至少 16 字节）。
    返回缓冲指针。note<=0 时返回空串。 */
static const char *make_wav_name(int note, char *buf)
{
    if (note <= 0) {
        buf[0] = '\0';
        return buf;
    }
    buf[0] = 's';
    int i = 1, t = note;
    char tmp[8];
    int ti = 0;
    while (t > 0) {
        tmp[ti++] = (char)('0' + (t % 10));
        t /= 10;
    }
    while (ti > 0)
        buf[i++] = tmp[--ti];
    buf[i++] = '.'; buf[i++] = 'w'; buf[i++] = 'a'; buf[i++] = 'v';
    buf[i]   = '\0';
    return buf;
}

/* 播放 alicia_score[current_note_idx] 当前音符并推进索引/位置。
   - 时值 ms 由 dur 按当前 BPM 换算；内核连续流阻塞到播完才返回，
     因此调用方在音符之后的“停留时间”严格 == 播放时长。
   - 每拍（每 4 个十六分音符，pos16%4==0）把该小节左手伴奏和弦与旋律
     混音同时发声（audio_play_voices_ms）；休止符写等长静音保持节奏。
   - 主歌 -> 副歌之间（pos16==128）插一段“呼吸”静音。 */
static void play_current_note(void)
{
    /* 整首已播完（排序交换数多于音符数）：后续 swap 不再发声，避免越界 */
    if (current_note_idx >= alicia_score_len)
        return;

    const PianoEvent *ev = &alicia_score[current_note_idx];
    uint32_t bpm = current_bpm();
    uint32_t ms  = (uint32_t)((ev->dur * 60000UL) / (bpm * 4U));
    if (ms == 0)
        ms = 1;

    /* 乐句呼吸：主歌末尾 -> 副歌起始（第 17 小节 = pos16 128） */
    if (g_pos16 == 128)
        audio_play_silence_ms(150);

    if (ev->note == 0 || ev->wav == 0) {
        audio_play_silence_ms(ms);
    } else {
        const char *voices[4];
        uint32_t n = 0, bn = 0;
        char     names[4][16];
        voices[n++] = make_wav_name(ev->note, names[bn++]);   /* 旋律(右手) */

        /* 每拍叠加左手伴奏和弦（oom 伴奏） */
        if ((g_pos16 % 4) == 0) {
            uint32_t m = g_pos16 / 8;   /* 小节号 0-based */
            if (m < alicia_accomp_len) {
                for (int k = 0; alicia_accomp[m][k] > 0 && n < 4; k++)
                    voices[n++] = make_wav_name(alicia_accomp[m][k], names[bn++]);
            }
        }

        printf("[MELODY] #%u note=%u ms=%u voices=%u %s\r\n",
               current_note_idx, ev->note, ms, n,
               ev->wav ? ev->wav : "rest");

        audio_play_voices_ms(voices, n, ms);
    }

    current_note_idx++;
    g_pos16 += ev->dur;
}

/* 由排序 swap 调用：播放当前音符（阻塞到播完 -> 停留 == 播放时长）。 */
void trigger_next_sort_note(void)
{
    play_current_note();
}

/* 排序结束后补齐剩余音符，保证整首乐谱只播一遍、完整播完。 */
void play_score_to_end(void)
{
    while (current_note_idx < alicia_score_len)
        play_current_note();
    audio_play_silence_ms(500);   /* 曲终留白，避免 audio_stop 硬切 */
}

/* 演示开始前清零播放进度。 */
void reset_score_playback(void)
{
    current_note_idx = 0;
    g_pos16 = 0;
}
