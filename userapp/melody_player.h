/*
 * vim: filetype=c:fenc=utf-8:ts=4:et:sw=4:sts=4
 *
 * melody_player —— 排序可视化钢琴音播放模块
 *
 * 由 shell_sort 的每次 swap 调用 trigger_next_sort_note()，从
 * alicia_score（定义于 NoteSequence.c）中依次取出音符并播放。
 */
#ifndef _MELODY_PLAYER_H
#define _MELODY_PLAYER_H

#include <inttypes.h>

typedef struct {
    uint8_t     note;      /* MIDI note number (0 for rest) */
    uint8_t     dur;       /* Duration in 16th notes */
    uint8_t     hand;      /* 0 = Left Hand (Bass), 1 = Right Hand (Melody) */
    const char *wav;       /* 88keyVoice 采样文件名（/s{N}.wav，N = MIDI note；休止符为 0） */
} PianoEvent;

extern const PianoEvent alicia_score[];
extern const uint32_t  alicia_score_len;

/* 左手伴奏：每个小节(0-based)的根音+五度（0 结尾），直接播 samples/sNN.wav */
extern const int8_t  alicia_accomp[][3];
extern const uint32_t alicia_accomp_len;

/* 由排序算法的 swap 操作调用：播放当前音符（阻塞到播完，停留==播放时长） */
void trigger_next_sort_note(void);

/* 排序结束后补齐剩余音符，保证整首乐谱只播一遍、完整播完 */
void play_score_to_end(void);

/* 演示开始前清零播放进度（索引与小节位置） */
void reset_score_playback(void);

#endif /* _MELODY_PLAYER_H */
