/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * JIANT 0.5: the technical graphs of the pages that had none (they showed the scope): what the four knobs do, drawn
 * as an engineer would, live where the sound is (thermal colours: heat_col / heat_y).
 *   DIST       the transfer curve of the drive's TYPE (in -> out), the unity line, the level now as a dot on it; the
 *              tone as a low-pass response at the right
 *   DLY        the echoes as an impulse train: the first at MIX, each FDBK lower, TIME apart; a playhead at tempo
 *   DLY 2      the echoes left (top) and right (bottom): WIDTH sets the right ones later, PITCH steps them up / down,
 *              SPRAY scatters them
 *   REVERB     the impulse response: PRE's gap, then a dense tail as long as SIZE, cooling as fast as DAMP (SPRING: a
 *              chirp)
 *   REVERB 2   the tail left (up) and right (down), WIDE apart, MOD wobbling it (at RATE, while it sounds), FILT tilting
 *   CHORUS     the taps' delay lines: three sines a third apart, as deep as DEPTH, running at RATE while it sounds
 *   MASTER     CLIP's curve with the level now; PUNCH's transient shape; DUCK's gain now as a meter
 *   ENV / LFO DEST  the source now (the envelope's level, the LFO's value) into four bipolar columns under the cards,
 *              each as long as its amount and lit by what it sends now
 *   VOICE      the track's voices: one slot each, as full as its envelope now; the glide as a curve between two notes
 *   GLOBAL     a bar of 16 steps with SWING moving the off-beats, the playhead, the beat */
#define TK_X0 PANEL_X0
#define TK_X1 (PANEL_X0 + PANEL_W)
#define TK_Y0 8
#define TK_Y1 112
/* the current page's knob c as 0..1000 (no knob: 500) */
static int32_t tk(uint32_t c)
{
    int16_t *vp;
    const param_desc_t *d = page_desc(cur_page(), c, &vp);
    int32_t r = d && d->label && d->label[0] != '-' ? RATIO(d, enum_rank(d, *vp)) : -1;
    return r < 0 ? 500 : r;
}
/* .. as an amount -500..500: a bipolar knob about its middle, a unipolar one from 0 up */
static int32_t tk_amt(uint32_t c)
{
    int16_t *vp;
    const param_desc_t *d = page_desc(cur_page(), c, &vp);
    return d && d->min >= 0 ? tk(c) / 2 : tk(c) - 500;
}
static void tk_axes(int32_t x0, int32_t y0, int32_t x1, int32_t y1)   /* a plot's frame: corner ticks, the axes */
{
    cv_rect(x0, y1, x1 - x0, 1, T_RAISE);
    cv_rect(x0, y0, 1, y1 - y0, T_RAISE);
    cv_rect(x1 - 4, y0, 5, 1, T_RAISE);
    cv_rect(x1, y0, 1, 5, T_RAISE);
}
static void tk_dot(int32_t x, int32_t y) { cv_rrect(x - 3, y - 3, 7, 7, 3, T_TEXT, T_SURF); }

/* DIST: in -> out of dist_shape's maths (the input's full scale +-131072 as the track's mix sees it) */
static int32_t tk_dist_y(uint32_t type, int32_t x, int32_t g, int32_t d)
{
    int32_t v = ((x >> 2) * g) >> 10, a;
    switch (type) {
    case DT_HARD: return clamp(v, -22000, 22000) * 32767 / 26000;
    case DT_FOLD:
        v = clamp(v, -110000, 110000);
        for (a = 0; a < 3; a++)
            v = v > 22000 ? 44000 - v : v < -22000 ? -44000 - v : v;
        return v * 32767 / 26000;
    case DT_CRUSH: {
        uint32_t sh = 4u + (uint32_t)d / 12u;
        x = (int32_t)((uint32_t)(clamp(x, -131072, 131068) >> sh) << sh);
        return clamp(((x >> 2) * g) >> 10, -26000, 26000) * 32767 / 26000;
    }
    case DT_RECT: a = x < 0 ? -x : x; return softclip((((a - 65536) >> 2) * g) >> 10);
    default: return softclip(v + 2400) - softclip(2400);
    }
}
static void tech_dist(const track_t *t)
{
    int32_t d = t->p[P_DIST], g = 4096 + d * d * 2, i, py = 0, x0 = 22, y0 = TK_Y0, s = 100, cx = x0 + s / 2;
    int32_t cy = y0 + s / 2, en = bg_energy(), tone = t->p[P_DTONE];
    uint32_t type = (uint32_t)t->p[P_DTYPE] > DT_RECT ? DT_SOFT : (uint32_t)t->p[P_DTYPE];
    tk_axes(x0, y0, x0 + s, y0 + s);
    cv_rect(x0, cy, s, 1, T_LINE);
    cv_rect(cx, y0, 1, s, T_LINE);
    for (i = 0; i <= s; i += 4)                         /* the unity line, dotted */
        cv_rect(x0 + i, y0 + s - i, 1, 1, T_DIM);
    for (i = 0; i <= s; i++) {
        int32_t x = (i - s / 2) * 131072 / (s / 2), y = cy - tk_dist_y(type, x, d ? g : 4096, d) * (s / 2) / 32767;
        y = clamp(y, y0, y0 + s);
        if (i)
            cv_line_t(x0 + i - 1, py, x0 + i, y, heat_y(y < cy ? y : 2 * cy - y, y0, cy, T_THEME), 2);
        py = y;
    }
    if (en > 2) {                                       /* the level now, on the curve */
        int32_t xi = s / 2 + en * (s / 2) / 256;
        tk_dot(x0 + xi, clamp(cy - tk_dist_y(type, en * 512, d ? g : 4096, d) * (s / 2) / 32767, y0, y0 + s));
    }
    {   /* the tone: a low-pass's response, its corner from TONE and DRIVE (fx.c track_dist k) */
        int32_t k = clamp(32000 - d * 95 + tone * 300, 2500, 32767), fx = 140, fw = 82, fc = fx + 8 + (k - 2500) * (fw - 34) / 30267, f;
        tk_axes(fx, y0 + 30, fx + fw, y0 + s);
        py = y0 + 34;
        for (f = 0; f <= fw; f++) {
            int32_t r = fx + f <= fc ? 0 : (fx + f - fc) * (fx + f - fc) / 6, y = clamp(y0 + 34 + r, y0 + 34, y0 + s);
            if (f)
                cv_line_t(fx + f - 1, py, fx + f, y, heat_y(y, y0 + 34, y0 + s, T_THEME), 2);
            py = y;
        }
    }
}

/* DLY: the echoes. span: six of them */
static void tech_dly(void)
{
    int32_t time = (int32_t)delay_samples(), mix = song.g[G_DMIX], fb = song.g[G_DFDBK], amp = mix * 1000 / 127, n;
    int32_t x0 = TK_X0 + 6, w = PANEL_W - 12, base = TK_Y1 - 4, h = 88, tone = song.g[G_DCOLOR] - 64;
    tk_axes(x0, TK_Y0, x0 + w, base);
    cv_rect(x0, base - h, 3, h, T_TEXT);                /* the dry hit */
    for (n = 1; n <= 6 && amp > 4; n++) {
        int32_t x = x0 + n * w / 7, bh = amp * h / 1000;
        int32_t heat = clamp(60 + amp * 180 / 1000 + tone * n, 8, 250);   /* (the tone: each repeat warmer / colder) */
        cv_rect(x - 2, base - bh, 4, bh, heat_col(heat));
        amp = amp * (fb > 100 ? 100 : fb) / 100;        /* (past 100: it grows, held at the top here) */
        if (fb > 100) amp = 1000;
    }
    if (song.playing && time > 0) {                     /* the playhead: where the echo period is now */
        int32_t x = x0 + (int32_t)((ms_clock % (uint32_t)(time * 7)) * (uint32_t)w / (uint32_t)(time * 7));
        cv_rect(x, TK_Y0, 1, base - TK_Y0, T_MID);
    }
}

/* DLY 2: left echoes on top, right below */
static void tech_dly2(void)
{
    int32_t wd = tk(0), pit = song.g[G_DPIT], spry = tk(2), n, x0 = TK_X0 + 6, w = PANEL_W - 12, mid = (TK_Y0 + TK_Y1) / 2;
    cv_rect(x0, mid, w, 1, T_RAISE);
    for (n = 1; n <= 6; n++) {
        int32_t jit = (int32_t)((n * 2654435761u) >> 27) - 16, x = x0 + n * w / 7 + jit * spry / 1000;
        int32_t up = clamp(pit * n * 2, -36, 36), hot = 220 - n * 26;
        cv_rect(x - 2, mid - 8 - 30 + up / 2, 4, 30, heat_col(hot));                        /* left */
        cv_rect(x - 2 + wd * 18 / 1000, mid + 8 - up / 2, 4, 30, heat_col(hot - 10));        /* right, WIDTH later */
    }
}

/* REVERB: the impulse response */
static void tech_rev(void)
{
    int32_t size = song.g[G_RSIZE], damp = song.g[G_RDAMP], pre = song.g[G_RPRE], spring = song.g[G_RTYPE] == 1;
    int32_t x0 = TK_X0 + 6, w = PANEL_W - 12, base = TK_Y1 - 4, h = 92, len = 40 + size * (w - 50) / 127, gap = pre * 40 / 100;
    int32_t i, ty = song.g[G_RTYPE], mod = song.g[G_RMOD];
    tk_axes(x0, TK_Y0, x0 + w, base);
    if (ty == 3) {                                      /* RESO: four strings, each a comb's teeth at its pitch, ringing as
                                                         * long as SIZE lets them, as bright as DAMP leaves them */
        for (i = 0; i < 4; i++) {
            int32_t x = x0 + 30 + i * 44 + (mod > 64 && i >= 2 ? 10 : 0), k, hh = 30 + size * 55 / 127;
            for (k = 0; k < 6; k++) {
                int32_t th = hh - k * hh / (3 + (127 - size) / 20 + 1), yy;
                if (th <= 0) break;
                yy = base - th;
                cv_rect(x + k * (6 + i), yy, 2, th, heat_col(clamp(220 - k * 30 - damp, 20, 236)));
            }
            cv_rect(x - 2, base - hh - 6, 1, hh + 6, T_MID);
        }
        return;
    }
    if (ty == 4) {                                      /* CLOUD: its bloom, the grains scattered back over SIZE, as many as RATE,
                                                         * up (an octave), down and level by MOD; FREEZE: a line over them */
        int32_t ng = 12 + song.g[G_RRATE] / 3, spread = 40 + size * (w - 40) / 127;
        for (i = 0; i < w - 8; i += 2) {                 /* (0.5.1) the bloom: the network's tail under the grains,
                                                         * longer with SIZE */
            int32_t env = 1000 - i * 1000 / (40 + size * (w - 40) / 127 + 1), bh;
            if (env <= 0)
                break;
            bh = env * (28 + (int32_t)(((uint32_t)(i + 3) * 2654435761u) >> 27)) / 1000;
            cv_rect(x0 + 6 + i, base - bh, 1, bh, heat_col(clamp(env * 120 / 1000 - damp / 4, 6, 236)));
        }
        for (i = 0; i < ng; i++) {
            uint32_t hsh = (uint32_t)(i + 1) * 2654435761u;
            int32_t gx = x0 + w - 6 - (int32_t)((hsh >> 8) % (uint32_t)spread), lane = (int32_t)((hsh >> 20) % 100u) < mod * 3 / 4 ?
                         (int32_t)((hsh >> 24) % 3u) + 1 : 0;
            int32_t gy = base - 46 - (lane == 1 ? 26 : lane == 2 ? -26 : lane == 3 ? 14 : 0) + (int32_t)((hsh >> 4) % 9u) - 4;
            int32_t gw = 6 + (int32_t)((hsh >> 12) % 10u);
            cv_rect(gx - gw / 2, gy, gw, 3, heat_col(clamp(60 + (int32_t)((hsh >> 16) % 160u) - damp / 2, 10, 236)));
        }
        if (size >= 127)
            cv_rect(x0 + 4, TK_Y0 + 4, w - 8, 1, T_ACCENT);
        return;
    }
    cv_rect(x0, base - h, 3, h, T_TEXT);                /* the dry hit */
    for (i = 0; i < len && x0 + 6 + gap + i < x0 + w; i += 2) {
        int32_t env = 1000 - i * 1000 / len, r = (int32_t)(((uint32_t)(i + 7) * 2654435761u) >> 22) % 1000;
        int32_t bh = env * (400 + r * 6 / 10) / 1000 * h / 1000;
        int32_t heat = clamp(env * 230 / 1000 - i * damp / (len + 1), 6, 236);
        if (spring)
            bh = bh * (600 + (bg_sin((uint32_t)(i * i * 9)) >> 6) * 400 / 512) / 1000;
        if (bh > 0)
            cv_rect(x0 + 6 + gap + i, base - bh, 1, bh, heat_col(heat));
        if (ty == 2 && mod && i % 6 == 0 && bh > 4)   /* SHIMMER: sparks an octave up over the tail, as many as MOD */
            cv_rect(x0 + 6 + gap + i, base - bh - 6 - (i * 7 % 23) * mod / 127, 2, 2, heat_col(clamp(heat + 40, 0, 250)));
    }
}

/* REVERB 2: the tail left (up) and right (down) */
static void tech_rev2(void)
{
    int32_t mod = tk(0), wide = tk(3), filt = song.g[G_RFILT], x0 = TK_X0 + 6, w = PANEL_W - 12, mid = (TK_Y0 + TK_Y1) / 2;
    int32_t i, ph = bg_energy() > 2 ? (int32_t)ui.frame * (8 + tk(1) / 20) : 0;
    cv_rect(x0, mid, w, 1, T_RAISE);
    for (i = 0; i < w; i += 2) {
        int32_t env = 1000 - i * 1000 / w, wob = (bg_sin((uint32_t)(i * 700 + ph * 64)) * mod / 1000) >> 12;
        int32_t bh = env * 44 / 1000, off = wide * 6 / 1000;
        int32_t heat = clamp(env * 200 / 1000 + filt, 6, 236);
        if (bh <= 0) continue;
        cv_rect(x0 + i + wob, mid - 2 - off - bh, 1, bh, heat_col(heat));
        cv_rect(x0 + i - wob, mid + 2 + off, 1, bh, heat_col(heat - 12));
    }
}

/* CHORUS: three taps' delay modulation */
static void tech_chorus(void)
{
    int32_t rate = tk(0), depth = tk(1), x0 = TK_X0 + 6, w = PANEL_W - 12, mid = (TK_Y0 + TK_Y1) / 2, k, i;
    uint32_t ph = bg_energy() > 2 ? ui.frame * (uint32_t)(40 + rate) * 8u : 0u;
    cv_rect(x0, mid, w, 1, T_RAISE);
    for (k = 0; k < 3; k++) {
        int32_t py = mid;
        for (i = 0; i <= w; i++) {
            int32_t y = mid - ((bg_sin((uint32_t)(i * (60 + rate / 4)) + ph + (uint32_t)k * 21845u) * depth / 1000 * 40) >> 15);
            if (i)
                cv_line_t(x0 + i - 1, py, x0 + i, y, heat_y(y, mid - 40, mid + 40, T_THEME), k ? 1 : 2);
            py = y;
        }
    }
}

/* MASTER: the clipper's curve, PUNCH's shape, DUCK's gain now */
static void tech_master(void)
{
    int32_t clip = song.g[G_CLIP], g = 1024 + clip * 31, i, py = 0, x0 = 22, y0 = TK_Y0, s = 100, cy = y0 + s / 2;
    int32_t en = bg_energy(), punch = song.g[G_PUNCH], duck = song.g[G_DUCK];
    tk_axes(x0, y0, x0 + s, y0 + s);
    cv_rect(x0, cy, s, 1, T_LINE);
    for (i = 0; i <= s; i++) {
        int32_t x = (i - s / 2) * 32767 / (s / 2), y = cy - softclip(x * g >> 10) * (s / 2) / 32767;
        if (i)
            cv_line_t(x0 + i - 1, py, x0 + i, y, heat_y(y < cy ? y : 2 * cy - y, y0, cy, T_THEME), 2);
        py = y;
    }
    if (en > 2)
        tk_dot(x0 + s / 2 + en * (s / 2) / 256, cy - softclip(en * 128 * g >> 10) * (s / 2) / 32767);
    {   /* PUNCH: a drum hit's envelope (~190 ms) times punch_gain (eng_drum.c): the push, the fuller body */
        int32_t px0 = 136, pw = 60, b = y0 + s, py2 = b - 45;
        tk_axes(px0, y0 + 20, px0 + pw, b);
        for (i = 0; i <= pw; i++) {
            int32_t e = 45 * (pw - i) / pw, y = b - (e * punch_gain((uint32_t)i * 255u / (uint32_t)pw, punch) >> 15);
            if (i)
                cv_line_t(px0 + i - 1, py2, px0 + i, y, heat_y(y, y0 + 20, b, T_THEME), 2);
            py2 = y;
        }
    }
    {   /* DUCK: the gain the other parts get now (full: no duck) */
        int32_t mx = 210, mh = s, gh = duck_g1 * mh / 32767;
        cv_rect(mx, y0, 10, mh, T_RAISE);
        cv_rect(mx, y0 + mh - gh, 10, gh, duck ? heat_col(60 + gh * 180 / mh) : T_DIM);
    }
}

/* ENV DEST / LFO DEST: the source now into four bipolar columns, one under each card */
static void tech_dest(const track_t *t, int lfo)
{
    int32_t src = 0, c, mid = (TK_Y0 + TK_Y1) / 2, half = 44;
    uint32_t i;
    if (lfo)
        src = mulq15(t->lfo_val, t->lfo_fade);
    else
        for (i = 0; i < NVOICE; i++)
            if (t->v[i].stage && t->v[i].stage < 4u && (t->v[i].env >> 9) > src)
                src = t->v[i].env >> 9;                 /* Q15 */
    for (c = 0; c < 4; c++) {
        int32_t x = CARD_X(c) + 22, amt = tk_amt((uint32_t)c), bh = amt * half / 500, live = bh * src / 32767;
        cv_rect(x - 6, mid, 25, 1, T_RAISE);
        cv_rect(x + 6, mid - half, 1, 2 * half, T_LINE);
        if (bh > 0) cv_rect(x, mid - bh, 13, bh, ux_mix(T_SURF, T_THEME, 35));
        else if (bh < 0) cv_rect(x, mid, 13, -bh, ux_mix(T_SURF, T_THEME, 35));
        if (live > 0) cv_rect(x, mid - live, 13, live, heat_col(80 + live * 170 / half));
        else if (live < 0) cv_rect(x, mid, 13, -live, heat_col(80 - live * 170 / half));
    }
    {   /* the source: its level now as a bar at the left edge */
        int32_t sh = (src < 0 ? -src : src) * half / 32767;
        cv_rect(TK_X0, src >= 0 ? mid - sh : mid, 3, sh ? sh : 1, T_TEXT);
    }
}

/* VOICE: the track's voices now; the glide between two notes */
static void tech_voice(const track_t *t)
{
    uint32_t i, nv = trk_nvoice(t) ? trk_nvoice(t) : NVOICE;
    int32_t sw = PANEL_W / (int32_t)NVOICE, glide = t->p[P_GLIDE], py = 0, x;
    for (i = 0; i < NVOICE; i++) {
        int32_t x0 = TK_X0 + (int32_t)i * sw + 2, h = 40, lv = 0;
        if (t->v[i].stage && t->v[i].stage < 4u)
            lv = (int32_t)(((int64_t)t->v[i].env * h) >> 24);
        cv_rrect(x0, TK_Y0, sw - 4, h, 2, i < nv ? T_RAISE : T_LINE, T_SURF);
        if (lv > 0)
            cv_rect(x0 + 1, TK_Y0 + h - lv, sw - 6, lv, heat_col(60 + lv * 190 / h));
    }
    for (x = 0; x <= PANEL_W - 20; x++) {               /* the glide: low note to high, as slow as GLIDE */
        int32_t tm = glide ? x * 127 * 6 / (glide * 4 + 1) : 1000, k = tm > 255 ? 255 : tm;
        int32_t y = TK_Y1 - 4 - 40 * (x < 30 ? 0 : (k * (255 - (255 - k) * (255 - k) / 255)) / 255) / 255;
        if (x < 30) y = TK_Y1 - 4;
        if (x)
            cv_line_t(TK_X0 + 10 + x - 1, py, TK_X0 + 10 + x, y, heat_y(y, TK_Y1 - 44, TK_Y1 - 4, T_THEME), 2);
        py = y;
    }
}

/* GLOBAL (JIANT 0.5): the bar as 16 columns, one a sixteenth, in four beats; each as hot as the tempo (40 BPM cold ..
 * 240 white), the downbeats taller, the off-beats late by SWING, the one playing now white; under them CLK's three
 * sources (the one followed lit, its name over it) and TUNE as a bar from the middle (flat left, sharp right) */
static void tech_global(void)
{
    int32_t sw = song.g[G_SWING], bpm = song.g[G_BPM], tune = song.g[G_TUNE], i, x0 = TK_X0 + 6, w = PANEL_W - 12;
    int32_t step = w / 16, base = 60, hot = 40 + (bpm - 40) * 190 / 200, now = -1, cx, k;
    static const char *const CK[3] = {"INT", "USB", "TRS"};
    if (song.playing) {
        uint32_t bar = div_samples(2) * 16u;
        now = bar ? (int32_t)((ms_clock % bar) / div_samples(2)) : -1;
    }
    cv_rect(x0, base + 1, w, 1, T_RAISE);
    for (i = 0; i < 16; i++) {
        int32_t x = x0 + i * step + ((i & 1) ? sw * step / 200 : 0), hh = i % 4 == 0 ? 46 : i % 2 == 0 ? 30 : 20;
        uint16_t c = i == now ? T_TEXT : heat_col(hot - (i % 4 ? 30 : 0) + ((i & 1) ? sw / 2 : 0));
        cv_rect(x + 2, base - hh, step - 5, hh, c);
    }
    for (k = 0; k < 3; k++) {                           /* CLK: the sources */
        int32_t x = x0 + k * 40, on = song.g[G_CLOCK] == k;
        cv_rrect(x, 74, 34, 6, 3, on ? heat_col(200) : T_RAISE, T_SURF);
        cv_text_in(x, 82, 34, &AF_S, CK[k], on ? T_TEXT : T_DIM, T_SURF);
    }
    cx = x0 + 120 + (w - 120) / 2;                      /* TUNE: from the middle */
    cv_rect(x0 + 124, 76, w - 124, 2, T_RAISE);
    cv_rect(cx, 70, 1, 14, T_MID);
    if (tune)
        cv_rect(tune > 0 ? cx + 1 : cx + tune * ((w - 124) / 2) / 50, 74, (tune > 0 ? tune : -tune) * ((w - 124) / 2) / 50, 6,
                heat_col(60 + (tune > 0 ? tune : -tune) * 180 / 50));
}

/* the page's technical graph, if it has one (1) */
static int graph_tech(const track_t *t, const page_t *pg)
{
    if (pg->scope != SC_GLOBAL)
        switch (pg->id[0]) {
        case P_DIST: tech_dist(t); return 1;
        case P_ED_FLT: tech_dest(t, 0); return 1;
        case P_LD_PIT: tech_dest(t, 1); return 1;
        case P_VOICE: tech_voice(t); return 1;
        case P_MSDIV: cv_oy = GOY; graph_mseq(t, T_THEME); return 1;   /* (MSEQ's, on the 100 px scale) */
        default: return 0;
        }
    switch (pg->id[0]) {
    case G_DTIME: tech_dly(); return 1;
    case G_WIDTH: tech_dly2(); return 1;
    case G_RTYPE: tech_rev(); return 1;
    case G_RMOD: tech_rev2(); return 1;
    case G_CRATE: tech_chorus(); return 1;
    case G_CLIP: tech_master(); return 1;
    case G_BPM: tech_global(); return 1;
    default: return 0;
    }
}
