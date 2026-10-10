/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Effects: per-track DIST insert, then sends into three
 * shared buses (chorus, tempo delay, reverb). Mono buses, stereo dry mix. */
#define DLY_LEN 65536u           /* 1.49 s: 1/4 at 40 BPM fits */
#define CHO_LEN 2048u
static int16_t dly_buf[DLY_LEN] __attribute__((section(".pool")));
static int16_t cho_buf[CHO_LEN] __attribute__((section(".pool")));
static const uint16_t REV_COMB[4] = {1116, 1188, 1277, 1356};
static const uint16_t REV_AP[2] = {556, 441};
static int16_t rev_comb[1116 + 1188 + 1277 + 1356] __attribute__((section(".pool")));
static union {                          /* ROOM's allpasses; SPRING's allpass chain (int32: no clamps) */
    int16_t ap[556 + 441];
    int32_t sp[(556 + 441) / 2];
} rev_u __attribute__((section(".pool")));
#define rev_ap (rev_u.ap)
#define RPRE_LEN 2048u                   /* (JIANT) the reverb's pre-delay line, at half rate (4 KB): up to 92 ms */
static int16_t rpre_buf[RPRE_LEN] __attribute__((section(".pool")));
static int32_t rpre_out[CTL];                     /* .. the block out of it (the reverb's input) */
static struct {
    uint32_t dly_w, cho_w, cho_ph;
    int32_t dly_lp, dly_hp;              /* the delay's feedback: high cut (COLR), low cut (JIANT: HPF) */
    uint32_t rpre_w, rm_ph;              /* (JIANT) the pre-delay's write count (samples); the reverb modulation's phase */
    int32_t rm_m[4], rf_lp;              /* .. the combs' offsets (Q8 samples); the reverb's input filter (FILT) */
    uint32_t gr_ph, gr_sp[2];            /* .. the GRAIN delay: grain 0's phase (Q16), each grain's spray (samples) */
    int32_t rpre_s;                      /* .. the even sample, averaged with the odd one into the line */
    uint16_t comb_i[4], ap_i[2];
    int32_t comb_lp[4];
    uint8_t rtype;                       /* the reverb model running (G_RTYPE: 0 ROOM, 1 SPRING, 0.5: 2 SHIMMER 3 RESO 4 CLOUD) */
    uint32_t sh_w, sh_ph;                /* (JIANT 0.5) SHIMMER: its line's write count, the grains' phase */
    int32_t rs_lp[4];                    /* .. RESO: the combs' loop low-passes */
    uint32_t rs_per[4];                  /* .. (0.6.5) their periods now (Q8), gliding to the chord's */
    uint8_t rs_root;                     /* .. the note FOLW last heard + 1 (0: none yet) */
    uint16_t rs_w;                       /* .. their write index (each its own line, 1024) */
    uint32_t cl_w, cl_rng;               /* .. CLOUD: its line's write count (half rate), its random numbers */
    int32_t cl_next, cl_lp, cl_half;     /* .. the samples to the next grain, the output's low-pass, the odd sample */
    uint16_t sp_w;                       /* SPRING: the loop's write index (SP_MASK) */
    int32_t sp_lp, sp_hp, sp_he, sp_size;   /* .. its loop low-pass, low cut (and its remainder), the loop
                                             * length (Q8, glides) */
    uint32_t sp_ph;                      /* .. the output tap's wobble */
} fx;

/* SPRING (G_RTYPE 1): one spring of a spring tank, mono like the other buses, in the ROOM's own buffers (no
 * RAM of its own): the input and the loop's return -> a low cut (~110 Hz: a spring carries little bass) ->
 * SP_N stretched first-order allpasses, (a + z^-4) / (1 + a z^-4) (after Valimaki, Parker and Abel: below
 * fs / 8 = 5.5 kHz the group delay rises with frequency, the chirp; each pass round the loop adds more of
 * it: the "boing", the drips) -> the loop's delay line (rev_comb, SP_LEN) -> back through a one-pole
 * low-pass (DAMP) and the decay gain (SIZE). The output: the spring's far end, half way along the loop
 * (the first sound 15 .. 30 ms after the send: the tank's own pre-delay; a slow wobble of a sample or two
 * on it), plus a second, quieter pickup at three quarters (a shorter spring beside it: denser). SIZE sets
 * the loop's length (30 .. 60 ms) and its decay; DAMP the loop's low-pass. The allpasses' states: 4
 * samples each (int32), in rev_ap's memory. Changing the model fades the old one's block out and clears both buffers. */
#define SP_LEN 4096u                     /* the loop's line in rev_comb (4937 samples) */
#define SP_MASK (SP_LEN - 1u)
#define SP_N 10u                         /* allpass stages */
#define SP_A 2867                        /* their coefficient, Q12 (0.7: Q12 keeps (x - o) * a in 32 bits up to
                                          * |x - o| < 749000, far past any peak the chain reaches) */
_Static_assert(sizeof rev_comb / 2u >= SP_LEN && sizeof rev_u.sp / 4u >= 4u * (SP_N + 1u), "SPRING in ROOM's buffers");

/* DIST: low cut -> drive (1x..8x, exponential) -> asymmetric soft clip
 * (a little bias = even harmonics) -> tone low-pass that closes with drive ->
 * make-up gain (straight into tanh over the full band, it would sound like a
 * broken digital fuzz). State per part (track_t dist_*). */
/* (JIANT) DIST TYPE (P_DTYPE): SOFT the biased tanh as before; HARD a hard clip; FOLD a triangle wavefolder (the drive
 * folds the wave back on itself: more partials as it rises); CRUSH fewer bits and a held sample (the drive takes both
 * down); RECT full-wave rectified (an octave up, fuzz), its DC taken out. Then the two-pole low-pass, TONE (P_DTONE)
 * moving it darker / brighter, and the make-up. DRIVE 0: out of the chain */
enum { DT_SOFT, DT_HARD, DT_FOLD, DT_CRUSH, DT_RECT };
static __attribute__((noinline)) int32_t dist_shape(track_t *t, uint32_t type, int32_t x, int32_t g, int32_t d)
{
    int32_t v = ((x >> 2) * g) >> 10, a;
    switch (type) {
    case DT_HARD:
        return clamp(v, -22000, 22000);
    case DT_FOLD:                                        /* folded back at +-T (T 22000), up to 5T: three folds */
        v = clamp(v, -110000, 110000);
        for (a = 0; a < 3; a++)
            v = v > 22000 ? 44000 - v : v < -22000 ? -44000 - v : v;
        return v;
    case DT_CRUSH: {
        uint32_t hold = 1u + (uint32_t)d / 12u, sh = 4u + (uint32_t)d / 12u;   /* (up to 11x held, 4 .. 14 bits off) */
        if (!t->dist_hc) {
            t->dist_hold = (int32_t)((uint32_t)(clamp(x, -131072, 131068) >> sh) << sh);
            t->dist_hc = (uint8_t)hold;
        }
        t->dist_hc--;
        return clamp(((t->dist_hold >> 2) * g) >> 10, -26000, 26000);   /* (the drive's gain as the others: level kept) */
    }
    case DT_RECT:
        a = x < 0 ? -x : x;
        t->dist_dc += (a - t->dist_dc) >> 9;             /* (its mean: the DC the rectifier adds) */
        return softclip((((a - t->dist_dc) >> 2) * g) >> 10);
    default:
        return softclip(v + 2400) - softclip(2400);
    }
}
static void track_dist(track_t *t, int32_t *b, uint32_t n)
{
    int32_t d = t->p[P_DIST], i, g, k, mk, b0 = softclip(2400);
    uint32_t type = (uint32_t)t->p[P_DTYPE];
    if (!d)
        return;                                         /* states kept: switching on does not click */
    g = 4096 + d * d * 2;                                /* Q12: 1x .. ~9x, gentle at first */
    k = clamp(32000 - d * 95 + t->p[P_DTONE] * 300, 2500, 32767);   /* tone: transparent at low drive .. ~3 kHz, Q15 */
    mk = type == DT_HARD || type == DT_FOLD || type == DT_CRUSH ? 30000 - d * 60 : 30000 - d * 120;   /* make-up */
    if (type > DT_RECT)
        type = DT_SOFT;
    for (i = 0; i < (int32_t)n; i++) {
        int32_t x = b[i], y;
        t->dist_hp += (x - t->dist_hp + 64) >> 7;           /* ~55 Hz low cut: keep the bass out of the clipper */
        x = clamp(x - t->dist_hp, -230000, 230000);         /* (x >> 2) * g fits 32 bits; the clip is flat out there */
        y = type == DT_SOFT ? softclip((((x >> 2) * g) >> 10) + 2400) - b0 : dist_shape(t, type, x, g, d);
        t->dist_lp1 += mulq15(y - t->dist_lp1, k);         /* two poles: tames the fizz */
        t->dist_lp2 += mulq15(t->dist_lp1 - t->dist_lp2, k);
        b[i] = mulq15(t->dist_lp2, mk);
    }
}

/* master: peak limiter in front of the soft clipper. Fast attack (~0.1 ms),
 * ~150 ms release, threshold where tanh is still nearly linear, so chords
 * get quieter instead of crushed. */
#define LIM_T 18000
static int32_t lim_env = LIM_T;
/* (JIANT) CLIP (G_CLIP 0..100): the mix driven x1 .. x4 into the soft clip ahead of the limiter, its level kept near
 * -4 dBFS (as DRUM's DRV); 0: out of the chain. Set each block (master_begin) */
static int32_t clip_g, clip_mk;
/* the mix (dry + wet, before the MASTER knob: the same saturation at any volume) through CLIP, in place; wet
 * cleared (it is in the mix now). Its own loop, only while CLIP is on: the ISR's master loop stays as it was */
static int32_t mix_l[CTL], mix_r[CTL], wet[CTL];
static __attribute__((noinline)) void clip_block(uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {                           /* (>> 4: the sums may be far above Q15; << 2 back) */
        mix_l[i] = ((softclip(((mix_l[i] + wet[i]) >> 4) * clip_g >> 10) * clip_mk) >> 15) << 2;
        mix_r[i] = ((softclip(((mix_r[i] + wet[i]) >> 4) * clip_g >> 10) * clip_mk) >> 15) << 2;
        wet[i] = 0;
    }
}
static __attribute__((noinline)) void master_begin(void)
{
    int32_t a = clip_eff;
    clip_g = a > 0 ? 4096 + a * 12288 / 100 : 0;
    if (clip_g)                                         /* (JIANT 0.5) made up at a mix's usual level (-15 dBFS), not near
                                                         * full scale: CLIP adds saturation, not loudness (the leveler) */
        clip_mk = (int32_t)((6000u << 15) / (uint32_t)softclip((6000 * clip_g) >> 12));
}
/* (JIANT 0.5) the LEVELER, always on: the mix (dry + wet, before the MASTER knob) followed block by block (its peak,
 * ~45 ms up, ~1.5 s down: no pumping) and brought toward LEV_T at 2:1 (the gain the square root of LEV_T / level, -9 .. +6 dB),
 * ramped across the block; the gain itself smoothed (~190 ms); below LEV_GATE (-40 dB) it eases back to unity, so a tail or silence is not
 * pumped up. The peak
 * limiter and the soft clip (master_out) after it catch what is left: quiet patches and CLIP's drive sit at one
 * loudness */
#define LEV_T 12000                                     /* (the mix's scale: Q15 at MASTER full) ~ -9 dBFS peaks: the
                                                         * limiter's headroom (0.5.1: was 16000, it limited 95 % of the time) */
#define LEV_GATE 400                                    /* -38 dB */
static int32_t lev_env, lev_g = 32768, lev_cur = 32768, lev_dg;   /* the gain now and its step a sample (master_out) */
static __attribute__((noinline)) void lev_block(uint32_t n)
{
    uint32_t i;
    int32_t pk = 0, g1;
    for (i = 0; i < n; i++) {
        int32_t l = mix_l[i] + wet[i], r = mix_r[i] + wet[i];
        l = l < 0 ? -l : l;
        r = r < 0 ? -r : r;
        if (l > pk) pk = l;
        if (r > pk) pk = r;
    }
    lev_env += pk > lev_env ? (pk - lev_env) >> 6 : -((lev_env - pk) >> 11);   /* (0.5.1: ~45 ms up, ~1.5 s down: it
                                                         * rides the song's loudness, not each hit; was ~1 / ~46 ms) */
    g1 = 32768;                                         /* (below the gate: back to unity, slowly) */
    if (lev_env > LEV_GATE && pk > LEV_GATE) {         /* (0.5.1: the block itself above the gate too: a tail
                                                         * dying under it is not lifted while the slow follower falls) */
        uint32_t q = ((uint32_t)LEV_T << 12) / (uint32_t)lev_env, rt = 0, b = 1u << 30;   /* LEV_T / level, Q12 */
        q <<= 8;                                        /* (Q20: its square root Q10) */
        while (b > q) b >>= 2;
        while (b) {
            if (q >= rt + b) { q -= rt + b; rt = (rt >> 1) + b; }
            else rt >>= 1;
            b >>= 2;
        }
        g1 = clamp((int32_t)rt << 5, 11500, 65536);     /* Q15: x0.35 .. x2 */
    }
    g1 = lev_g + ((g1 - lev_g) >> (lev_env > LEV_GATE && pk > LEV_GATE ? 8 : 5));   /* (smoothed: ~190 ms; gated, under -38 dB: ~25 ms
                                                         * back to unity, no gain left on a tail's offset) */
    lev_cur = lev_g;                                    /* (master_out ramps it across the block, after the DC block) */
    lev_dg = (g1 - lev_g) / (int32_t)n;
    lev_g = g1;
}
/* DUCK (G_DUCK 0..100, G_DREL its release 40 .. 600 ms): a kick (eng_drum.c duck_hit) takes the parts that are not
 * DRUM down by up to 30 dB in ~2 ms, holds them there ~25 ms, and they come back with the release, shaped as a
 * sidechain pump (the gain follows 1 - (1 - env)^2: low for longer, then a quick rise). duck_g0 / duck_g1: the gain
 * (Q15) at the block's start and end, ramped by mix_part */
static int32_t duck_env, duck_g0 = 32767, duck_g1 = 32767;
static uint8_t duck_att, duck_hold;
static __attribute__((noinline)) void duck_block(void)
{
    int32_t dep = song.g[G_DUCK] * 31731 / 100, e;       /* (1 - 0.032: -30 dB at 100) */
    if (duck_hit) {
        duck_hit = 0;
        duck_att = 1;
        duck_hold = (uint8_t)(1103u / CTL);              /* (25 ms in blocks) */
    }
    duck_g0 = duck_g1;
    if (duck_att) {
        duck_env += (32767 - duck_env) >> 1;
        if (duck_env > 31000)
            duck_att = 0;
    } else if (duck_hold) {
        duck_hold--;
    } else if (duck_env) {
        duck_env = (int32_t)(((uint32_t)duck_env * dv_kb(40000u + (uint32_t)song.g[G_DREL] * 5600u)) >> 16);
    }
    e = (duck_env * (65536 - duck_env)) >> 15;           /* 1 - (1 - env)^2 */
    duck_g1 = 32767 - mulq15(dep, e > 32767 ? 32767 : e);
}
/* MENU > USB LEVEL FIXED (for #42: record over USB with the speaker turned down): the mix goes to master_out at the
 * full MASTER level, USB audio takes that (audio.c uac_tap), and only then does MASTER scale what the DAC gets
 * (usb_fixed_dac). MASTER (0, the default): MASTER before master_out, as always (USB follows the knob) */
static volatile uint8_t fx_usb_fixed;
#define MASTER_FULL 4096                /* main.c: the MASTER knob's top, Q12 */
/* the MASTER pot (ADC 0..1023, smoothed in main.c) to its level, square law: 0 .. 4088 (Q12) */
static inline uint32_t master_of_pot(uint32_t k10) { return (k10 * k10) >> 8; }
static __attribute__((noinline)) void usb_fixed_dac(int32_t *out, uint32_t n)   /* audio ISR, after uac_tap */
{
    uint32_t i;
    int32_t m = (int32_t)song.master_q12;
    for (i = 0; i < 2u * n; i++)
        out[i] = (out[i] * m) >> 12;                /* (|out| <= 32767 after the soft clip: fits) */
}
static volatile uint8_t fx_lowcut;     /* settings: 1 LOWCUT 12 dB/oct ~110 Hz, 2 BASS+ (the small speaker):
                                        * 12 dB/oct ~220 Hz plus the harmonics of the bass (spk_bass) */
static int32_t lc_l1, lc_l2, lc_r1, lc_r2, dc_l, dc_r, dce_l, dce_r;

/* DC blocker (~2 Hz), always on: a leaky integrator of the input (Q6 state) subtracted from it.
 * The >> 12 step keeps its remainder (error feedback, 0..4095) and adds it to the next one, so no
 * part of the step is lost: the state follows the input exactly, down to 0 after the sound stops.
 * (A rounded step of (x - dc) / 4096 would stop moving at |x - dc| < 2048 and leave an offset of up
 * to +-31 at the output after silence.) */
static inline int32_t dc_block(int32_t x, int32_t *dc, int32_t *err)
{
    int32_t e = (x << 6) - *dc + *err, d = e >> 12;
    *err = e - (d << 12);
    *dc += d;
    return x - ((*dc + 32) >> 6);
}

static int32_t lce[4];
static inline int32_t lowcut1(int32_t x, int32_t *lc, int32_t *err, uint32_t sh)   /* x minus its one-pole low-pass */
{
    int32_t e = x - *lc + *err, d = e >> sh;
    *err = e - (d << sh);
    *lc += d;
    return x - *lc;
}

/* BASS+: what the speaker cannot play, heard through its harmonics. The bass below ~150 Hz is clipped at its
 * own envelope (a level-following trapezoid: odd harmonics), then band-passed ~220 Hz..1 kHz and added.
 * The low-pass has 4 poles (#42: with 2, the trapezoid rebuilt the 300 .. 600 Hz of the mix itself, late,
 * and cancelled up to 6 dB of it; now under 0.5 dB) */
static int32_t sb_lp1, sb_lp2, sb_lp3, sb_lp4, sb_env, sb_h1, sb_h2, sb_hl;
static inline int32_t spk_bass(int32_t m)
{
    int32_t a, t, u;
    sb_lp1 += ((m - sb_lp1) * 692) >> 15;
    sb_lp2 += ((sb_lp1 - sb_lp2) * 692) >> 15;
    sb_lp3 += ((sb_lp2 - sb_lp3) * 692) >> 15;
    sb_lp4 += ((sb_lp3 - sb_lp4) * 692) >> 15;
    a = sb_lp4 < 0 ? -sb_lp4 : sb_lp4;
    if (a > sb_env)
        sb_env += (a - sb_env) >> 2;
    else if (sb_env > 0)
        sb_env -= (sb_env >> 11) + 1;
    t = clamp(sb_lp4 * 8, -sb_env, sb_env);
    sb_h1 += (t - sb_h1) >> 5;
    u = t - sb_h1;
    sb_h2 += (u - sb_h2) >> 5;
    u -= sb_h2;
    sb_hl += (u - sb_hl) >> 3;
    return sb_hl * 3;
}

static inline void master_out(int32_t *l, int32_t *r)
{
    int32_t al, ar, a;
    *l = dc_block(*l, &dc_l, &dce_l);
    *r = dc_block(*r, &dc_r, &dce_r);
    if (fx_lowcut) {                  /* two one-pole high-passes, error feedback as dc_block (the */
        uint32_t sh = fx_lowcut == 2u ? 5u : 6u;    /* rounded step stopped at |x - lc| < 32: an offset) */
        int32_t b = fx_lowcut == 2u ? spk_bass((*l + *r) >> 1) : 0;
        *l = lowcut1(*l, &lc_l1, &lce[0], sh);
        *l = lowcut1(*l, &lc_l2, &lce[1], sh) + b;
        *r = lowcut1(*r, &lc_r1, &lce[2], sh);
        *r = lowcut1(*r, &lc_r2, &lce[3], sh) + b;
    }
    *l = (clamp(*l, -262143, 262143) * (lev_cur >> 4)) >> 11;   /* (JIANT 0.5) the leveler's gain (lev_block) */
    *r = (clamp(*r, -262143, 262143) * (lev_cur >> 4)) >> 11;
    lev_cur += lev_dg;
    al = *l < 0 ? -*l : *l;
    ar = *r < 0 ? -*r : *r;
    a = al > ar ? al : ar;
    if (a > lim_env)
        lim_env += (a - lim_env) >> 2;
    else if (lim_env > LIM_T)
        lim_env -= ((lim_env - LIM_T) >> 11) + 1;   /* (0.5.1: ~45 ms back, was ~90) */
    if (lim_env > LIM_T) {
        int32_t g = (int32_t)(((uint32_t)LIM_T << 15) / (uint32_t)lim_env);   /* < 32768 */
        *l = ((*l >> 4) * g) >> 11;                      /* >> 4 first: |l| may be far above Q15 */
        *r = ((*r >> 4) * g) >> 11;
    }
    *l = softclip(*l);
    *r = softclip(*r);
}

/* length of one division (N_DIV order) in samples at the song tempo */
static const uint8_t DIV_DEN[6] = {1, 2, 4, 8, 3, 6};    /* original IDs stay fixed; slow rates append */
static uint32_t midi_beat_samples;                    /* zero until an external clock has a measured tempo */
static uint32_t beat_samples(void)
{
    return song.g[G_CLOCK] && midi_beat_samples ? midi_beat_samples : (uint32_t)FS * 60u / (uint32_t)song.g[G_BPM];
}
static uint32_t div_samples(uint32_t div)
{
    uint32_t quarter = beat_samples();
    return div < 6u ? quarter / DIV_DEN[div] : div < 10u ? quarter << (div - 5u) : quarter / DIV_DEN[div % 6u];
}

#include "perform.c"                                 /* the FX hold layer's effects (the master) */
#include "pfx.c"                                     /* .. its punch-in MIDI effects (on the notes) */
#include "click.c"                                   /* the metronome's click (after the master: audio.c) */

/* (JIANT) GRAIN delay (G_DPIT / G_DSPRY not 0): the line is read by two grains of GR_LEN samples, half a grain
 * apart, each faded in and out (triangles: their sum is flat), each sweeping its read point so it plays PITCH
 * semitones up / down (as a tape pitch shifter); a grain starting again jumps back, SPRY further by a random
 * amount (up to ~190 ms). In the feedback: each repeat a further PITCH away (shimmer, falling echoes), scattered.
 * The same line (dly_buf), no RAM of its own */
#define GR_LEN 2048u
static const uint32_t GR_RATIO[25] = {32768, 34716, 36781, 38968, 41285, 43740, 46341, 49097, 52016, 55109, 58386, 61858, 65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715, 131072};   /* 2^(st/12), Q16 */
static __attribute__((noinline)) int32_t dly_grain(uint32_t dl)
{
    int32_t s = (int32_t)GR_RATIO[song.g[G_DPIT] + 12] - 65536, x = 0;
    uint32_t e = (GR_LEN * (uint32_t)(s < 0 ? -s : s)) >> 8, j;   /* the read point's sweep over a grain, Q8 */
    fx.gr_ph = (fx.gr_ph + 65536u / GR_LEN) & 0xFFFFu;
    for (j = 0; j < 2u; j++) {
        uint32_t ph = (fx.gr_ph + j * 32768u) & 0xFFFFu, d, di;
        int32_t a, b, w;
        if (ph < 65536u / GR_LEN)                       /* this grain starts again: its spray */
            fx.gr_sp[j] = song.g[G_DSPRY] ? rng() % ((uint32_t)song.g[G_DSPRY] * 64u + 1u) : 0u;
        if (dl + fx.gr_sp[j] > DLY_LEN - 2u - GR_LEN * 2u)   /* (JIANT 0.5) a long TIME (1/1 and up: the whole line): */
            dl = DLY_LEN - 2u - GR_LEN * 2u - fx.gr_sp[j];  /* room for the sweep, else the read point sat clamped (no pitch) */
        d = ((dl + fx.gr_sp[j]) << 8) + ((e * ((s > 0 ? 65535u - ph : ph) >> 4)) >> 12);   /* Q8: up, the delay
                                                         * shrinks over the grain; down, it grows */
        di = d >> 8;
        if (di > DLY_LEN - 2u)
            di = DLY_LEN - 2u;
        a = dly_buf[(fx.dly_w - di) & (DLY_LEN - 1u)];
        b = dly_buf[(fx.dly_w - di - 1u) & (DLY_LEN - 1u)];
        w = (int32_t)(ph < 32768u ? ph : 65535u - ph);  /* the triangle, 0 .. 32767 */
        x += ((a + (((b - a) * (int32_t)(d & 255u)) >> 8)) * w) >> 15;
    }
    return x;
}

static uint32_t delay_samples(void)
{
    uint32_t g = (uint32_t)song.g[G_DTIME];
    uint32_t s = g < DT_FREE ? div_samples(g) : (uint32_t)DTIME_MS[(g - DT_FREE) % 48u] * 441u / 10u;   /* (JIANT 0.4: free) */
    return s < 16u ? 16u : s >= DLY_LEN ? DLY_LEN - 1u : s;
}

/* (JIANT 0.5) the combs' feedback from SIZE: 0.76 .. 0.99, the top of the knob a long tail (was 0.957 at most) */
static int32_t rev_size(void)
{
    int32_t g = song.g[G_RSIZE];
    return 25000 + g * 40 + g * g / 7;
}
/* ROOM (G_RTYPE 0): 4 damped combs + 2 allpasses (Freeverb-like, mono), added to out (WIDE: rev_room_mod) */
static __attribute__((noinline)) void rev_room(const int32_t *rev_in, int32_t *out, uint32_t n)
{
    uint32_t i, k;
    int32_t size = rev_size(), damp = 32767 - song.g[G_RDAMP] * 200;
    for (i = 0; i < n; i++) {
        int32_t a = 0;
        int16_t *c = rev_comb;
        int32_t in = mulq15(rev_in[i], 2580);           /* 1/8 at -4 dB: level as before the allpass fix */
        for (k = 0; k < 4u; k++) {
            int32_t o = c[fx.comb_i[k]];
            fx.comb_lp[k] = o + mulq15(fx.comb_lp[k] - o, 32767 - damp);
            c[fx.comb_i[k]] = (int16_t)clamp(in + mulq15(fx.comb_lp[k], size), -32768, 32767);
            if (++fx.comb_i[k] >= REV_COMB[k])
                fx.comb_i[k] = 0;
            a += o;
            c += REV_COMB[k];
        }
        c = rev_ap;
        for (k = 0; k < 2u; k++) {
            int32_t o = c[fx.ap_i[k]];
            int32_t v = a + (o >> 1);
            c[fx.ap_i[k]] = (int16_t)clamp(v, -32768, 32767);
            a = o - a;                                  /* Freeverb: out = buf - in (o - v is a notch comb) */
            if (++fx.ap_i[k] >= REV_AP[k])
                fx.ap_i[k] = 0;
            c += REV_AP[k];
        }
        out[i] += a;
    }
}

/* (JIANT) ROOM with its networks modulated (G_RMOD > 0): each comb read up to ~5.7 ms nearer (between samples, the
 * depth square law, the offset ramped sample by sample: no steps), by a slow sine of G_RRATE a quarter apart per
 * comb: the resonances drift and detune, the ringing smears into a chorused, wide tail. WIDE wd (0.3): combs 1, 3
 * against 2, 4 as a side (left +, right -) into mix_l / mix_r. Its own loop: with MOD and WIDE 0 the plain one runs
 * (MOD 0 here: offsets 0, the plain one's sound) */
static __attribute__((noinline)) void rev_room_mod(const int32_t *rev_in, int32_t *out, uint32_t n, int32_t wd)
{
    uint32_t i, k;
    int32_t size = rev_size(), damp = 32767 - song.g[G_RDAMP] * 200, m1[4], dm[4];
    int32_t depth = song.g[G_RMOD] ? 64 + song.g[G_RMOD] * song.g[G_RMOD] * 4 : 0;   /* Q8 samples: 0.25 .. 252 (~5.7 ms) */
    fx.rm_ph += LFO_INC[song.g[G_RRATE] & 127];
    for (k = 0; k < 4u; k++) {
        m1[k] = (int32_t)(((uint32_t)(osc_sine(fx.rm_ph + k * 0x40000000u) + 32768) * (uint32_t)depth) >> 16);
        dm[k] = (m1[k] - fx.rm_m[k]) / (int32_t)CTL;    /* (Q8, per sample) */
    }
    for (i = 0; i < n; i++) {
        int32_t a = 0, sd = 0;
        int16_t *c = rev_comb;
        int32_t in = mulq15(rev_in[i], 2580);
        for (k = 0; k < 4u; k++) {
            int32_t m = fx.rm_m[k] += dm[k];
            uint32_t L = REV_COMB[k], j0 = fx.comb_i[k] + ((uint32_t)m >> 8), j1;
            int32_t o0, o1, o;
            if (j0 >= L)
                j0 -= L;
            j1 = j0 + 1u >= L ? 0u : j0 + 1u;
            o0 = c[j0];
            o1 = c[j1];
            o = o0 + (((o1 - o0) * (m & 255)) >> 8);    /* (a shorter delay: nearer the write) */
            fx.comb_lp[k] = o + mulq15(fx.comb_lp[k] - o, 32767 - damp);
            c[fx.comb_i[k]] = (int16_t)clamp(in + mulq15(fx.comb_lp[k], size), -32768, 32767);
            if (++fx.comb_i[k] >= L)
                fx.comb_i[k] = 0;
            a += o;
            sd += (k & 1u) ? -o : o;
            c += L;
        }
        if (wd) {
            sd = (sd * wd) >> 7;
            mix_l[i] += sd;
            mix_r[i] -= sd;
        }
        c = rev_ap;
        for (k = 0; k < 2u; k++) {
            int32_t o = c[fx.ap_i[k]];
            int32_t v = a + (o >> 1);
            c[fx.ap_i[k]] = (int16_t)clamp(v, -32768, 32767);
            a = o - a;
            if (++fx.ap_i[k] >= REV_AP[k])
                fx.ap_i[k] = 0;
            c += REV_AP[k];
        }
        out[i] += a;
    }
    for (k = 0; k < 4u; k++)
        fx.rm_m[k] = m1[k];                             /* (exact at the block's end) */
}

/* SPRING (see the top), added to out */
static __attribute__((noinline)) void rev_spring(const int32_t *rev_in, int32_t *out, uint32_t n)
{
    uint32_t i, k, s = (uint32_t)song.g[G_RSIZE];
    int32_t g = 19661 + (int32_t)s * 85 + (int32_t)(s * s) / 16;   /* the loop's gain: 0.6 .. 0.96 (0.5: was 0.93) */
    int32_t kl = 26000 - song.g[G_RDAMP] * 160;         /* its low-pass: ~9 kHz .. ~1.3 kHz */
    int32_t len = (int32_t)(1323u + ((s * 1323u) >> 7)) << 8, L, L2, L3, f, w;
    int16_t *ln = rev_comb;
    int32_t *ap = rev_u.sp;
    if (!fx.sp_size)
        fx.sp_size = len;
    fx.sp_size += clamp(len - fx.sp_size, -256, 256);   /* SIZE glides (a sample a block at most) */
    L = fx.sp_size >> 8;
    fx.sp_ph += 2u * LFO_INC[song.g[G_RMOD] ? song.g[G_RRATE] & 127 : 24];   /* the wobble: a slow sine, 1.5 samples */
    w = (fx.sp_size >> 1) + ((osc_sine(fx.sp_ph) * (3 + song.g[G_RMOD] / 2)) >> 8);   /* deep (MOD: up to 66); the far end, Q8 */
    L2 = w >> 8;
    f = w & 255;
    L3 = (L * 3) >> 2;
    for (i = 0; i < n; i++) {
        uint32_t wp = fx.sp_w, j = (wp & 3u) * (SP_N + 1u);
        int32_t x = mulq15(rev_in[i], 2580), r = ln[(wp - (uint32_t)L) & SP_MASK], p, o;
        int32_t t0 = ln[(wp - (uint32_t)L2) & SP_MASK], t1 = ln[(wp - (uint32_t)L2 - 1u) & SP_MASK];
        fx.sp_lp += mulq15(r - fx.sp_lp, kl);
        o = fx.sp_lp * g;
        x += (o + ((o >> 31) & 32767)) >> 15;           /* towards 0: a loop of floors would hold an offset */
        o = x - fx.sp_hp + fx.sp_he;                    /* the low cut, its step's remainder kept (as */
        fx.sp_he = o & 63;                              /* dc_block): no dead band to hold an offset in the loop */
        fx.sp_hp += o >> 6;
        x -= fx.sp_hp;
        p = ap[j];                                      /* the chain: ap[j + k], stage k's output 4 samples ago */
        ap[j] = x;
        for (k = 1; k <= SP_N; k++) {                   /* (lossless: bounded by the loop's input, no clamp) */
            int32_t v = (x - ap[j + k]) * SP_A;         /* towards 0, as the loop's gain: floors would feed */
            o = ap[j + k];                              /* the loop a little offset and noise for ever */
            x = ((v + ((v >> 31) & 4095)) >> 12) + p;
            p = o;
            ap[j + k] = x;
        }
        ln[wp & SP_MASK] = (int16_t)clamp(x, -32768, 32767);
        fx.sp_w = (uint16_t)(wp + 1u);
        out[i] += (t0 + (((t1 - t0) * f) >> 8)) * 4 + ln[(wp - (uint32_t)L3) & SP_MASK] * 2;
    }
}
/* (JIANT 0.3) SPRING's WIDE, after its block: a tap a quarter along the loop against 3/4 as the side (left +,
 * right -; both older than the block, not written in it) */
static __attribute__((noinline)) void rev_spring_side(uint32_t n, int32_t wd)
{
    const int16_t *ln = rev_comb;
    uint32_t i, L = (uint32_t)fx.sp_size >> 8, L3 = (L * 3u) >> 2, wp = (uint32_t)fx.sp_w - n;
    for (i = 0; i < n; i++, wp++) {
        int32_t sd = ((ln[(wp - L3) & SP_MASK] - ln[(wp - (L >> 2)) & SP_MASK]) * 3 * wd) >> 7;
        mix_l[i] += sd;
        mix_r[i] -= sd;
    }
}


/* (JIANT 0.5) three more models on the reverb send, no parameter of their own (the REVERB pages' knobs, read per model):
 *   SHIMMER (2)  ROOM with its output an octave up fed back into it (two crossfaded grains reading its own line twice
 *                as fast: a pitch shifter): each pass round the loop an octave higher, a halo of rising partials.
 *                MOD the shimmer's amount; SIZE DAMP PRE as ROOM
 *   RESO (3)     four combs tuned to the song's scale (ROOT and SCALE: its I III V VII from C3), a sympathetic string
 *                each: whatever is sent rings in the key. SIZE their sustain (to ~0.995), DAMP their brightness, MOD the
 *                voicing open (the V and VII an octave up), PRE as ever. In ROOM's comb memory
 *   CLOUD (4)    a granular cloud: the send recorded (half rate, ~0.74 s) and read by up to six grains, each from a
 *                random point of the last SIZE of it, 70 .. 380 ms long, faded in and out, at the pitch of the note or
 *                (MOD: the more, the oftener) an octave up / down or a fifth up; RATE their density, DAMP the tone,
 *                WIDE them left and right. SIZE full: FREEZE (the line stops being written: the cloud holds) */
static uint32_t scale_mask(const track_t *t);           /* seq.c */
#define SH_LEN 2048u
static int16_t shim_buf[SH_LEN] __attribute__((section(".pool")));
static int32_t shim_fb[CTL];
static __attribute__((noinline)) void rev_shimmer(const int32_t *rev_in, int32_t *out, uint32_t n)
{
    int32_t in[CTL], t[CTL], amt = song.g[G_RMOD] * 190;   /* Q15: up to ~0.74 */
    uint32_t i, j;
    for (i = 0; i < n; i++) {
        in[i] = rev_in[i] + mulq15(shim_fb[i], amt);
        t[i] = 0;
    }
    rev_room(in, t, n);
    for (i = 0; i < n; i++) {
        int32_t x = 0;
        out[i] += t[i];
        shim_buf[fx.sh_w & (SH_LEN - 1u)] = (int16_t)clamp(t[i] >> 1, -32768, 32767);
        for (j = 0; j < 2u; j++) {                      /* the delay falls a sample a sample: read at twice the speed */
            uint32_t ph = (fx.sh_ph + j * (SH_LEN / 2u)) & (SH_LEN - 1u), d = SH_LEN - 1u - ph;
            int32_t w = (int32_t)(ph < SH_LEN / 2u ? ph : SH_LEN - 1u - ph);   /* the triangles: their sum flat */
            x += (shim_buf[(fx.sh_w - d) & (SH_LEN - 1u)] * w) >> 9;
        }
        fx.sh_ph++;
        fx.sh_w++;
        shim_fb[i] = x;                                 /* (into the next block's input) */
    }
}
#define RS_LEN 1024u                                     /* each comb's line, in rev_comb: down to ~43 Hz */
_Static_assert(sizeof rev_comb / 2u >= 4u * RS_LEN, "RESO in ROOM's buffers");
/* (JIANT 0.6.5) RESO's two knobs of its own, in the slots of PRE and DAMP (no pre-delay, a fixed tone on RESO):
 * PITCH (G_RPRE 0..14) the chord that many steps of the scale up; FOLW (G_RDAMP 0..6) OFF / T1..T6: the chord built on
 * the lowest note that track sounds (held after it stops), so it moves with the sequence. The strings glide there */
static uint32_t reso_pitch(void) { return song.g[G_RTYPE] == 3 ? (uint32_t)clamp(song.g[G_RPRE], 0, 14) : 0u; }
static uint32_t reso_folw(void) { return song.g[G_RDAMP] >= 1 && song.g[G_RDAMP] <= 6 ? (uint32_t)song.g[G_RDAMP] : 0u; }
static __attribute__((noinline)) void rev_reso(const int32_t *rev_in, int32_t *out, uint32_t n)
{
    const track_t *t = &trk[0];
    uint32_t i, k, per[4], mask, s, fol = reso_folw(), up = reso_pitch();
    int32_t sz = song.g[G_RSIZE], dk = 32767 - 60 * 200, note[4], deg = 0, nn;
    int32_t fb = sz <= 100 ? 26000 + sz * 52 : clamp(31200 + (sz - 100) * 58, 0, 32740);   /* (0.5.1: the top of SIZE
                                                         * rings on for tens of seconds: 0.9992 at 127, was 0.995) */
    for (k = 0; k < NTRK; k++)                           /* the song's scale: the first melodic track's (they share it) */
        if (ENGINES[eng_idx(trk[k].eng_req)] != &ENG_DRUM) { t = &trk[k]; break; }
    mask = scale_mask(t) & 0xFFFu;
    if (!mask) mask = 0xFFFu;
    nn = 48 + t->p[P_ROOT];
    if (fol) {                                           /* FOLW: the lowest note the track sounds */
        const track_t *ft = &trk[fol - 1u];
        uint32_t lo = 0xFFu;
        for (k = 0; k < NVOICE; k++)
            if (ft->v[k].active && ft->v[k].gate && ft->v[k].note < lo)
                lo = ft->v[k].note;
        if (lo != 0xFFu)
            fx.rs_root = (uint8_t)(lo + 1u);
        if (fx.rs_root) {
            nn = 48 + (int32_t)(fx.rs_root - 1u) % 12;          /* (its pitch class from C3, down onto the scale) */
            for (k = 0; k < 12u && !((mask >> (uint32_t)((nn - t->p[P_ROOT] + 120) % 12)) & 1u); k++)
                nn--;
        }
    }
    for (k = 0; k < up; nn++)                            /* PITCH: that many steps of the scale up */
        if ((mask >> (uint32_t)((nn + 1 - t->p[P_ROOT] + 120) % 12)) & 1u)
            k++;
    for (k = 0, s = 0; k < 4u && s < 48u; s++, nn++)     /* the I, III, V, VII from there up */
        if ((mask >> (uint32_t)((nn - t->p[P_ROOT] + 120) % 12)) & 1u) {
            if (deg % 2 == 0)
                note[k++] = nn;
            deg++;
        }
    for (; k < 4u; k++) note[k] = note[k - 1] + 12;
    if (song.g[G_RMOD] > 64) { note[2] += 12; note[3] += 12; }   /* MOD: the voicing open */
    for (k = 0; k < 4u; k++) {
        uint32_t inc = pitch_inc((uint32_t)clamp(note[k] * 16, 0, 2047));
        per[k] = 0xFFFFFFFFu / ((inc >> 8) | 1u);          /* the period, Q8 */
        if (per[k] > (RS_LEN - 2u) << 8) per[k] = (RS_LEN - 2u) << 8;
        if (!fx.rs_per[k])
            fx.rs_per[k] = per[k];
        fx.rs_per[k] += (uint32_t)(((int32_t)per[k] - (int32_t)fx.rs_per[k]) / 6);   /* (a glide, ~6 blocks) */
        per[k] = fx.rs_per[k];
    }
    for (i = 0; i < n; i++) {
        int32_t in = mulq15(rev_in[i], 3600), a = 0;
        for (k = 0; k < 4u; k++) {
            int16_t *c = rev_comb + k * RS_LEN;
            uint32_t d = per[k] >> 8, f = per[k] & 255u;
            int32_t x0 = c[(fx.rs_w - d) & (RS_LEN - 1u)], x1 = c[(fx.rs_w - d - 1u) & (RS_LEN - 1u)];
            int32_t o = x0 + (((x1 - x0) * (int32_t)f) >> 8);
            fx.rs_lp[k] = o + mulq15(fx.rs_lp[k] - o, 32767 - dk);
            c[fx.rs_w & (RS_LEN - 1u)] = (int16_t)clamp(in + mulq15(fx.rs_lp[k], fb), -32768, 32767);
            a += o;
        }
        fx.rs_w++;
        out[i] += a;
    }
}
#define CL_LEN 16384u                                    /* half rate: ~0.74 s */
#define CL_N 12u
static int16_t cloud_buf[CL_LEN] __attribute__((section(".pool")));
static struct { uint32_t pos, inc; uint16_t len, age; int16_t pl, pr; } cl_g[CL_N];
static uint32_t cl_rand(void) { fx.cl_rng = fx.cl_rng * 1664525u + 1013904223u; return fx.cl_rng >> 8; }
/* CLOUD (G_RTYPE 4, JIANT 0.5, made wet in 0.5.1): a granular wash. The input and the cloud's own tail fed back
 * (SIZE: the tail ~0.09, the grains ~0.43: the wash builds on itself) are written at half rate into a 0.74 s line;
 * up to 12 overlapping
 * grains (RATE: their density; 45 .. 140 ms long, triangle windows) read it from anywhere behind the write, MOD of
 * them an octave up, down or a fifth up; each grain panned at random (WIDE: how far, left and right). The grains go
 * through ROOM's network (SIZE its decay, DAMP its tone; MOD chorusing it, WIDE its combs apart): the grains are not
 * heard dry, they bloom into a tail around the sound. SIZE full freezes the line (the cloud holds) */
static __attribute__((noinline)) void rev_cloud(const int32_t *rev_in, int32_t *out, uint32_t n)
{
    static const uint32_t PITCH[4] = {65536u, 131072u, 32768u, 98184u};   /* x1, an octave up, down, a fifth up (Q16) */
    uint32_t i, k, freeze = song.g[G_RSIZE] >= 127;
    int32_t gap = 1700 - song.g[G_RRATE] * 11, dk = 32767 - song.g[G_RDAMP] * 220, wd = song.g[G_RWIDE];
    int32_t fb = song.g[G_RSIZE] * 24, fg = song.g[G_RSIZE] * 110, g[CTL], t[CTL];   /* (Q15: the tail, ~0.09, and the
                                                         * grains themselves, ~0.66, back into the line) */
    for (i = 0; i < n; i++) {
        int32_t l = 0, r = 0, m;
        if (!freeze) {                                   /* the line: the input and the tail, two samples into one */
            int32_t x = (rev_in[i] >> 1) + mulq15(shim_fb[i], fb) + mulq15(fx.cl_lp, fg);
            if (fx.cl_w & 1u)
                cloud_buf[(fx.cl_w >> 1) & (CL_LEN - 1u)] = (int16_t)clamp((fx.cl_half + x) >> 1, -32768, 32767);
            else
                fx.cl_half = x;
            fx.cl_w++;
        }
        if (--fx.cl_next <= 0) {                         /* a grain is born, in the first one free */
            fx.cl_next = gap / 2 + (int32_t)(cl_rand() % (uint32_t)gap);
            for (k = 0; k < CL_N && cl_g[k].len; k++) {}
            if (k < CL_N) {
                uint32_t pk = cl_rand() % 100u < (uint32_t)song.g[G_RMOD] * 3u / 4u ? 1u + cl_rand() % 3u : 0u;
                int32_t pan = (int32_t)(cl_rand() % 257u) - 128;   /* -128 .. 128, times WIDE */
                pan = pan * wd >> 7;
                cl_g[k].len = (uint16_t)(2000u + cl_rand() % 4200u);
                cl_g[k].pos = ((fx.cl_w >> 1) - 200u - cl_rand() % (CL_LEN - 6000u)) << 16;
                cl_g[k].inc = (PITCH[pk] >> 1) + (cl_rand() % 129u) - 64u;   /* (the line at half rate; each grain
                                                         * a few cents off: a chorused shimmer, not one pitch) */
                cl_g[k].age = 0;
                cl_g[k].pl = (int16_t)(128 - pan);       /* (0 .. 256: 128 the centre) */
                cl_g[k].pr = (int16_t)(128 + pan);
            }
        }
        for (k = 0; k < CL_N; k++) {
            if (!cl_g[k].len) continue;
            {
                uint32_t p = cl_g[k].pos >> 16, f = (cl_g[k].pos >> 8) & 255u, a = cl_g[k].age, L = cl_g[k].len;
                int32_t x0 = cloud_buf[p & (CL_LEN - 1u)], x1 = cloud_buf[(p + 1u) & (CL_LEN - 1u)];
                int32_t x = x0 + (((x1 - x0) * (int32_t)f) >> 8), w = (int32_t)((a < L - a ? a : L - a) * 2u * 256u / L);
                x = (x * w) >> 8;
                l += (x * cl_g[k].pl) >> 7;
                r += (x * cl_g[k].pr) >> 7;
                cl_g[k].pos += cl_g[k].inc;
                if (++cl_g[k].age >= cl_g[k].len) cl_g[k].len = 0;
            }
        }
        m = (l + r) >> 1;
        fx.cl_lp = m + mulq15(fx.cl_lp - m, 32767 - dk);
        g[i] = fx.cl_lp * 2;                             /* (into the network: its input is 1/8 at -4 dB) */
        t[i] = 0;
        if (wd) {                                        /* the grains' own place, left and right (a side) */
            int32_t sd = ((l - r) * wd) >> 9;
            mix_l[i] += sd;
            mix_r[i] -= sd;
        }
    }
    if (song.g[G_RMOD] || wd)
        rev_room_mod(g, t, n, wd);
    else
        rev_room(g, t, n);
    for (i = 0; i < n; i++) {
        out[i] += t[i] + (t[i] >> 1) + (g[i] >> 2);      /* the bloom (+3.5 dB, 0.5.1), the grains themselves under it */
        shim_fb[i] = t[i];                               /* (the tail into the line, next block) */
    }
}
/* the model ty's block into out */
static void rev_run(uint32_t ty, const int32_t *rev_in, int32_t *out, uint32_t n)
{
    switch (ty) {
    case 1: rev_spring(rev_in, out, n); if (song.g[G_RWIDE]) rev_spring_side(n, song.g[G_RWIDE]); break;
    case 2: rev_shimmer(rev_in, out, n); break;
    case 3: rev_reso(rev_in, out, n); break;
    case 4: rev_cloud(rev_in, out, n); break;
    default:
        if (song.g[G_RMOD] || song.g[G_RWIDE]) rev_room_mod(rev_in, out, n, song.g[G_RWIDE]);
        else rev_room(rev_in, out, n);
        break;
    }
}

/* the reverb's buffers and states to silence (the model changed) */
static void rev_clear(void)
{
    uint32_t i;
    for (i = 0; i < sizeof rev_comb / 2u; i++)
        rev_comb[i] = 0;
    for (i = 0; i < sizeof rev_u.ap / 2u; i++)          /* (int16: 997 of them, an odd count the int32 view misses one of) */
        rev_u.ap[i] = 0;
    for (i = 0; i < 4u; i++)
        fx.comb_lp[i] = fx.rs_lp[i] = 0;
    fx.sp_lp = fx.sp_hp = fx.sp_he = 0;
    for (i = 0; i < SH_LEN; i++)                        /* (JIANT 0.5) SHIMMER's line, CLOUD's grains */
        shim_buf[i] = 0;
    for (i = 0; i < CTL; i++)
        shim_fb[i] = 0;
    for (i = 0; i < CL_N; i++)
        cl_g[i].len = 0;
}

static int32_t part_buf[CTL];                            /* a part's block (mix_part); the fade of a model change */

/* (0.5.1) the chorus ensemble: three taps of buf (written at w), the LFO phase ph a third apart each, base delays
 * 300 / 420 / 540 samples plus the sweep (depth Q8); returns the middle (Q15 x2 at unity), *sd the left - right */
static inline int32_t cho_tap(const int16_t *buf, uint32_t w, int32_t r)
{
    uint32_t ri = (uint32_t)r >> 8;
    int32_t f = r & 255, c0 = buf[(w - ri) & (CHO_LEN - 1u)], c1 = buf[(w - ri - 1u) & (CHO_LEN - 1u)];
    return c0 + (((c1 - c0) * f) >> 8);
}
static inline int32_t cho_ens(const int16_t *buf, uint32_t w, uint32_t ph, int32_t depth, int32_t *sd)
{
    int32_t a = cho_tap(buf, w, (300 << 8) + ((osc_sine(ph) + 32768) * depth >> 8));
    int32_t b = cho_tap(buf, w, (420 << 8) + ((osc_sine(ph + 0x55555555u) + 32768) * depth >> 8));
    int32_t c = cho_tap(buf, w, (540 << 8) + ((osc_sine(ph + 0xAAAAAAAAu) + 32768) * depth >> 8));
    *sd = a - b;
    return ((a + b + c) * 21845) >> 15;                 /* (x2 / 3 each: the sum at x2, as the one tap was) */
}

/* process the three buses for one block; sends in, wet stereo-equal out */
static void fx_buses(const int32_t *cho_in, const int32_t *dly_in, const int32_t *rev_in, int32_t *wet,
                     uint32_t n)
{
    uint32_t i, dl = delay_samples();
    int32_t fb = song.g[G_DFDBK] * 230, tn = song.g[G_DCOLOR] - 64;   /* (JIANT 0.4) TONE: one knob, low-pass .. open .. low cut */
    int32_t col = tn < 0 ? 500 + (64 + tn) * (64 + tn) * 8 : 32767;
    int32_t dmix = song.g[G_DMIX] * 258;
    int32_t cdepth = song.g[G_CDEPTH] * 6, rt;
    uint32_t cinc = LFO_INC[song.g[G_CRATE] & 127] / CTL;
    int32_t hk = tn > 0 ? tn * tn * 4 : 0, wd = song.g[G_WIDTH];   /* (the low cut: up to ~3 kHz); WIDTH */
    int32_t gr = song.g[G_DPIT] || song.g[G_DSPRY];     /* (JIANT) the GRAIN delay */
    uint32_t dr = dl + (wd ? div_samples(3) * (uint32_t)wd / 127u : 0u);   /* the right echo: up to 1/32 later */
    if (dr >= DLY_LEN)
        dr = DLY_LEN - 1u;
    if (!hk)
        fx.dly_hp = 0;                                  /* (no low cut: none held over from one) */
    if (song.g[G_RFILT]) {                              /* (JIANT) the reverb's tone: FILT < 0 a low-pass (~12 kHz ..
                                                         * ~350 Hz), > 0 a low cut (~60 Hz .. ~1.5 kHz) */
        int32_t rf = song.g[G_RFILT], k = rf < 0 ? 32767 - (-rf) * 495 : 300 + rf * 110;
        for (i = 0; i < n; i++) {
            fx.rf_lp += mulq15(rev_in[i] - fx.rf_lp, k);
            rpre_out[i] = rf < 0 ? fx.rf_lp : rev_in[i] - fx.rf_lp;
        }
        rev_in = rpre_out;
    }
    if (song.g[G_RPRE] && song.g[G_RTYPE] != 3) {       /* (JIANT) the reverb's pre-delay (in place; RESO: PITCH) */
        uint32_t pd = (uint32_t)song.g[G_RPRE] * 20u;   /* (ms -> half-rate samples: 2000 at 100) */
        for (i = 0; i < n; i++) {                       /* half rate: two samples averaged in, read back between */
            uint32_t h = fx.rpre_w >> 1;
            int32_t a = rpre_buf[(h - pd) & (RPRE_LEN - 1u)], b = rpre_buf[(h - pd + 1u) & (RPRE_LEN - 1u)];
            if (fx.rpre_w & 1u)
                rpre_buf[h & (RPRE_LEN - 1u)] = (int16_t)clamp((fx.rpre_s + (rev_in[i] >> 3)) >> 1, -32768, 32767);
            else
                fx.rpre_s = rev_in[i] >> 3;
            rpre_out[i] = ((fx.rpre_w & 1u) ? (a + b) >> 1 : a) * 8;
            fx.rpre_w++;
        }
        rev_in = rpre_out;
    }
    for (i = 0; i < n; i++) {
        int32_t y = 0, x, side = 0;
        /* chorus (0.5.1: an ensemble): three taps of the line, ~7, 9.5 and 12 ms, each swept by the LFO a third of a
         * turn apart; the first left, the second right, the third in the middle: always stereo (WIDTH wider) */
        cho_buf[fx.cho_w & (CHO_LEN - 1u)] = (int16_t)clamp(cho_in[i] >> 1, -32768, 32767);
        fx.cho_ph += cinc;
        {
            int32_t sd;
            y += cho_ens(cho_buf, fx.cho_w, fx.cho_ph, cdepth, &sd);
            sd = (sd * (80 + wd / 2)) >> 7;
            side += sd;
        }
        fx.cho_w++;
        /* delay with a low-passed (COLR) and low-cut (HPF) feedback */
        x = gr ? dly_grain(dl) : dly_buf[(fx.dly_w - dl) & (DLY_LEN - 1u)];
        fx.dly_lp += mulq15(x - fx.dly_lp, col);
        fx.dly_hp += mulq15(fx.dly_lp - fx.dly_hp, hk);
        dly_buf[fx.dly_w & (DLY_LEN - 1u)] =
            (int16_t)clamp((dly_in[i] >> 1) + mulq15(fx.dly_lp - fx.dly_hp, fb), -32768, 32767);
        if (wd) {                                       /* (JIANT) the right echo later: mid and side */
            int32_t xr = dly_buf[(fx.dly_w - dr) & (DLY_LEN - 1u)];
            side += mulq15(x - xr, dmix);
            x = (x + xr) >> 1;
        }
        fx.dly_w++;
        y += mulq15(x << 1, dmix);
        wet[i] = y;
        if (side) {                                     /* (the side: left +, right -) */
            mix_l[i] += side;
            mix_r[i] -= side;
        }
    }
    rt = (uint32_t)song.g[G_RTYPE] <= 4u ? song.g[G_RTYPE] : 0;
    if (rt != fx.rtype) {                               /* the model changed: the old one's block fades out, */
        int32_t *t = part_buf, g = 65536, d = 65536 / (int32_t)n;   /* its buffers are cleared, the new */
        for (i = 0; i < n; i++)                                     /* one starts from silence */
            t[i] = 0;
        rev_run(fx.rtype, rev_in, t, n);
        for (i = 0; i < n; i++, g -= d)
            wet[i] += mulq16(t[i], (uint32_t)g);
        rev_clear();
        fx.rtype = (uint8_t)rt;
        return;
    }
    rev_run(rt, rev_in, wet, n);
}

/* one block of the whole mix (shared with hostsim.c): events -> each part (with its modulation matrix)
 * -> dist -> level / pan / sends -> buses -> (the SLICER's bus) -> master; out: stereo Q15 */
static void events_block(uint32_t n);                    /* seq.c */
static int32_t send_c[CTL], send_d[CTL], send_r[CTL];   /* (mix_l mix_r wet: above, clip_block) */
/* a part's level (Q12) over the block, ducked: its start << 8, its step a sample in *dl (a block is CTL: no divide) */
static __attribute__((noinline)) int32_t duck_ramp(int32_t lvl, int32_t *dl)
{
    int32_t l0 = ((lvl * (duck_g0 >> 3)) >> 12) << 8;
    *dl = ((((lvl * (duck_g1 >> 3)) >> 12) << 8) - l0) >> CTL_LOG2;
    return l0;
}

/* one synth part into the dry mix and the sends; a part with no voice sounding costs
 * the LFO tick and a cleared buffer only (after the DIST tail has run out) */
static void mix_part(track_t *t, uint32_t n)
{
    int32_t *b = part_buf;
    uint32_t i;
    mod_begin(t);                                       /* the matrix's per-block values into t->p (mod.c) */
    if (track_render(t, b, n))
        t->tail = 16;                                   /* blocks of DIST state to run out after the last voice */
    else if (!t->tail || !t->p[P_DIST] || !--t->tail) {
        if (mod.on)
            mod_end(t);
        return;
    }
    {
        int32_t lvl = LEVEL_Q12[t->p[P_LEVEL] & 127], pan = t->p[P_PAN], lq = lvl << 8, dl = 0;
        int32_t gl = 4096 - (pan > 0 ? pan * 64 : 0), gr = 4096 + (pan < 0 ? pan * 64 : 0);
        int32_t c = t->p[P_CHOR] * 258, d = t->p[P_DLY] * 258, r = t->p[P_REV] * 258, pk = t->peak;
        int32_t xmax = c > d ? c : d, *ol = mix_l, *or_ = mix_r;
        if (slicer_routes((uint32_t)(t - trk))) {       /* (JIANT 0.6.3) into the SLICER's bus, not the dry mix */
            ol = sl_l;
            or_ = sl_r;
        }
        xmax = 0x7FFFFFFF / ((xmax > r ? xmax : r) | 1);   /* sends: loud chords at a high LEVEL */
        if ((duck_g0 < 32767 || duck_g1 < 32767) && ENGINES[t->engine] != &ENG_DRUM)   /* DUCK: the kick's */
            lq = duck_ramp(lvl, &dl);
        track_dist(t, b, n);
        if ((pf.mute >> (t - trk)) & 1u)
            perf_mute((uint32_t)(t - trk), b, n);       /* perform.c: a black key in the FX layer */
        for (i = 0; i < n; i++, lq += dl) {
            int32_t x = ((b[i] >> 2) * (lq >> 8)) >> 10, a = x < 0 ? -x : x;   /* pre-shift: 8 loud voices */
            int32_t xs = clamp(x, -xmax, xmax);         /* sends: mulq15 would overflow */
            if (a > pk)
                pk = a;
            if (c)
                send_c[i] += mulq15(xs, c);
            if (d)
                send_d[i] += mulq15(xs, d);
            if (r)
                send_r[i] += mulq15(xs, r);
            ol[i] += (x * gl) >> 12;
            or_[i] += (x * gr) >> 12;
        }
        t->peak = pk;
    }
    if (mod.on)
        mod_end(t);                                     /* the stored values back */
}

/* (JIANT 0.5.1) DRUM-X X-MOD's LFO, a block: its cycle RATE's bars or notes of the tempo, on the transport's grid while
 * it plays (ms_clock: in time with the bar), free while stopped; dx_mot_v (eng_drum.c) the MORPH offset, +-DPTH/2 */
static uint32_t dx_mot_free, dx_mot_cyc;
static int32_t dx_mot_sh;
static __attribute__((noinline)) void dx_mot_block(uint32_t n)
{
    static const uint8_t Q8[9] = {0, 64, 32, 16, 8, 4, 2, 1, 1};   /* the cycle in 1/16 notes x 4 .. (1/32: half) */
    uint32_t r = dx_mot[XM_RATE], len, pos, ph, cyc;
    int32_t w;
    dx_mot_free += n;
    if (!r || r > 8u || !dx_mot[XM_DPTH]) {
        dx_mot_v = 0;
        return;
    }
    len = beat_samples() * Q8[r] / 4u;                  /* (a quarter = 4 sixteenths) */
    if (r == 8u)
        len = beat_samples() / 8u;
    if (len < 64u)
        len = 64u;
    pos = song.playing ? ms_clock : dx_mot_free;
    cyc = pos / len;
    ph = (uint32_t)(((uint64_t)(pos % len) << 32) / len);
    switch (dx_mot[XM_SHPE]) {
    case 1: w = ph < 0x80000000u ? (int32_t)(ph >> 15) - 32768 : 98303 - (int32_t)(ph >> 15); break;   /* TRI */
    case 2: w = (int32_t)(ph >> 16) - 32768; break;                                                   /* SAW */
    case 3: w = 32767 - (int32_t)(ph >> 16); break;                                                   /* RAMP */
    case 4:                                                                                             /* S&H */
        if (cyc != dx_mot_cyc) {
            dx_mot_rng = dx_mot_rng * 1664525u + 1013904223u;
            dx_mot_sh = (int32_t)(dx_mot_rng >> 16) - 32768;
        }
        w = dx_mot_sh;
        break;
    default: w = osc_sine(ph); break;                                                                  /* SINE */
    }
    dx_mot_cyc = cyc;
    dx_mot_v = (w * dx_mot[XM_DPTH]) >> 16;             /* (+-DPTH / 2) */
}

/* the master with the FX layer's effects between its level and master_out (perform.c) */
static __attribute__((noinline)) void perf_master(int32_t *out, uint32_t n)
{
    uint32_t i;
    int32_t mg = fx_usb_fixed ? MASTER_FULL : (int32_t)song.master_q12;   /* (USB LEVEL FIXED: MASTER after) */
    for (i = 0; i < n; i++) {
        mix_l[i] = (((mix_l[i] + wet[i]) >> 2) * mg) >> 10;
        mix_r[i] = (((mix_r[i] + wet[i]) >> 2) * mg) >> 10;
    }
    perf_block(mix_l, mix_r, n);
    for (i = 0; i < n; i++) {
        int32_t l = mix_l[i], r = mix_r[i];
        master_out(&l, &r);
        out[2u * i] = l;
        out[2u * i + 1u] = r;
    }
}

static void mix_block(int32_t *out, uint32_t n)
{
    uint32_t i;
    int perf;
    int32_t mg = fx_usb_fixed ? MASTER_FULL : (int32_t)song.master_q12;   /* (USB LEVEL FIXED: MASTER after) */
    for (i = 0; i < n; i++)
        send_c[i] = send_d[i] = send_r[i] = mix_l[i] = mix_r[i] = sl_l[i] = sl_r[i] = 0;
    pfx_block(n, (perf_kill ? 0u : (perf_held | perf_latched) & PF_MIDI) | scene_pfx);
    events_block(n);
    if (song.playing)                                   /* (JIANT) the modulation sequences' clock (mod.c): the block's end on
                                                         * the transport's grid (through tempo changes, an external clock) */
        ms_clock = clk_pos == CLK_START ? ms_clock + n : clk_n * div_samples(2) + clk_pos + n;
    macro_master();                                     /* CLIP / PNCH with the matrix (mod.c) */
    dx_mot_block(n);                                    /* (0.5.1) DRUM-X X-MOD's LFO */
    master_begin();
    duck_block();
    perf = perf_begin(n);                               /* the FX hold layer at work (perform.c) */
    for (i = 0; i < NPART; i++)
        mix_part(&trk[i], n);
    if (pfx_any)
        pfx_end();                                      /* (DEC- / DEC+ back) */
    if (perf)
        perf_pre(mix_l, mix_r, sl_l, sl_r, send_d, send_r, n);   /* (THROW: the SLICER's bus too) */
    fx_buses(send_c, send_d, send_r, wet, n);
    if (slicer_fx())                                    /* (JIANT 0.6.3) FX ON: the returns through the SLICER */
        for (i = 0; i < n; i++) {
            sl_l[i] += wet[i];
            sl_r[i] += wet[i];
            wet[i] = 0;
        }
    slicer_bus(sl_l, sl_r, n);                          /* (its clock always; the bus when on) */
    if (slicer_on())
        for (i = 0; i < n; i++) {
            mix_l[i] += sl_l[i];
            mix_r[i] += sl_r[i];
        }
    if (clip_g)
        clip_block(n);
    lev_block(n);                                       /* (JIANT 0.5) the leveler: the loudness held */
    if (perf) {
        perf_master(out, n);
        return;
    }
    for (i = 0; i < n; i++) {
        int32_t l = (((mix_l[i] + wet[i]) >> 2) * mg) >> 10;
        int32_t r = (((mix_r[i] + wet[i]) >> 2) * mg) >> 10;
        master_out(&l, &r);
        out[2u * i] = l;
        out[2u * i + 1u] = r;
    }
}
