/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * JIANT (FELUCCA TONIC): after SLOOP 2.4.1's arranger, live sections and SONG REC (isod89/sloop-fm1, GPL-3.0). */
/* The song, the audio ISR's part (song_chain.c: what it is). Included at the end of seq.c. */
static uint32_t chain_bar(void) { return 16u * div_samples(2); }        /* a 4/4 bar: 16 steps of 1/16 (DIV 2) */

/* the staged section in: every track from its step 0 (seq_tick: chain.carry samples into it) */
static void sec_apply(void)
{
    uint32_t i, j;
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        const sec_trk_t *s = &sec_stage.t[i];
        seq_release(t);
        motion_restore(t);
        if (t->eng_req != s->engine)
            panic_req |= (uint8_t)(1u << i);              /* (another engine: what the keys hold ends too) */
        t->eng_req = s->engine;
        t->preset = s->preset;
        t->user = 0;
        for (j = 0; j < P_COUNT; j++)
            t->p[j] = s->p[j];
        memcpy(t->step, s->step, sizeof t->step);
        memcpy(fm6_patch[i], s->fm6, FP_SIZE + 1u);
        fm6_pgen[i]++;
        t->seq_idx = (uint16_t)(t->p[P_SLEN] - 1);
        t->seq_pos = 0x7FFFFFFFu;
        t->rh_n = t->rskip_n = 0;
        t->rat_left = 0;
    }
    motion = sec_stage.motion;
    memcpy(dx_kit, sec_stage.dx, sizeof dx_kit);
    dx_mute_set(sec_stage.dx_mute);
    memcpy(pfx_lane, sec_stage.pfx_lane, sizeof pfx_lane);
    memcpy(&song.g[G_SLMODE], sec_stage.slg, sizeof sec_stage.slg);   /* (0.6.3) the SLICER's bus as the section has it */
    pfx_ltgt = sec_stage.pfx_ltgt;
    pfx_lph = 0;                                      /* (the lane from the section's start) */
    memset(motion_active, 0, sizeof motion_active);
    memset(motion_locked, 0, sizeof motion_locked);
    motion_base_valid = 0;
    live.cur = (int8_t)sec_stage.section;
    sec_stage.ready = 0;
    chain.applied = 1;
}
/* (JIANT) the scene in effect onto what the section brought: its mutes; a row's own scene also the macros and the
 * punch-in effects (which nothing else sets) */
static void scene_apply(uint32_t row)
{
    const scene_t *s;
    uint32_t i;
    if (chain.loop || row >= CHAIN_ROWS)
        return;                                       /* (a quick chain: no scenes) */
    if (song_idx.scene[song_cur][row].on) {
        s = &song_idx.scene[song_cur][row];
        chain.scn = (uint8_t)(row + 1u);
        memcpy(macro_v, s->mac, sizeof macro_v);
        for (i = 0; i < 4u; i++)
            macro_v[i] &= 127u;
        scene_pfx = ((uint32_t)s->pfx << PF_OCTD) & PF_MIDI;
    } else if (chain.scn) {
        s = &song_idx.scene[song_cur][chain.scn - 1u];
    } else {
        return;
    }
    for (i = 0; i < NTRK; i++)
        trk[i].p[P_MUTE] = (int16_t)(i < 4u ? (s->mute >> i) & 1u : (s->on >> (i - 3u)) & 1u);
    dx_mute_set((uint16_t)((dx_mute & ~DXG_ALL) | ((s->mute >> 4) & DXG_ALL)));
}
static void chain_apply(void)                         /* a song row's section in */
{
    chain.row = sec_stage.row;
    chain.remaining = chain.config.row[chain.row].bars;
    sec_apply();
    scene_apply(chain.row);
}

/* SONG REC: a row for section s, its bars counted from 0 */
static void srec_finish(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < live.nrec; i++)
        if (live.rec[i].bars) {
            live.rec_scene[n] = live.rec_scene[i];
            live.rec[n++] = live.rec[i];
        }
    live.rec_done = (uint8_t)(n ? n : 0xFFu);
    live.srec = 0;
    live.nrec = 0;
}
static void srec_add(uint32_t s)
{
    if (live.nrec >= CHAIN_ROWS) {
        srec_finish();                                /* full: what was played so far is the song */
        return;
    }
    live.rec[live.nrec].slot = (uint8_t)(s & 3u);
    live.rec[live.nrec].bars = 0;
    scene_capture(&live.rec_scene[live.nrec]);       /* (JIANT: the row's scene, as it begins) */
    live.nrec++;
}
static void srec_bar(void)                            /* a bar ended */
{
    if (live.srec == 2u && live.nrec) {
        chain_row_t *e = &live.rec[live.nrec - 1u];
        if (e->bars < CHAIN_BARS) {
            e->bars++;
        } else {                                      /* (64 bars of one section: it goes on in a new row) */
            srec_add(e->slot);
            if (live.srec == 2u)
                live.rec[live.nrec - 1u].bars = 1;
        }
    } else if (live.srec == 1u) {                     /* armed: it records from this bar */
        live.srec = 2;
        live.nrec = 0;
        srec_add(live.cur >= 0 ? (uint32_t)live.cur : 0u);
    }
}
static void srec_stop(void)                           /* SONG REC pressed again, or STOP: a begun bar counts */
{
    if (live.srec == 2u && live.nrec && chain.bar_pos && live.rec[live.nrec - 1u].bars < CHAIN_BARS)
        live.rec[live.nrec - 1u].bars++;
    if (live.srec == 2u)
        srec_finish();
    live.srec = 0;
}

static void chain_start(void)                         /* (seq_start) */
{
    chain.carry = 0;
    chain.bar_pos = 0;
    chain.armed_bar = 0;
    if (!chain.armed)
        return;
    chain.armed = 0;
    if (!sec_stage.ready || sec_stage.row != 0u)
        return;                                       /* (cannot happen: chain_prepare staged row 0) */
    chain.rec = song.rec;
    song.rec = 0;                                     /* a song plays, it does not record */
    chain.running = 1;
    chain.scn = 0;
    chain_apply();
}
static void chain_stop(void)                          /* (seq_stop) */
{
    chain.armed = 0;
    chain.armed_bar = 0;
    live.req = -1;
    srec_stop();
    if (!chain.running)
        return;
    song.rec = chain.rec;
    chain.running = 0;
    scene_pfx = 0;                                    /* (a scene's punch-in effects end with the song) */
    chain.ended = chain.kept;                         /* (a quick chain keeps nothing: what plays stays) */
}
static void chain_tick(uint32_t n)
{
    uint32_t bar;
    if (!song.playing)
        return;
    bar = chain_bar();
    chain.bar_pos += n;
    if (!bar || chain.bar_pos < bar)
        return;
    chain.bar_pos -= bar;                             /* (a bar is longer than a block) */
    chain.carry = chain.bar_pos;
    srec_bar();
    if (chain.running) {
        uint32_t next = chain.row + 1u;
        if (chain.remaining > 1u) {
            chain.remaining--;
            return;
        }
        if (next >= chain.config.count) {
            if (!chain.loop) {                        /* the song's end */
                seq_stop();
                return;
            }
            next = 0;                                 /* (a quick chain: round again) */
        }
        if (sec_stage.ready && sec_stage.row == next) {
            chain_apply();                            /* (not staged yet: this row one bar more) */
            if (live.srec == 2u)
                srec_add(sec_stage.section);
        }
        return;
    }
    if (chain.armed_bar && sec_stage.ready && sec_stage.row == 0u) {   /* a quick chain from this bar */
        chain.armed_bar = 0;
        chain.running = 1;
        chain.rec = song.rec;
        chain_apply();
        if (live.srec == 2u)
            srec_add(live.cur);
        return;
    }
    if (live.req >= 0 && sec_stage.ready && sec_stage.row == SEC_LIVE) {   /* a live jump */
        live.req = -1;
        song.rec = 0;                                 /* (a take does not run on into another section) */
        sec_apply();
        if (live.srec == 2u)
            srec_add(live.cur);
    }
}
