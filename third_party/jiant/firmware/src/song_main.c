/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * JIANT (FELUCCA TONIC): after SLOOP 2.4.1's song (isod89/sloop-fm1, GPL-3.0). */
/* The song, the main loop's part (song_chain.c: what it is). Included by project.c. */
static project_store_t song_keep __attribute__((section(".pool")));   /* the music as it was before PLAY */

/* ---- the songs (NSONG, four sections each: storage.c) */
static uint8_t song_idx_dirty;                    /* the index changed (rows, the current song), not in flash yet */
static uint32_t song_idx_t;                       /* .. since (fm1_ms): written once the rows rest a while */

static void song_rows_fix(chain_config_t *c)      /* a song's rows as chain_config takes them */
{
    if (!chain_valid(c))
        chain_defaults(c);
}
/* persist_boot, before the slots are filled: the index from flash (none or broken: song 1, its rows taken from its
 * section A at song_index_boot: Felucca's song, kept in the project) */
static void song_index_load(void)
{
    uint32_t k;
#if FELUCCA_FLASH
    int n = st_load(OBJ_SONGIDX, &song_idx, sizeof song_idx);
    if (song_idx.magic == SONG_MAGIC && song_idx.cur < NSONG &&
        ((song_idx.version == 2u && n == (int)sizeof song_idx) ||
         (song_idx.version == 1u && n == (int)__builtin_offsetof(song_index_t, scene)))) {
        if (song_idx.version == 1u) {               /* (before the scenes: none) */
            memset(song_idx.scene, 0, sizeof song_idx.scene);
            song_idx.version = 2;
        }
        for (k = 0; k < NSONG; k++)
            song_rows_fix(&song_idx.rows[k]);
        song_cur = song_idx.cur;
        return;
    }
#endif
    memset(&song_idx, 0, sizeof song_idx);
    song_idx.magic = SONG_MAGIC;
    song_idx.version = 2;
    for (k = 0; k < NSONG; k++)
        chain_defaults(&song_idx.rows[k]);
    song_cur = 0;
    song_idx.rsv[0] = 1;                          /* (no index yet: song_index_boot takes section A's rows) */
}
/* persist_boot, after the slots: the current song's rows */
static void song_index_boot(void)
{
    if (song_idx.rsv[0]) {
        song_idx.rsv[0] = 0;
        if (proj_import(&proj_scratch, &proj_slot[0], sizeof(project_store_t)) && chain_valid(&proj_scratch.chain))
            song_idx.rows[0] = proj_scratch.chain;
    }
    chain_config = song_idx.rows[song_cur];
}
/* the current song's name: its section A's, else SONG n (b: PROJ_NAME_LEN + 1 bytes) */
static void song_name(char *b)
{
    if (!project_name(0, b) || !b[0]) {
        str_cpy(b, "SONG ", PROJ_NAME_LEN + 1u);
        b[5] = (char)('1' + song_cur);
        b[6] = 0;
    }
}

/* section s (a project slot) into sec_stage for row `row` (SEC_LIVE: a live jump); 0 = staged */
static int song_stage(uint32_t s, uint32_t row)
{
    project_t *p = &proj_scratch;
    uint32_t k, i;
    if (!proj_import(p, &proj_slot[s & 3u], sizeof(project_store_t)))
        return 1;
    sec_stage.ready = 0;
    for (k = 0; k < NTRK; k++) {
        sec_trk_t *d = &sec_stage.t[k];
        const proj_trk_t *t = &p->t[k];
        uint32_t e = eng_live(t->engine);
        d->engine = (uint8_t)e;
        d->preset = (uint8_t)(ENGINES[e]->npresets ? (t->preset >= PROJ_DEF_KEEP ? 0u : t->preset) % ENGINES[e]->npresets
                                                   : 0u);
        for (i = 0; i < P_COUNT; i++)
            d->p[i] = (int16_t)param_fit(param_desc_of(e, i), t->p[i]);
        memcpy(d->step, t->step, sizeof d->step);
        proj_steps(d->step);
        fm6_unpack(p->fm6[k], d->fm6);
        fm6_sanitize(d->fm6);
    }
    sec_stage.motion = p->motion;
    memcpy(sec_stage.dx, p->dx, sizeof sec_stage.dx);
    sec_stage.dx_mute = (uint16_t)(p->dx_mute & DXM_ALL);
    memcpy(sec_stage.pfx_lane, p->pfx_lane, sizeof sec_stage.pfx_lane);
    memcpy(sec_stage.slg, &p->g[G_SLMODE], sizeof sec_stage.slg);   /* (0.6.3) the section's SLICER */
    sec_stage.pfx_ltgt = p->pfx_ltgt > 2u ? 0u : p->pfx_ltgt;
    sec_stage.row = (uint8_t)row;
    sec_stage.section = (uint8_t)(s & 3u);
    RING_PUBLISH();
    sec_stage.ready = 1;
    return 0;
}

/* the bars section s's loop takes: its longest pattern (LEN steps of its DIV) in bars, rounded up, 1..CHAIN_BARS */
static uint32_t section_bars(uint32_t s)
{
    project_t *p = &proj_scratch;
    uint32_t k, bars = 1, bar = 16u * div_samples(2);
    if (!bar || !proj_import(p, &proj_slot[s & 3u], sizeof(project_store_t)))
        return 1;
    for (k = 0; k < NTRK; k++) {
        uint32_t len = (uint32_t)clamp(p->t[k].p[P_SLEN], 1, NSTEP);
        uint32_t b = (len * div_samples((uint32_t)p->t[k].p[P_SDIV]) + bar - 1u) / bar;
        bars = b > bars ? b : bars;
    }
    return bars > CHAIN_BARS ? CHAIN_BARS : bars;
}

/* Main loop only: PLAY a song. 0 = armed, 1 = no rows, 2 = busy, 3 + s = section s not saved */
static uint32_t chain_prepare(void)
{
    uint32_t i;
    if (transport_busy())
        return 2;
    if (!chain_valid(&chain_config) || !chain_config.count)
        return 1;
    for (i = 0; i < chain_config.count; i++)
        if (!proj_import(&proj_scratch, &proj_slot[chain_config.row[i].slot], sizeof(project_store_t)))
            return 3u + chain_config.row[i].slot;
    project_capture(&proj_scratch);                   /* the music as it is: back when the song ends */
    chain.kept = (uint8_t)proj_pack(&song_keep, &proj_scratch);
    chain.config = chain_config;
    chain.loop = 0;
    chain.ended = 0;
    if (song_stage(chain.config.row[0].slot, 0))
        return 3u + chain.config.row[0].slot;
    RING_PUBLISH();
    chain.armed = 1;
    transport_req = 1;
    return 0;
}

/* section s as the music now, the song's rows kept (a section is a project: its own rows are not the song's) */
static int section_load(uint32_t s)
{
    chain_config_t keep = chain_config;
    uint8_t row = ui.song_row;
    if (!proj_import(&proj_scratch, &proj_slot[s & 3u], sizeof(project_store_t)) || project_restore_runtime(&proj_scratch))
        return 1;
    chain_config = keep;
    ui.song_row = row;
    proj_cur = (uint8_t)(s & 3u);
    live.cur = (int8_t)(s & 3u);
    return 0;
}

/* the song layer (ui_layer.c), main loop. Section s: in on the next bar (stopped: now). 0 done */
static int section_jump(uint32_t s)
{
    char b[8] = "A";
    s &= 3u;
    b[0] = (char)('A' + s);
    if (chain.running && !chain.loop) {
        ui_message("STOP THE SONG FIRST");
        return 1;
    }
    if (!project_used(s)) {
        ui_say(b, " IS EMPTY");
        return 1;
    }
    if (!song.playing && !transport_busy()) {
        if (section_load(s))
            return 1;
        ui_say("SECTION ", b);
        return 0;
    }
    fm1_irq_off();                                    /* (the ISR may be applying one) */
    chain.running = 0;                                /* (a single section ends a quick chain) */
    chain.armed_bar = 0;
    live.req = -1;
    fm1_irq_on();
    if (song_stage(s, SEC_LIVE))
        return 1;
    live.req = (int8_t)s;
    ui_say(b, " NEXT BAR");
    return 0;
}
/* the music now into section s: RAM at once (playing too), flash once stopped (song_poll) */
static void section_store(uint32_t s)
{
    char b[8] = "A";
    s &= 3u;
    b[0] = (char)('A' + s);
    project_capture(&proj_scratch);
    if (!proj_pack(&proj_wire, &proj_scratch)) {        /* (0.6: packed aside first, the section kept if it does not fit) */
        ui_message(proj_full ? "SECTION FULL" : "SAVE FORMAT ERROR");
        return;
    }
    proj_wire_gen++;
    memcpy(&proj_slot[s], &proj_wire, sizeof proj_slot[s]);
    live.cur = (int8_t)s;
    live.dirty |= (uint8_t)(1u << s);
    ui_say("STORED ", b);
}
/* RECALL: the section playing back as it was stored */
static void section_recall(void)
{
    if (live.cur < 0) {
        ui_message("NO SECTION YET");
        return;
    }
    if (!section_jump((uint32_t)live.cur))
        str_cpy(ui.msg2, "RECALL", sizeof ui.msg2);
}
/* a quick chain of n sections (n >= 2), each its longest pattern's bars, round and round */
static void quick_chain(const uint8_t *s, uint32_t n)
{
    uint32_t i;
    if (chain.running && !chain.loop) {
        ui_message("STOP THE SONG FIRST");
        return;
    }
    for (i = 0; i < n; i++)
        if (!project_used(s[i])) {
            char b[8] = "A";
            b[0] = (char)('A' + (s[i] & 3u));
            ui_say(b, " IS EMPTY");
            return;
        }
    fm1_irq_off();
    chain.running = 0;
    chain.armed_bar = 0;
    live.req = -1;
    fm1_irq_on();
    chain_defaults(&chain.config);
    chain.config.count = (uint8_t)n;
    for (i = 0; i < n; i++) {
        chain.config.row[i].slot = (uint8_t)(s[i] & 3u);
        chain.config.row[i].bars = (uint8_t)section_bars(s[i]);
    }
    chain.loop = 1;
    chain.kept = 0;
    chain.ended = 0;
    if (song_stage(chain.config.row[0].slot, 0))
        return;
    RING_PUBLISH();
    if (song.playing)
        chain.armed_bar = 1;                          /* on the next bar */
    else {
        chain.armed = 1;                              /* from the top, now */
        transport_req = 1;
    }
    ui_message("QUICK CHAIN");
}
/* SONG REC on / off */
static void song_rec_toggle(void)
{
    if (chain.running && !chain.loop) {
        ui_message("STOP THE SONG FIRST");
        return;
    }
    fm1_irq_off();
    if (live.srec)
        srec_stop();
    else
        live.srec = 1;
    fm1_irq_on();
    if (live.srec)
        ui_message(song.playing ? "SONG REC NEXT BAR" : "SONG REC: PLAY");
}

/* the current song's stored sections into flash: 0 done (or no flash) */
static int song_flush(void)
{
    uint32_t k;
#if FELUCCA_FLASH
    if (!flash_ok)
        return 0;
    for (k = 0; k < 4u; k++)
        if (((live.dirty >> k) & 1u) && st_save(proj_obj(k), &proj_slot[k], sizeof proj_slot[k]))
            return 1;
#else
    (void)k;
#endif
    live.dirty = 0;
    return 0;
}
/* (JIANT) the index changed outside the rows (a scene): into flash as the rows go (song_poll) */
static void song_idx_touch(void)
{
    song_idx_dirty = 1;
    song_idx_t = fm1_ms;
}
/* the index into flash (the songs' rows, the current song): 0 done (or no flash) */
static int song_index_save(void)
{
    song_idx.rows[song_cur] = chain_config;
    song_idx.cur = song_cur;
#if FELUCCA_FLASH
    if (flash_ok && st_save(OBJ_SONGIDX, &song_idx, sizeof song_idx))
        return 1;
#endif
    song_idx_dirty = 0;
    return 0;
}
/* song n (0..NSONG - 1) in, stopped only: the current one kept first, then n's sections and rows; its section A
 * becomes the music (an empty song: the music stays, a new song to store sections into). 0 done */
static int song_select(uint32_t n)
{
    char nm[PROJ_NAME_LEN + 1u];
    uint32_t k;
    if (n >= NSONG || n == song_cur)
        return 1;
    if (transport_busy()) {
        ui_message("STOP TO CHANGE SONG");
        return 1;
    }
    if (song_flush()) {
        ui_message("SAVE ERROR");
        return 1;
    }
    song_idx.rows[song_cur] = chain_config;           /* (the rows of the song left) */
    song_cur = (uint8_t)n;
    chain_config = song_idx.rows[n];
    if (song_index_save()) {                          /* (failed: written again by song_poll) */
        song_idx_dirty = 1;
        song_idx_t = fm1_ms;
    }
    for (k = 0; k < 4u; k++) {
        memset(proj_slot[k].raw, 0, 4);
#if FELUCCA_FLASH
        if (flash_ok)
            proj_fetch(k);
#endif
    }
    ui.song_row = 0;
    live.cur = -1;
    live.req = -1;
    if (project_used(0))
        (void)section_load(0);
    else
        proj_cur = PROJ_NO_SLOT;
    song_name(nm);
    ui_say(nm, "");
    return 0;
}

/* Main loop, each pass: the next section staged; the music back after the song; SONG REC's rows; stored sections
 * into flash once stopped */
static void song_poll(void)
{
    uint32_t k;
    if (chain.applied) {
        chain.applied = 0;
        for (k = 0; k < NTRK; k++)
            fm6_adopt(k);                             /* (an FM6 track's SLOT: F n or OWN) */
        sync_reload = 1;
        ui.force = 1;
    }
    if (chain.running && !sec_stage.ready) {
        uint32_t next = chain.row + 1u;
        if (next >= chain.config.count && chain.loop)
            next = 0;
        if (next < chain.config.count)
            (void)song_stage(chain.config.row[next].slot, next);
    }
    if (live.rec_done) {
        uint32_t n = live.rec_done;
        live.rec_done = 0;
        if (n == 0xFFu) {
            ui_message("NO SONG");
        } else {
            char b[8];
            chain_defaults(&chain_config);
            chain_config.count = (uint8_t)n;
            memset(song_idx.scene[song_cur], 0, sizeof song_idx.scene[song_cur]);
            for (k = 0; k < n; k++) {
                chain_config.row[k] = live.rec[k];
                song_idx.scene[song_cur][k] = live.rec_scene[k];   /* (each row's scene, as it began) */
            }
            song_idx_touch();
            ui.song_row = 0;
            fmt_int(b, (int32_t)n);
            ui_say("SONG ROWS ", b);
        }
    }
    if (chain.ended && !song.playing) {
        uint8_t cur = proj_cur;
        char name[PROJ_NAME_LEN + 1u];
        chain.ended = 0;
        str_cpy(name, proj_name, sizeof name);
        if (chain.kept && proj_import(&proj_scratch, &song_keep, sizeof song_keep) &&
            !project_restore_runtime(&proj_scratch)) {
            proj_cur = cur;                           /* (the music before the song: its slot and name) */
            str_cpy(proj_name, name, PROJ_NAME_LEN + 1u);
            ui_message("SONG END");
        }
    }
    if (memcmp(&chain_config, &song_idx.rows[song_cur], sizeof chain_config)) {   /* the rows changed */
        song_idx.rows[song_cur] = chain_config;
        song_idx_dirty = 1;
        song_idx_t = fm1_ms;
    }
#if FELUCCA_FLASH
    if (song_idx_dirty && flash_ok && !transport_busy() && (uint32_t)(fm1_ms - song_idx_t) >= 2000u &&
        song_index_save())
        song_idx_t = fm1_ms;                          /* (failed: tried again later) */
    if (live.dirty && flash_ok && !transport_busy())
        for (k = 0; k < 4u; k++)
            if (((live.dirty >> k) & 1u) && !st_save(proj_obj(k), &proj_slot[k], sizeof proj_slot[k]))
                live.dirty &= (uint8_t)~(1u << k);    /* (a failed write stays dirty: tried again) */
#else
    live.dirty = 0;
    song_idx_dirty = 0;
#endif
}
