/* 有效段裁剪实现，见 segment.h。只做整数运算，不依赖 libm。 */
#include <stddef.h>

#include "segment.h"

static int abs16(short v)
{
    return (v < 0) ? -(int)v : (int)v;
}

int seg_find_span(const short *s, int n, int thresh, int *out_start, int *out_len)
{
    int start = -1;
    int end = -1;

    if (s == NULL || n <= 0)
        return -1;

    if (thresh < 0)
        thresh = 0;

    for (int i = 0; i < n; i++) {
        if (abs16(s[i]) >= thresh) {
            start = i;
            break;
        }
    }
    if (start < 0)
        return -2;

    for (int i = n - 1; i >= start; i--) {
        if (abs16(s[i]) >= thresh) {
            end = i;
            break;
        }
    }
    if (end < start)
        return -2;

    if (end - start + 1 < SEG_MIN_SAMPLES)
        return -3;

    *out_start = start;
    *out_len   = end - start + 1;
    return 0;
}

int seg_thresh_for_peak(int peak)
{
    int t;

    if (peak <= 0)
        return SEG_FLOOR;

    t = peak / 8;
    return (t > SEG_FLOOR) ? t : SEG_FLOOR;
}

int seg_find_loudest(const short *s, int n, int win, int *out_start)
{
    int step, best = 0;
    unsigned long long best_e = 0, cur = 0;

    if (s == NULL || out_start == NULL || win <= 0 || n < win)
        return -1;

    step = win / 2;
    if (step <= 0)
        step = 1;

    for (int i = 0; i < win; i++) {
        int v = (int)s[i];
        cur += (unsigned long long)(v * v);
    }
    best_e = cur;

    for (int i = step; i + win <= n; i += step) {
        for (int k = 0; k < step; k++) {
            int vo = (int)s[i - step + k];
            int vi = (int)s[i + win - step + k];
            cur -= (unsigned long long)(vo * vo);
            cur += (unsigned long long)(vi * vi);
        }
        if (cur > best_e) {
            best_e = cur;
            best = i;
        }
    }

    *out_start = best;
    return 0;
}
