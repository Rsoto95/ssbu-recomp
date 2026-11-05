/* Rollback netcode: keeping the two frame clocks together (rb.h). Every input message gives a sample of how far this
 * side runs ahead of the peer; every 30 frames the mean of the middle third of the last 30 samples decides:
 *   ahead by 8 ms or more: run 0.5 % slower (and, in the first 120 frames only, skip one frame when a whole frame ahead);
 *   behind by 8 ms or more: run 1 % faster, and when a whole frame behind run up to 3 extra frames (the caller spreads
 *   them, one per 5 frames). The thresholds follow Slippi's netplay (study N1b section 6). SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>
#include "../rollback.h"

#define SAMPLE_COUNT 30                    /* offset samples kept (one per input message) */
static int32_t g_samples[SAMPLE_COUNT];    /* ring of the latest offsets, in microseconds (positive = this side is ahead) */
static int g_nsamples, g_next_sample, g_frame_us = 16683;     /* samples held, ring write position, microseconds per frame */
static int32_t g_estimate;                 /* the last decision's estimate of the offset, microseconds */

/* Starts a match: forgets every sample and the estimate. `frame_us` is the length of one frame in microseconds (16683 = 59.94 Hz when not given). */
void rb_ts_reset(int frame_us) { memset(g_samples, 0, sizeof g_samples); g_nsamples = g_next_sample = 0; g_estimate = 0; g_frame_us = frame_us > 0 ? frame_us : 16683; }
/* Records one measurement of how far ahead of the peer this side's frame clock runs (microseconds, negative = behind). Keeps the latest SAMPLE_COUNT. */
void rb_ts_sample(int32_t offset_us) { g_samples[g_next_sample] = offset_us; g_next_sample = (g_next_sample + 1) % SAMPLE_COUNT; if (g_nsamples < SAMPLE_COUNT) g_nsamples++; }
/* (2026-11-05) Forgets the samples (the estimate stays): called when a correction starts, so the next decision is made from
 * samples taken after it and not from the old lead again (an estimate over 3 s of pings kept slowing a game already caught up). */
void rb_ts_forget(void) { g_nsamples = g_next_sample = 0; }
/* The estimate of how far ahead of the peer this side is, as of the last decision (microseconds). */
int32_t rb_ts_offset(void) { return g_estimate; }

/* qsort order for int32_t values, ascending. */
static int cmp32(const void *a, const void *b) { int32_t x = *(const int32_t *)a, y = *(const int32_t *)b; return (x > y) - (x < y); }

/* Called once per frame. On every 30th frame (and with at least 3 samples) it estimates the offset as the mean of the middle third
 * of the samples (sorted, so spikes at both ends are ignored) and answers what to do about it:
 *   ahead by 8 ms or more: speed 0.995 (run 0.5 % slower), and in the first 120 frames also skip one frame when a whole frame ahead;
 *   behind by 8 ms or more: speed 1.01 (1 % faster), and when a whole frame behind, run up to 3 extra frames;
 *   otherwise: nothing (speed 1.0). On the frames in between it always answers nothing. */
RbSyncAction rb_ts_decide(int frame) {
    RbSyncAction a = {0, 0, 1.0};
    if (frame % 30 || g_nsamples < 3) return a;
    int32_t v[SAMPLE_COUNT];
    memcpy(v, g_samples, sizeof(int32_t) * (size_t)g_nsamples);
    qsort(v, (size_t)g_nsamples, sizeof v[0], cmp32);
    int lo = g_nsamples / 3, hi = g_nsamples - g_nsamples / 3;
    int64_t sum = 0;
    for (int i = lo; i < hi; i++) sum += v[i];
    g_estimate = (int32_t)(sum / (hi - lo));
    if (g_estimate >= 8000) {
        a.speed = 0.995;
        if (frame <= 120 && g_estimate >= g_frame_us) a.skip = 1;
    } else if (g_estimate <= -8000) {
        a.speed = 1.01;
        if (g_estimate <= -g_frame_us) { a.advance = (int)(-g_estimate / g_frame_us); if (a.advance > 3) a.advance = 3; }
    }
    return a;
}
