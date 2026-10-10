/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * JIANT 0.5: the beings, in lines. A sound page without a graph of its own (the engine's EDIT pages) shows a creature
 * as a thermal camera's isotherms: four nested outlines of one body, each as hot as the being is there (heat_col:
 * the outer membrane cold, the core hot), the nuclei hottest. Colour is intensity: the output's level warms every
 * line (and swells the body); in silence it cools and rests. Antialiased lines only (ui_organic.c og_line), ~400 a
 * frame. Each knob of the page shapes it by what it is (its label), else by its place:
 *   CUT        the spikes: a round cell closed, opening into spikes as the filter lets the harmonics through
 *   RES        the membrane rings: a fine ripple round the outline
 *   DTN SPRD   two nuclei, apart (the beating of two oscillators)
 *   NOIS RAND BRTH CRSH   the outline frays
 *   WAVE       the body's lobes
 *   (else)     KNOB 1 more spikes, 2 longer, 3 lobes wobbling, 4 more nuclei
 * DRUM-X (graph_colony): the kit as a colony, a small being per sound, each swelling and warming as it hits (its
 * voice's level now), its body by the sound (the kick round, the snare spiky, the hats a fringe ..), a muted one a
 * dim outline. */
typedef struct {
    uint8_t k0, kk;                                     /* spikes: k0 + KNOB 1 x kk */
    uint8_t ss;                                         /* their length at KNOB 2 full (16 = the radius x 1.6) */
    uint8_t lob;                                        /* lobes of the membrane */
    uint8_t ax;                                         /* stretched: x * ax / 16 */
    uint8_t tw;                                         /* the spikes twist */
    uint8_t sh;                                         /* their sharpness (a power of the cosine) */
} genome_t;
static const genome_t GENOME[8] = {
    {6, 18, 16, 3, 22, 0, 6},                           /* a radiolarian: a sun of thin rays */
    {3, 5, 14, 2, 26, 3, 2},                            /* an amoeba: its arms swirl */
    {4, 6, 10, 4, 18, 0, 1},                            /* a diatom: blunt */
    {10, 30, 8, 5, 20, 1, 10},                          /* a ciliate: a fringe of hairs */
    {5, 9, 12, 3, 19, 2, 4},                            /* a spore */
    {2, 4, 16, 2, 30, 1, 3},                            /* a worm: long, two horns */
    {8, 12, 12, 6, 17, 0, 8},                           /* a pollen grain */
    {3, 7, 15, 3, 21, 4, 5},                            /* a hydra: curled tentacles */
};
/* (JIANT 0.5) a species per engine: ANALOG the radiolarian, PHASE the swirling amoeba, LOFI the blunt diatom, VOICE the
 * hydra (a mouth of tentacles), WHEEL the ciliate (a fringe of pipes), NOISE the spore, FM6 the pollen grain (its
 * operators' geometry), the retired slots the worm */
static const uint8_t EGEN[NENGINES] = {0, 5, 1, 2, 5, 7, 5, 3, 5, 5, 0, 4, 6, 5};
static const genome_t LANE_G_HOME = {12, 0, 10, 3, 18, 0, 6};   /* HOME: a DRUM-X track's being (spiky, a beat) */
typedef struct {
    int32_t spikes, len, wob, lob, nuc, sep, fray, ripple;   /* len wob sep fray ripple 0..256 */
} bfeat_t;
/* a line's colour: heat v; GREY and MONO its grey (their lines blend grey on grey: og_plot) */
static uint16_t bg_col(int32_t v)
{
    uint16_t c = heat_col(v);
    return settings.palette == UI_GREY_INDEX || settings.palette == UI_BW_INDEX ? ux_gray(ux_luma(c) >> 3) : c;
}
static int32_t bg_sin(uint32_t ang) { return sine_i(ang << 16); }   /* Q15 */
/* the output's level now, 0..256, slewed (fast up, slow down) */
static int32_t bg_energy(void)
{
    static int32_t e;
    static uint32_t fr = 0xFFFFFFFFu;
    uint32_t w = scope_w, i;
    int32_t pk = 0, v;
    if (fr == ui.frame)                                 /* (once a frame: a redraw in the same frame draws the same) */
        return e;
    fr = ui.frame;
    for (i = 1; i <= 256u; i += 2u) {
        v = scope_buf[(w - i) & (SCOPE_N - 1u)];
        if (v < 0) v = -v;
        if (v > pk) pk = v;
    }
    v = pk * 256 / 12000;
    if (v > 256) v = 256;
    e += v > e ? (v - e) * 3 / 4 : (v - e) / 6;
    return e;
}
#define BG_N 96                                         /* vertices of an outline */
/* one outline: the body at (cx, cy), radius r (Q4), x stretched by ax/16; outer: with the spikes, the ripple, the
 * fray. Colour c */
static void bg_outline(const genome_t *g, const bfeat_t *f, int32_t cx, int32_t cy, int32_t r, int outer, uint32_t t,
                       uint16_t c)
{
    int32_t i, px = 0, py = 0;
    for (i = 0; i <= BG_N; i++) {
        uint32_t th = (uint32_t)(i % BG_N) * 65536u / BG_N;
        int32_t rr = r + ((((r * f->wob) >> 8) * ((9175 * bg_sin((uint32_t)f->lob * th + t) +
                                                    3932 * bg_sin((uint32_t)(f->lob + 2) * th - 2u * t)) >> 15)) >> 15);
        int32_t x, y;
        if (outer) {
            int32_t c2 = bg_sin(((uint32_t)f->spikes * th >> 1) + (uint32_t)g->tw * 3000u + 0x4000u), p = c2 > 0 ? c2 : 0, n;
            for (n = 1; n < g->sh; n++)
                p = (p * c2) >> 15;
            if (p < 0) p = 0;
            rr += (((r * f->len) >> 8) * p >> 15) * 26 >> 4;
            rr += (((r * f->ripple) >> 8) * bg_sin(th * 18u + 3u * t) >> 15) / 14;
            if (f->fray)
                rr += ((int32_t)((((uint32_t)i * 2654435761u) + t * 40503u) >> 24) - 128) * ((r * f->fray) >> 8) / 1800;
        }
        x = cx + ((rr * bg_sin(th + 0x4000u) >> 15) * g->ax >> 4);
        y = cy + (rr * bg_sin(th) >> 15);
        if (i)
            og_line(px, py, x, y, c);
        px = x;
        py = y;
    }
}
/* a small ring (a nucleus), Q4 */
static void bg_ring(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t i, px = cx + r, py = cy;
    for (i = 1; i <= 12; i++) {
        uint32_t th = (uint32_t)i * 65536u / 12u;
        int32_t x = cx + (r * bg_sin(th + 0x4000u) >> 15), y = cy + (r * bg_sin(th) >> 15);
        og_line(px, py, x, y, c);
        px = x;
        py = y;
    }
}
/* the being: its isotherms (outer cold .. core hot, all warmer with en 0..256), then its nuclei. cx cy r in Q4 */
static void being_lines(const genome_t *g, const bfeat_t *f, int32_t cx, int32_t cy, int32_t r, int32_t en, uint32_t t)
{
    static const int16_t SC[4] = {256, 180, 112, 52}, HT[4] = {28, 90, 150, 206};
    int32_t i, n;
    r += r * en * 30 / 65536;                            /* (it swells with the sound) */
    for (i = 0; i < 4; i++)
        bg_outline(g, f, cx, cy, r * SC[i] >> 8, !i, t, bg_col(HT[i] + en * 40 / 256));
    for (n = 0; n < f->nuc; n++) {                       /* the nuclei: orbiting, DTN apart */
        uint32_t a = t + (uint32_t)n * 65536u / (uint32_t)f->nuc;
        int32_t d = f->nuc > 1 ? r * (24 + f->sep * 40 / 256) / 100 : 0;
        int32_t x = cx + ((d * bg_sin(a + 0x4000u) >> 15) * g->ax >> 4), y = cy + (d * bg_sin(a) >> 15) * 7 / 10;
        bg_ring(x, y, r * 15 / 100, bg_col(214 + en * 42 / 256));
        bg_ring(x, y, r * 6 / 100, bg_col(240));
    }
}
/* the page's knob c as 0..256 (none: -1) and its label */
static int32_t bg_knob(uint32_t c, const char **lab)
{
    int16_t *vp;
    const param_desc_t *d = page_desc(cur_page(), c, &vp);
    int32_t r = d && d->label && d->label[0] != '-' ? RATIO(d, enum_rank(d, *vp)) : -1;
    *lab = d && d->label ? d->label : "";
    return r < 0 ? -1 : r * 256 / 1000;
}
/* a being's features from four of its track's parameters (ids), each by what it is (its label), else by its place */
static void bg_feat(const track_t *t, const genome_t *g, const uint8_t *ids, bfeat_t *f)
{
    int32_t k[4];
    const char *lab[4];
    uint32_t c;
    for (c = 0; c < 4u; c++) {
        const param_desc_t *d = ids[c] < P_COUNT ? track_desc(t, ids[c]) : 0;
        int32_t r = d && d->label && d->label[0] != '-' ? RATIO(d, enum_rank(d, t->p[ids[c]])) : -1;
        lab[c] = d && d->label ? d->label : "";
        k[c] = r < 0 ? 128 : r * 256 / 1000;
    }
    f->spikes = g->k0 + k[0] * g->kk / 256;
    f->len = 48 + k[1] * 208 / 256;
    f->wob = 40 + k[2] * 216 / 256;
    f->lob = g->lob;
    f->nuc = 1 + k[3] * 3 / 257;
    f->sep = f->fray = f->ripple = 0;
    for (c = 0; c < 4u; c++) {                           /* a knob by what it is */
        const char *l = lab[c];
        if (str_eq(l, "CUT")) { f->spikes = 10 + k[c] * 22 / 256; f->len = k[c] * k[c] / 256; }
        else if (str_eq(l, "RES") || str_eq(l, "RESO")) f->ripple = k[c];
        else if (str_eq(l, "DTN") || str_eq(l, "SPRD") || str_eq(l, "DTUNE")) { f->sep = k[c]; if (f->nuc < 2) f->nuc = 2; }
        else if (str_eq(l, "NOIS") || str_eq(l, "RAND") || str_eq(l, "BRTH") || str_eq(l, "CRSH")) f->fray = k[c];
        else if (str_eq(l, "WAVE")) f->lob = 2 + k[c] * 5 / 257;
    }
}
/* (JIANT 0.5) the sound exaggerated on a being: at energy en (0..256) it bulges (its lobes), its spikes shoot out, its
 * membrane shivers and frays; a hit is seen at once */
static void bg_excite(bfeat_t *f, int32_t en, int32_t k)
{
    f->wob = clamp(f->wob + en * k / 4, 0, 256);
    f->len = clamp(f->len + en * k / 3, 0, 420);
    f->ripple = clamp(f->ripple + en * k / 3, 0, 256);
    f->fray = clamp(f->fray + en * k / 6, 0, 256);
}
/* the page's being: its engine's genome, its four knobs by what they are */
static void graph_being(const track_t *t)
{
    static uint32_t tm, f0;                              /* its time runs with the sound: in silence it rests */
    const genome_t *g = &GENOME[EGEN[t->eng_req % NENGINES]];
    const page_t *pg = cur_page();
    int32_t en = bg_energy();
    bfeat_t f;
    bg_feat(t, g, pg->id, &f);
    f.lob += (int32_t)(ui.page & 1u);                    /* (the engine's second page: a lobe more, the same species) */
    bg_excite(&f, en, 2);
    if (en > 2) tm += (ui.frame - f0) * (uint32_t)(160 + en * 3);
    f0 = ui.frame;
    being_lines(g, &f, 120 * 16, 61 * 16, (36 * 16) * (256 + en / 3) / 256, en, tm);
}

/* (JIANT 0.5) HOME: the system as an ecosystem. A being per track (its engine's genome, its HOME knobs by what they
 * are), each warmed, swollen and moved by its own sound (the part's peak, slewed); a muted one a dim outline, the
 * selected one larger, its number in the theme colour. Still in silence */
static void graph_ecosys(void)
{
    static const int16_t PX[NTRK] = {44, 120, 196, 64, 146, 206}, PY[NTRK] = {34, 28, 40, 84, 78, 86};
    static int32_t en[NTRK], sx[NTRK], sy[NTRK], vx[NTRK], vy[NTRK];   /* (positions and speeds, Q8 px; a frame) */
    static uint32_t tm[NTRK], f0;
    static uint8_t init;
    uint32_t c, k, dt = ui.frame - f0, still = (ui_prefs & PREF_ANIM_OFF) != 0u;
    if (!init) {
        init = 1;
        for (c = 0; c < NTRK; c++) {
            sx[c] = PX[c] << 8; sy[c] = PY[c] << 8;
            vx[c] = (c & 1u) ? 90 : -110; vy[c] = (c & 2u) ? 70 : -60;
        }
    }
    if (dt > 8u) dt = 8u;
    if (still) dt = 0;
    for (k = 0; k < 22u; k++) {                         /* the cytoplasm: granules drifting */
        uint32_t h = k * 2654435761u;
        int32_t gx = 10 + (int32_t)((h >> 8) % 220u + ui.frame * (1u + (h >> 28)) / 6u) % 220;
        int32_t gy = 6 + (int32_t)(((h >> 16) % 110u) + (still ? 0 : (uint32_t)(bg_sin((uint32_t)(ui.frame * 300u + h)) >> 13))) % 110;
        cv_rect(gx, gy, 1 + (int32_t)(h >> 31), 1 + (int32_t)(h >> 31), ux_mix(T_SURF, T_THEME, 22 + (int32_t)(h >> 29) * 4));
    }
    for (c = 0; c < NTRK; c++) {
        track_t *t = &trk[c];
        uint32_t e = t->eng_req % NENGINES;
        const genome_t *g = e == ENGI_DRUM ? &LANE_G_HOME : &GENOME[EGEN[e]];
        int32_t v = t->peak * 256 / 7000, r = (c == song.sel ? 17 : 12) * 16, rad, sp;
        uint8_t ids[4];
        bfeat_t f;
        char num[2] = {(char)('1' + c), 0};
        if (v > 256) v = 256;
        if (dt) {                                   /* (once a frame: a redraw in the same frame draws the same) */
            t->peak = 0;
            en[c] = v > en[c] ? v : en[c] + (v - en[c]) / 4;   /* (a hit at once, a quick fall) */
        }
        if (en[c] > 2) tm[c] += dt * (uint32_t)(200 + en[c] * 5);
        else tm[c] += dt * 60u;                     /* (silent: it still breathes, slowly) */
        for (v = 0; v < 4; v++) ids[v] = (uint8_t)ENGINES[e]->knob[v];
        bg_feat(t, g, ids, &f);
        bg_excite(&f, en[c], 4);                    /* (HOME: the most excited) */
        r = r * (256 + en[c] * 5 / 4 + (bg_sin(tm[c] / 3u) >> 11)) / 256;   /* (it pulses, and swells up to ~2.2 x) */
        rad = r >> 4;
        sp = 256 + en[c] * 6;                       /* (it swims; a hit kicks it on, up to ~7 x) */
        for (k = 0; k < dt; k++) {                  /* swimming in the panel, bouncing off its walls, wandering */
            vx[c] += bg_sin(ui.frame * 211u + c * 16000u + k * 977u) >> 12;
            vy[c] += bg_sin(ui.frame * 157u + c * 23000u + k * 613u) >> 12;
            vx[c] = clamp(vx[c], -140, 140);
            vy[c] = clamp(vy[c], -110, 110);
            sx[c] += vx[c] * sp >> 8;
            sy[c] += vy[c] * sp >> 8;
            if (sx[c] < (rad + 8) << 8) { sx[c] = (rad + 8) << 8; vx[c] = -vx[c]; }
            if (sx[c] > (232 - rad) << 8) { sx[c] = (232 - rad) << 8; vx[c] = -vx[c]; }
            if (sy[c] < (rad + 4) << 8) { sy[c] = (rad + 4) << 8; vy[c] = -vy[c]; }
            if (sy[c] > (104 - rad) << 8) { sy[c] = (104 - rad) << 8; vy[c] = -vy[c]; }
        }
        if (t->p[P_MUTE])
            bg_outline(g, &f, sx[c] >> 4, sy[c] >> 4, r, 1, tm[c], ux.mono || settings.palette == UI_BW_INDEX ?
                       ux_gray(ux_luma(T_DIM) >> 3) : T_DIM);
        else
            being_lines(g, &f, sx[c] >> 4, sy[c] >> 4, r, en[c], tm[c] + c * 20000u);
        cv_text_in((sx[c] >> 8) - 30, clamp((sy[c] >> 8) + rad / 2 + 4, 4, 104), 60, &AF_S, num,
                   c == song.sel ? T_THEME : t->p[P_MUTE] ? T_DIM : T_MID, T_SURF);
    }
    f0 = ui.frame;
}

/* DRUM-X: the colony. Two rows of four, the sound's name under each */
static const genome_t LANE_G[8] = {
    {3, 0, 2, 2, 16, 0, 2},                             /* BD: round, heavy */
    {12, 0, 14, 3, 16, 0, 6},                           /* SD: spiky */
    {5, 0, 8, 5, 18, 2, 3},                             /* CP: a cluster */
    {22, 0, 6, 4, 16, 0, 10},                           /* CH: a short fringe */
    {18, 0, 12, 4, 16, 1, 8},                           /* OH: a longer fringe */
    {4, 0, 4, 3, 17, 0, 2},                             /* TM: round, wobbly */
    {4, 0, 9, 4, 16, 0, 1},                             /* RS: blunt, square */
    {5, 0, 16, 5, 16, 0, 5},                            /* CB: a star */
};
static void graph_colony(const track_t *t, int32_t sel)
{
    const drum_lane_t *K = drum_kit_of(t);
    int32_t morph = clamp(t->p[P_E0] + dx_mot_v, 0, 127), mx = 30 + morph * 180 / 127;   /* (0.5.1: X-MOD live) */
    uint32_t l;
    cv_rect(30, 12, 180, 1, T_LINE);                     /* MORPH: A .. B */
    cv_rect(30, 11, mx - 30, 3, heat_col(morph * 2));
    cv_text(12, 8, &AF_S, "A", morph < 64 ? T_THEME : T_DIM);
    cv_text_r(226, 8, &AF_S, "B", morph >= 64 ? T_THEME : T_DIM, T_SURF);
    for (l = 0; l < 8u; l++) {
        int32_t x = 33 + (int32_t)(l % 4u) * 58, y = 38 + (int32_t)(l / 4u) * 46;
        int32_t e = K && K[l].x.live ? K[l].x.ea >> 22 : 0, muted = dx_lane_muted(l);
        const genome_t *g = &LANE_G[l];
        bfeat_t f = {g->k0, g->ss * 6, 40, g->lob, l == 2u ? 3 : 1, 160, 0, 0};
        if (muted)
            bg_outline(g, &f, x * 16, y * 16, 8 * 16, 1, l * 9000u, ux.mono || settings.palette == UI_BW_INDEX ? ux_gray(ux_luma(T_DIM) >> 3) : T_DIM);
        else if (e) {
            f.len = g->ss * 6 + e;
            f.wob = 40 + e * 3 / 4;
            f.ripple = e / 2;
            being_lines(g, &f, x * 16, y * 16, (8 + e * 8 / 256) * 16, e, ui.frame * 1500u + l * 9000u);
        } else
            bg_outline(g, &f, x * 16, y * 16, 8 * 16, 1, l * 9000u, bg_col(24));
        cv_text_in(x - 14, y + 14, 28, &AF_S, drum_lane_abbr(t, l), sel == (int32_t)l ? T_THEME : muted ? T_DIM : T_MID, T_SURF);
    }
}
