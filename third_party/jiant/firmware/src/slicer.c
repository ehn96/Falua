/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* SLICER (JIANT 0.6.3: one, on a bus): a tempo-synced 16-step gate / stutter on the tracks routed to it (SL BUS: T1..T6
 * ON / OFF) and, with FX ON, on the delay, reverb and chorus returns too: everything ON, the whole mix's stutter after the
 * effects, a sampler's cut. The routed tracks reach it after their DIST, filter, LEVEL and PAN (fx.c mix_part: into
 * sl_l / sl_r, not the dry mix); their sends stay as they were (the effects get them unchopped: with FX OFF the tails
 * ring free over the cut). The bus goes into the mix before the master's clip and leveler. Globals (core.h G_SLMODE..):
 *   SLCR  OFF / GATE / STUT      PAT   one of SL_NPAT patterns of 16 steps ('x' live / open, '.' gated / repeated)
 *   RATE  the step: 1/8, 1/16, 1/32, 8T, 16T, 32T    DEPTH  GATE: how far a '.' step closes; STUT: the repeat's level
 *   SL PITCH: the repeats' pitch, a 16-step sequence (LEN steps of the pattern's, semitones, QNT SCL: snapped to the
 *   song's scale), played as a tape: faster up, slower down; the loop keeps the step's rhythm.
 * GATE: the gain moves at most 1 / SL_RAMP per sample (2.9 ms from open to closed): a closing ramp ends on the step
 *   boundary, an opening one starts on it.
 * STUT: an 'x' step plays live and is recorded (stereo, 22.05 kHz, 16 bit, SL_FR frames: 371 ms); a '.' step plays that
 *   recording from its start, looped at the step length (halved until it fits the recording; up a pitch, as much as it
 *   holds), cross-faded with the live sound by DEPTH. Every pass of the loop is windowed (SL_RAMP at both ends and at
 *   the step end), the cross-fade moves as the gate does.
 * The step clock: always running (also with the SLICER OFF, so switching it on lands in time), on the transport's grid
 * while it plays, the global SWING; sample exact. With the SLICER OFF and no ramp left nothing goes through it.
 * perform.c borrows sl_buf as its REPEAT loop (sl_lent): STUT plays live meanwhile. */
#define SL_NPAT 16
#define SL_FR 8192u                     /* the recording: stereo frames, 22.05 kHz, 371 ms, 32 KB (a power of 2) */
#define SL_RAMP_LOG2 7
#define SL_RAMP (1 << SL_RAMP_LOG2)     /* 128 samples, 2.9 ms */
#define SL_SLOPE (32768 / SL_RAMP)
enum { SL_OFF, SL_GATE, SL_STUT };      /* G_SLMODE (params.c N_SLCR) */

/* bit k = step k: 1 = 'x' (open / live), 0 = '.' (closed / repeat) */
static const uint16_t SL_PAT[SL_NPAT] = {
    0x5555,   /*  1 x.x.x.x.x.x.x.x. */
    0xB6DB,   /*  2 xx.xx.xx.xx.xx.x */
    0x5249,   /*  3 x..x..x..x..x.x. */
    0xB777,   /*  4 xxx.xxx.xxx.xx.x */
    0x6D6D,   /*  5 x.xx.xx.x.xx.xx. */
    0x0F0F,   /*  6 xxxx....xxxx.... */
    0x1111,   /*  7 x...x...x...x... */
    0x54A5,   /*  8 x.x..x.x..x.x.x. */
    0x5333,   /*  9 xx..xx..xx..x.x. */
    0xA929,   /* 10 x..x.x..x..x.x.x */
    0xAAFF,   /* 11 xxxxxxxx.x.x.x.x */
    0xD501,   /* 12 x.......x.x.x.xx */
    0xAAAA,   /* 13 .x.x.x.x.x.x.x.x */
    0xAB6B,   /* 14 xx.x.xx.xx.x.x.x */
    0xBB5D,   /* 15 x.xxx.x.xx.xxx.x */
    0x7597,   /* 16 xxx.x..xx.x.xxx. */
};
static const uint8_t SL_DEN[6] = {2, 4, 8, 3, 6, 12};   /* G_SLRATE (N_SLDIV): a step = 1 / DEN beats */
static const uint32_t SL_RATIO[49] = {                  /* 2^(s / 12), Q16, s = -24 .. 24 */
    16384, 17358, 18390, 19484, 20643, 21870, 23170, 24548, 26008, 27554, 29193, 30929, 32768, 34716, 36781, 38968,
    41285, 43740, 46341, 49097, 52016, 55109, 58386, 61858, 65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193,
    104032, 110218, 116772, 123715, 131072, 138866, 147123, 155872, 165140, 174960, 185364, 196386, 208064, 220436,
    233544, 247431, 262144};

static int16_t sl_buf[SL_FR][2] __attribute__((section(".pool")));
typedef struct {
    uint32_t pos, len;           /* samples into the step, its length */
    uint32_t base;               /* the step without swing */
    uint32_t rp, loop;           /* repeat: output samples into the loop, its length (44.1 kHz), 0 = none */
    uint32_t rq, inc;            /* .. the read position (frames, Q16) and its step a sample (the pitch) */
    uint32_t rec;                /* recorded frames */
    int32_t gc;                  /* gate closure, Q15: 0 = open */
    int32_t w;                   /* STUT cross-fade, Q15: 0 = live */
    int32_t hl, hr;              /* recording: the first sample of a pair */
    uint8_t idx;                 /* step 0..15 */
    uint8_t bit;                 /* this step's pattern bit (latched at its start) */
    uint8_t rec_on;              /* recording this step */
} sl_t;
static sl_t sl;
static uint8_t sl_lent;          /* perform.c has borrowed sl_buf: STUT plays live, records nothing */
static int32_t sl_l[CTL], sl_r[CTL];   /* the bus, a block (fx.c mix_block) */

static void slicer_reset(void) { memset(&sl, 0, sizeof sl); }
static void slicer_start(void)   /* seq_start: the next block starts step 0 */
{
    sl.idx = 15;
    sl.pos = sl.len = 0;
}
static void slicer_drop(void)    /* the recording dropped (perform.c takes the buffer, gives it back) */
{
    sl.rec = sl.loop = sl.rec_on = 0;
}

static uint32_t sl_pattern(void) { return SL_PAT[(uint32_t)(song.g[G_SLPAT] - 1) % SL_NPAT]; }
/* running or still fading: the routed tracks go through it */
static int slicer_on(void) { return song.g[G_SLMODE] != SL_OFF || sl.gc || sl.w; }
static int slicer_routes(uint32_t k) { return k < NTRK && song.g[G_SLT1 + k] && slicer_on(); }
static int slicer_fx(void) { return song.g[G_SLFX] && slicer_on(); }

static uint32_t scale_mask(const track_t *t);           /* seq.c */
/* the pitch step of pattern step i: semitones (QNT SCL: the nearest of the song's scale, the first melodic track's) */
static int32_t sl_pitch(uint32_t i)
{
    uint32_t len = (uint32_t)clamp(song.g[G_SLPLEN], 1, 16), k, mask = 0xFFFu;
    int32_t s = clamp(song.g[G_SLP0 + i % len], -24, 24), d;
    if (!song.g[G_SLPQ] || !s)
        return s;
    for (k = 0; k < NTRK; k++)
        if (ENGINES[eng_idx(trk[k].eng_req)] != &ENG_DRUM) {
            mask = scale_mask(&trk[k]) & 0xFFFu;
            break;
        }
    for (d = 0; d < 12 && mask; d++) {             /* (a tie: the lower) */
        if ((mask >> (uint32_t)((s - d + 120) % 12)) & 1u) return clamp(s - d, -24, 24);
        if ((mask >> (uint32_t)((s + d + 120) % 12)) & 1u) return clamp(s + d, -24, 24);
    }
    return s;
}

/* the step clock enters the next step, at sample at of the block. While the transport runs it is the transport's grid
 * (mod.c clk_n / clk_pos: the 1/16 steps since PLAY, through tempo changes and an external clock): the step and the
 * place in it from there, so a SLICER switched on, its RATE changed or the tempo moved lands on the bar at once. Swing
 * pairs two steps: their pair from the grid, the step in it */
static __attribute__((noinline)) void sl_enter(sl_t *s, uint32_t at)
{
    uint32_t mode = (uint32_t)song.g[G_SLMODE], den = SL_DEN[(uint32_t)song.g[G_SLRATE] % 6u];
    int32_t sw;
    s->idx = (uint8_t)((s->idx + 1u) & 15u);
    s->base = (uint32_t)FS * 60u / (uint32_t)song.g[G_BPM] / den;
    s->pos = 0;
    sw = clamp(song.g[G_SWING], 0, SWING_MAX);
    if (song.playing && clk_pos != CLK_START) {
        uint32_t p = div_samples(2), q = 4u * p, a, k, r, b, rp, l0;
        a = ((clk_n % 192u) * p + clk_pos + at) * den;  /* (192 1/16s: a whole number of every RATE's 16 steps) */
        k = a / q;                                      /* the step (no swing), exact in 1/(4p den) */
        r = (a % q) / den;                              /* .. samples into it */
        b = q / den;
        s->base = b;
        l0 = b + (uint32_t)(sw * (int32_t)b / 250);
        rp = (k & 1u ? b : 0u) + r;                     /* into the swung pair */
        k &= ~1u;
        if (2u * b - rp <= 16u) {                       /* (a hair before a step's start: that start) */
            k += 2u;
            rp = 0;
        } else if (rp < l0 && l0 - rp <= 16u) {
            rp = l0;
        }
        s->idx = (uint8_t)((k + (rp >= l0)) & 15u);
        s->pos = rp >= l0 ? rp - l0 : rp;
    }
    sw = sw * (int32_t)s->base / 250;
    s->len = s->base + (uint32_t)((s->idx & 1u) ? -sw : sw);   /* (as core.h swing_step_len, the global SWING) */
    if (s->pos >= s->len)
        s->pos = s->len - 1u;
    s->bit = (uint8_t)((sl_pattern() >> s->idx) & 1u);
    s->rp = s->rq = 0;
    s->loop = 0;
    s->rec_on = 0;
    if (mode != SL_STUT || sl_lent) {
        s->rec = 0;                                 /* nothing old to repeat when STUT comes on */
    } else if (s->bit) {
        s->rec = 0;                                 /* a live step: record it */
        s->rec_on = 1;
    } else if (s->rec >= 2u * SL_RAMP) {            /* a repeat: the last live step, looped at the step's pitch */
        uint32_t l = s->base, ratio = SL_RATIO[sl_pitch(s->idx) + 24], most;
        while (l > 2u * SL_FR)
            l >>= 1;                                /* (a long step: a half, a quarter .. of it) */
        most = (uint32_t)(((uint64_t)s->rec << 17) / ratio);   /* (output samples the recording holds at this pitch) */
        s->loop = l < most ? l : most;
        s->inc = ratio >> 1;                        /* (frames a 44.1 kHz sample, Q16: 1/2 at the pitch as played) */
    }
}

/* m samples of one step (no boundary inside), the bus l / r */
static void sl_seg(sl_t *s, int32_t *bl, int32_t *br, uint32_t m)
{
    uint32_t mode = (uint32_t)song.g[G_SLMODE], j, nbit = (sl_pattern() >> ((s->idx + 1u) & 15u)) & 1u;
    int32_t depth = song.g[G_SLDEP] * 258;          /* Q15, 0..32766 */
    int32_t tc = mode == SL_GATE && !s->bit ? depth : 0;           /* gate closure of this step */
    int32_t tn = mode == SL_GATE && !nbit ? depth : 0;             /* .. of the next one */
    int32_t tw = mode == SL_STUT && s->loop ? depth : 0;           /* repeat level */
    for (j = 0; j < m; j++) {
        int32_t xl = bl[j], xr = br[j], yl, yr, tg = tc;
        uint32_t left = s->len - s->pos - j;        /* samples to the step end, >= 1 */
        if (left <= (uint32_t)SL_RAMP && tn > tg)
            tg = tn;                                /* close by the boundary */
        s->gc += clamp(tg - s->gc, -SL_SLOPE, SL_SLOPE);
        s->w += clamp(tw - s->w, -SL_SLOPE, SL_SLOPE);
        yl = s->gc ? xl - mulq16(xl, (uint32_t)s->gc << 1) : xl;
        yr = s->gc ? xr - mulq16(xr, (uint32_t)s->gc << 1) : xr;
        if (s->rec_on) {                            /* 2:1, the pair's mean; >> 3: the mix's headroom */
            if ((s->pos + j) & 1u) {
                sl_buf[s->rec][0] = (int16_t)clamp((s->hl + xl) >> 3, -32768, 32767);
                sl_buf[s->rec][1] = (int16_t)clamp((s->hr + xr) >> 3, -32768, 32767);
                if (++s->rec >= SL_FR)
                    s->rec_on = 0;
            } else {
                s->hl = xl;
                s->hr = xr;
            }
        }
        if (s->loop) {                              /* the repeat, windowed, read between frames at its pitch */
            uint32_t f = s->rq >> 16, fr = (s->rq >> 1) & 0x7FFFu, e = s->loop - 1u - s->rp, f1;
            int32_t rl, rr;
            if (f >= s->rec)
                f = s->rec - 1u;
            f1 = f + 1u < s->rec ? f + 1u : f;
            rl = sl_buf[f][0] + (((sl_buf[f1][0] - sl_buf[f][0]) * (int32_t)fr) >> 15);
            rr = sl_buf[f][1] + (((sl_buf[f1][1] - sl_buf[f][1]) * (int32_t)fr) >> 15);
            rl <<= 2;
            rr <<= 2;
            if (s->rp < e)
                e = s->rp;
            if (left - 1u < e)
                e = left - 1u;
            if (e < (uint32_t)SL_RAMP) {
                rl = (rl * (int32_t)e) >> SL_RAMP_LOG2;
                rr = (rr * (int32_t)e) >> SL_RAMP_LOG2;
            }
            s->rq += s->inc;
            if (++s->rp >= s->loop)
                s->rp = s->rq = 0;
            yl += mulq16(rl - yl, (uint32_t)s->w << 1);
            yr += mulq16(rr - yr, (uint32_t)s->w << 1);
        } else if (s->w) {
            yl -= mulq16(yl, (uint32_t)s->w << 1);  /* (the repeat ended on a boundary: it is 0 there) */
            yr -= mulq16(yr, (uint32_t)s->w << 1);
        }
        bl[j] = yl;
        br[j] = yr;
    }
}

/* the step clock over n samples and the SLICER on the bus l / r (fx.c mix_block: every block, on or not) */
static __attribute__((noinline)) void slicer_bus(int32_t *bl, int32_t *br, uint32_t n)
{
    uint32_t i = 0;
    int act = slicer_on();
    while (i < n) {
        uint32_t m;
        if (sl.pos >= sl.len)
            sl_enter(&sl, i);
        m = sl.len - sl.pos;
        if (m > n - i)
            m = n - i;
        if (act)
            sl_seg(&sl, bl + i, br + i, m);
        sl.pos += m;
        i += m;
    }
}
