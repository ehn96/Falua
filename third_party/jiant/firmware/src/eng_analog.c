/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* ANALOG: two band-limited oscillators (saw / square / tri / sine / PWM),
 * noise, drive and a trapezoidal low-pass. (JIANT 0.4, TRIO folded in: TRIO's sounds play here, project.c) three more
 * WAVEs in a loop of their own (analog_ext): SYNC (osc 2's saw restarted by osc 1, DTN its ratio 1 .. 8.9: the sync
 * lead's sweep), RING (osc 1's saw times osc 2's sine at that ratio: bells, metal), SAW3 (three saws, osc 2 DTN cents up,
 * osc 3 down: TRIO's fat stack). MIX: osc 1 against the other(s) as before. INT (0.4, once KTR): osc 2 INT semitones
 * from osc 1, as a classic two-oscillator synth (fifths, octaves; SAW3: its osc 2). */
static const char *const N_ANALOG_WAVE[] = {"SAW", "SQR", "TRI", "SIN", "PWM", "SYNC", "RING", "SAW3"};
enum { AW_SYNC = 5, AW_RING, AW_SAW3 };
/* (JIANT 0.5) FILTER TYPE (P_FTYPE): LP as before (analog_render's loop); HP BP from the same state-variable filter
 * (analog_ext's loop, every WAVE); COMB the filter open, the part's sum through a feedback comb tuned to the newest
 * note and CUT (+-32 semitones about it), RES its feedback (voice.c track_comb) */
enum { FT_LP, FT_HP, FT_BP, FT_COMB };
static inline int32_t svf_mode(const tsvf_t *c, int32_t in, int32_t *ic1, int32_t *ic2, uint32_t ft, int32_t k)
{
    int32_t v3 = in - *ic2;
    int32_t v1 = (c->a1 * *ic1 + c->a2 * v3) >> 13;
    int32_t v2 = *ic2 + ((c->a2 * *ic1 + c->a3 * v3) >> 13);
    *ic1 = clamp(2 * v1 - *ic1, -150000, 150000);
    *ic2 = clamp(2 * v2 - *ic2, -150000, 150000);
    return ft == FT_HP ? in - v2 - ((k * v1) >> 12) : ft == FT_BP ? (k * v1) >> 12 : ft == FT_COMB ? in : v2;
}

static void analog_note_on(track_t *t, voice_t *v)
{
    (void)t;
    v->ph[1] = v->ph[0] + 0x40000000u;
    v->s[0] = v->s[1] = 0;                            /* filter */
    if (!v->s[2])
        v->s[2] = 0x1234567 + (int32_t)v->age;        /* noise state */
}

/* SYNC RING SAW3 (see the top): a render of their own (the other WAVEs' loop untouched), the same noise, drive and
 * filter */
static __attribute__((noinline)) void analog_ext(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t wave = (uint32_t)p[P_E0], i, inc1 = m->inc, ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2], r, inc2, inc3;
    uint32_t ft = (uint32_t)p[P_FTYPE] & 3u, pw = 0x80000000u + (uint32_t)((m->shape - (64 << 8)) << 15);
    int32_t det = p[P_E1], m2 = p[P_E2] * 258, m1 = 32767 - m2, nz = p[P_E3] * 200, drv = p[P_E6], kd = 8192 - p[P_E5] * 7600 / 127;
    int32_t drive = 32768 + drv * 512, cut = (p[P_E4] << 8) + m->cutoff + ((v->pitch16 - 60 * 16) << 2);
    int32_t ic1 = v->s[0], ic2 = v->s[1], nst = v->s[2];
    tsvf_t flt;
    tsvf_coef(&flt, cut, p[P_E5]);
    r = 16u + (uint32_t)det;                            /* SYNC / RING: osc 2 at r / 16 of osc 1 (1 .. 8.9) */
    inc2 = wave == AW_SAW3 ? cents_inc(m->pitch16 + p[P_E7] * 16, det, 0) : (inc1 >> 4) * r;   /* (SAW3: osc 2 at INT) */
    if (wave != AW_SAW3 && p[P_E7]) {                   /* (JIANT 0.5) SYNC / RING: INT moves osc 2 too, semitones */
        inc2 = (cents_inc(m->pitch16 + p[P_E7] * 16, 0, 0) >> 4) * r;
        r = inc1 >> 8 ? (inc2 / (inc1 >> 8)) >> 4 : r; /* (its ratio to osc 1, x16: the restart below) */
        if (r < 1u) r = 1u;
    }
    inc3 = wave == AW_SAW3 ? cents_inc(m->pitch16, -det, 0) : 0u;
    if (wave < AW_SYNC)                                 /* (JIANT 0.5: HP BP COMB) the plain WAVEs: osc 2 as analog_render */
        inc2 = det || p[P_E7] ? cents_inc(m->pitch16 + p[P_E7] * 16, det, 0) : inc1;
    for (i = 0; i < n; i++) {
        int32_t a = osc_saw(ph0, inc1), b, s;
        if (wave < AW_SYNC) {
            switch (wave) {
            case 1: a = osc_pulse(ph0, inc1, 0x80000000u); b = osc_pulse(ph1, inc2, 0x80000000u); break;
            case 2: a = osc_tri(ph0); b = osc_tri(ph1); break;
            case 3: a = osc_sine(ph0); b = osc_sine(ph1); break;
            case 4: a = osc_pulse(ph0, inc1, pw); b = osc_pulse(ph1, inc2, pw); break;
            default: b = osc_saw(ph1, inc2); break;
            }
        } else if (wave == AW_SAW3) {
            b = (osc_saw(ph1, inc2) + osc_saw(ph2, inc3)) >> 1;
            ph2 += inc3;
        } else if (wave == AW_SYNC) {
            b = osc_saw(ph1, inc2);
        } else {
            b = ((a >> 1) * (osc_sine(ph1) >> 1)) >> 13;   /* RING (32 bits) */
        }
        ph1 += inc2;
        if (wave == AW_SYNC && ph0 + inc1 < ph0)        /* osc 1 starts a cycle: osc 2 with it */
            ph1 = ((ph0 + inc1) >> 4) * r;              /* (the new cycle's fraction, at osc 2's rate) */
        ph0 += inc1;
        s = mulq15(a, m1) + mulq15(b, m2);
        if (nz)
            s += mulq15((int32_t)(noise32(&nst) >> 17) - 16384, nz);
        if (drv)
            s = softclip(((s >> 2) * (drive >> 2)) >> 11);
        out[i] += voice_amp(soft_knee(svf_mode(&flt, s >> 1, &ic1, &ic2, ft, kd), 16000) << 1, m, i) << 1;
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = ph2;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = nst;
}

static __attribute__((noinline)) void analog_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t wave = (uint32_t)p[P_E0], i;
    int32_t det = p[P_E1], mix = p[P_E2], noise = p[P_E3];
    int32_t cut = (p[P_E4] << 8) + m->cutoff + ((v->pitch16 - 60 * 16) << 2);
    tsvf_t flt;
    int32_t drive = 32768 + p[P_E6] * 512;                       /* 1x .. 3x */
    uint32_t inc1 = m->inc;
    uint32_t inc2 = det || p[P_E7] ? cents_inc(m->pitch16 + p[P_E7] * 16, det, 0) : inc1;   /* (JIANT 0.4) osc 2: INT
                                                         * semitones away, DTN cents */
    uint32_t pw = 0x80000000u + (uint32_t)((m->shape - (64 << 8)) << 15);
    int32_t m2 = mix * 258, m1 = 32767 - m2;                    /* osc mix Q15 */
    int32_t nz = noise * 200, drv = p[P_E6];
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1];                  /* state in locals: out[] may alias v->s[] */
    int32_t ic1 = v->s[0], ic2 = v->s[1], nst = v->s[2];
    tsvf_coef(&flt, cut, p[P_E5]);
    for (i = 0; i < n; i++) {
        int32_t a, b, s;
        switch (wave) {
        case 1:
            a = osc_pulse(ph0, inc1, 0x80000000u);
            b = osc_pulse(ph1, inc2, 0x80000000u);
            break;
        case 2:
            a = osc_tri(ph0);
            b = osc_tri(ph1);
            break;
        case 3:
            a = osc_sine(ph0);
            b = osc_sine(ph1);
            break;
        case 4:
            a = osc_pulse(ph0, inc1, pw);
            b = osc_pulse(ph1, inc2, pw);
            break;
        default:
            a = osc_saw(ph0, inc1);
            b = osc_saw(ph1, inc2);
            break;
        }
        ph0 += inc1;
        ph1 += inc2;
        s = mulq15(a, m1) + mulq15(b, m2);
        if (nz)
            s += mulq15((int32_t)(noise32(&nst) >> 17) - 16384, nz);
        if (drv)
            s = softclip(((s >> 2) * (drive >> 2)) >> 11);   /* pre-shifts: drive is up to 3x, no overflow */
        {   /* filter, linear up to half scale, then a soft knee (only resonance peaks saturate); soft_knee()
             * written out: the host compiler makes this loop 1 % slower with the call */
            int32_t y = tsvf_lp(&flt, s >> 1, &ic1, &ic2), a = y < 0 ? -y : y;
            if (a > 16000) {
                a = 16000 + (softclip((a - 16000) * 2) >> 1);
                y = y < 0 ? -a : a;
            }
            s = y << 1;
        }
        out[i] += voice_amp(s, m, i) << 1;
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = nst;
}

/* the engine's render: SYNC RING SAW3 (0.4) and the FILTER TYPEs HP BP COMB (0.5) in analog_ext, the rest (LP) here
 * (a dispatch of its own: analog_render's loop as before) */
static void analog_dispatch(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    if ((uint32_t)t->p[P_E0] >= AW_SYNC || t->p[P_FTYPE])
        analog_ext(t, v, out, n, m);
    else
        analog_render(t, v, out, n, m);
}

static const preset_t ANALOG_PRESETS[] = {
    {"SAW LEAD", {0, 12, 64, 0, 90, 30, 10, 12}, {4, 70, 100, 50}, 20, 1, FX(0, 10, 45, 30), PAT(4)},
    {"SOFT PAD", {0, 20, 64, 4, 60, 10, 0, 0}, {80, 90, 110, 95}, 10, 0, FX(0, 60, 20, 70), PAT(5)},
    {"SQR BASS", {1, 0, 0, 0, 50, 70, 40, 0}, {0, 60, 40, 30}, 30, 1, FX(5, 0, 10, 10), PAT(2)},
    {"PWM STR", {4, 8, 40, 0, 75, 20, 0, 0}, {60, 80, 110, 85}, 8, 0, FX(0, 50, 20, 60), PAT(5)},
    {"ACID", {0, 0, 0, 0, 50, 100, 25, 0}, {0, 55, 20, 30}, 48, 1, FX(20, 0, 45, 15), PAT(1)},
    {"SINE KEY", {3, 6, 50, 0, 127, 0, 0, 0}, {2, 80, 30, 70}, 0, 0, FX(0, 30, 25, 40), PAT(6)},
    {"RAVE", {4, 30, 64, 0, 85, 20, 30, 7}, {20, 80, 110, 60}, 10, 1, FX(30, 40, 30, 30), PAT(13)},
    {"SUB BASS", {3, 0, 0, 0, 40, 0, 20, 0}, {0, 60, 100, 20}, 0, 1, FX(0, 0, 0, 10), PAT(8)},
    {"PLUCK", {0, 8, 50, 0, 30, 40, 0, 0}, {0, 88, 0, 60}, 55, 0, FX(0, 20, 50, 30), PAT(3)},
    {"BRASS", {0, 10, 64, 0, 45, 20, 10, 0}, {35, 70, 90, 45}, 40, 0, FX(0, 20, 20, 40), PAT(6)},
    {"WIND", {0, 0, 0, 90, 30, 90, 0, 0}, {60, 90, 60, 80}, 50, 0, FX(0, 30, 30, 70), PAT(5)},
    {"STRINGS", {0, 25, 64, 0, 70, 10, 0, 0}, {70, 90, 115, 90}, 5, 0, FX(0, 60, 20, 70), PAT(5)},
    /* (JIANT 0.4: TRIO's, folded in) SYNC (ratio 1 + 11/16), RING (1 + 18/16), SAW3 (9 ct a side) */
    {"SYNC LEAD", {5, 11, 90, 0, 82, 30, 10, 0}, {2, 70, 100, 40}, 18, 1, FX(0, 10, 40, 25), PAT(4)},
    {"RING BELL", {6, 18, 100, 0, 110, 20, 0, 0}, {0, 92, 0, 80}, 0, 0, FX(0, 20, 35, 55), PAT(7)},
    {"FAT BASS", {7, 9, 64, 0, 62, 45, 20, -12}, {0, 62, 60, 25}, 40, 1, FX(10, 0, 10, 10), PAT(2)},
};

static const engine_t ENG_ANALOG = {
    .name = "ANALOG",
    .page_title = {"OSC", "FLT"},
    .edit = {
        {"WAVE", F_ENUM, 0, 7, 0, N_ANALOG_WAVE, 0},
        {"DTN", F_INT, 0, 127, 10, 0, "ct"},
        {"MIX", F_PCT, 0, 127, 64, 0, 0},
        {"NOIS", F_PCT, 0, 127, 0, 0, 0},
        {"CUT", F_CUTOFF, 0, 127, 90, 0, 0},
        {"RES", F_PCT, 0, 127, 30, 0, 0},
        {"DRV", F_PCT, 0, 127, 0, 0, 0},
        {"INT", F_SEMI, -24, 24, 0, 0, 0},             /* (JIANT 0.4: osc 2's interval, was KTR: the filter's key
                                                         * tracking, fixed at its old default, half) */
    },
    .presets = ANALOG_PRESETS,
    .npresets = NELEM(ANALOG_PRESETS),
    .note_on = analog_note_on,
    .render = analog_dispatch,
    .knob = {P_E4, P_E5, P_ATK, P_REL},
    .keep = 0x03,                /* the filter */
};
