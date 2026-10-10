/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * JIANT (FELUCCA TONIC): the song as SLOOP 2.4.1 plays it (isod89/sloop-fm1, GPL-3.0: arranger.h, arranger_scene.c,
 * its live sections, quick chain and SONG REC), rebuilt on Felucca's sequencer. */
/* The song (docs/TONIC-SONG-PLAN.md). Four sections A..D, the four saved projects: a section is the whole project, the
 * four tracks' sounds (engine, parameters, FM6 patch), the DRUM-X kit, their steps and the motion; the tempo and the
 * globals are the song's (not the sections'). Sections go in on the bar (a 4/4 bar of 16 1/16 steps at the tempo), every track from
 * its step 0, the remainder of the block carried.
 *   the song    up to CHAIN_ROWS rows, each a section and the bars it plays (1..CHAIN_BARS): PLAY on SONG (or anywhere
 *               in SONG mode) plays it from the top and keeps the music as it was (song_keep), which comes back when
 *               the song ends (its last row) or stops
 *   live        (the song layer, SAVE held: ui_layer.c) a section asked for while the loop plays goes in on the next
 *               bar (stopped: at once); the music can be stored into a section (RAM at once, flash once stopped) and
 *               the section playing brought back as it was stored (RECALL); a quick chain (two or more sections
 *               tapped in one hold) plays them in turn, each its longest pattern's bars, round and round; SONG REC
 *               writes the sections played and their bars into the song's rows
 *   main loop   decodes the section that goes in next into sec_stage (song_main.c); the ISR only swaps it in
 *               (song_play.c). A row whose section is not staged in time plays its section another bar
 * This file: the state and the rows; song_play.c (included at the end of seq.c): the ISR's part; song_main.c
 * (included by project.c): the main loop's. */
static chain_config_t chain_config;           /* the current song's rows */
#define NSONG 8u                              /* songs: four sections each (storage.c: where they are kept) */
#define SONG_MAGIC 0x474E4F53u                /* "SONG" */
/* (JIANT) a row's scene: when the row goes in (a song, not a quick chain) the tracks' mutes, DRUM's group mutes, the
 * macros and the punch-in MIDI effects become what was stored; they stay through the rows after it that have none.
 * Stored by KNOB 4 on SONG (the state now) or by SONG REC (the state as each row began) */
typedef struct {
    uint8_t on;                               /* bit 0 = the row has one; (JIANT 0.6) bits 1, 2: T5, T6 muted */
    uint8_t mute;                             /* bits 0..3 T1..T4 muted, 4..7 DRUM's groups (DXG_*) */
    uint16_t pfx;                             /* the punch-in MIDI effects held: bit k = PF_OCTD + k */
    uint8_t mac[4];                           /* M1..M4 (mod.c macro_v) */
} scene_t;
typedef struct {                              /* the song index (flash: OBJ_SONGIDX) */
    uint32_t magic;
    uint8_t version, cur, rsv[2];
    chain_config_t rows[NSONG];               /* each song's rows (the current one's: chain_config, kept here) */
    scene_t scene[NSONG][CHAIN_ROWS];         /* (version 2, JIANT) their scenes; a version 1 index: none */
} song_index_t;
static void song_idx_touch(void);             /* song_main.c: the index changed, into flash soon */
/* the state now as a scene (main loop: KNOB 4 on SONG; ISR: SONG REC) */
static void scene_capture(scene_t *s)
{
    uint32_t i, m = dx_mute & DXG_ALL, on = 1u;
    m <<= 4;
    for (i = 0; i < NTRK; i++)
        if (trk[i].p[P_MUTE]) {
            if (i < 4u) m |= 1u << i;
            else on |= 1u << (i - 3u);
        }
    s->on = (uint8_t)on;
    s->mute = (uint8_t)m;
    s->pfx = (uint16_t)((((perf_held | perf_latched) & PF_MIDI) >> PF_OCTD) | (scene_pfx >> PF_OCTD));
    memcpy(s->mac, macro_v, sizeof s->mac);
}
static song_index_t song_idx;
static uint8_t song_cur;                      /* the song whose sections proj_slot holds (0: the four project slots) */
#define SEC_LIVE 0xFEu                /* sec_stage.row: a live jump (the song layer), not a song row */
typedef struct {                      /* a track of a section, as the ISR puts it in */
    int16_t p[P_COUNT];
    uint8_t engine, preset;
    step_t step[NSTEP];
    uint8_t fm6[FP_SIZE + 1u];        /* (unpacked: eng_fm6.c fm6_set_patch) */
} sec_trk_t;
static struct {
    sec_trk_t t[NTRK];
    motion_store_t motion;
    dx_lane_t dx[8];                  /* the section's DRUM-X kit */
    uint16_t dx_mute;                 /* .. its mutes (groups, sounds) */
    uint8_t pfx_lane[32], pfx_ltgt;   /* .. its punch-in lane (pfx.c) */
    int16_t slg[G_SL_N];              /* .. (0.6.3) its SLICER (the globals G_SLMODE ..) */
    volatile uint8_t ready;           /* main: staged; ISR: taken (0) */
    uint8_t row, section;             /* the song row it is for (SEC_LIVE: a live jump), the section (0..3) */
} sec_stage __attribute__((section(".pool")));
static struct {
    chain_config_t config;
    volatile uint8_t armed, running, row, remaining;   /* remaining: the bars of the row still to play */
    volatile uint8_t ended;           /* ISR: the song ended or stopped (song_poll brings the music back) */
    volatile uint8_t applied;         /* ISR: a section went in (song_poll: the FM6 SLOTs, the screen) */
    volatile uint8_t armed_bar;       /* a quick chain asked for while playing: it starts on the next bar */
    uint8_t loop;                     /* the quick chain: round and round (a song stops at its end) */
    uint8_t scn;                      /* the row whose scene is in effect + 1 (0: none since the song began) */
    uint8_t rec, kept;
    uint32_t carry;                   /* samples past the bar line at the end of the block: the first step's */
    uint32_t bar_pos;                 /* samples into the bar (while playing) */
} chain;
static struct {
    volatile int8_t req;              /* a section asked for (staged), in on the next bar; -1 none */
    volatile int8_t cur;              /* the section playing: jumped to, loaded or stored; -1 none */
    volatile uint8_t srec;            /* SONG REC: 0 off, 1 armed (from the next bar), 2 recording */
    volatile uint8_t nrec;            /* .. its rows so far (the last one still growing) */
    volatile uint8_t rec_done;        /* .. ISR: written, n rows (0xFF: nothing played); song_poll takes them */
    chain_row_t rec[CHAIN_ROWS];
    scene_t rec_scene[CHAIN_ROWS];    /* .. the scene as each row began */
    uint8_t dirty;                    /* sections stored in RAM, not in flash yet (bit s) */
    uint8_t mode;                     /* 1 SONG: PLAY plays the song; 0 LOOP */
} live = {.req = -1, .cur = -1};

static void chain_start(void);                   /* song_play.c (end of seq.c) */
static void chain_stop(void);
static void chain_tick(uint32_t n);
static uint32_t step_samples(const track_t *t, uint32_t period, uint32_t idx);   /* seq.c (motion.c uses it) */

static void chain_defaults(chain_config_t *c)
{
    uint32_t i;
    memset(c, 0, sizeof *c);
    for (i = 0; i < CHAIN_ROWS; i++)
        c->row[i].bars = 1;
}
static int chain_valid(const chain_config_t *c)
{
    uint32_t i;
    if (c->count > CHAIN_ROWS)
        return 0;
    for (i = 0; i < c->count; i++)
        if (c->row[i].slot >= 4u || !c->row[i].bars || c->row[i].bars > CHAIN_BARS)
            return 0;
    return 1;
}
static const step_t *seq_steps(const track_t *t) { return t->step; }   /* (a section's steps are the track's) */
