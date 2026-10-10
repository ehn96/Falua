/* SPDX-License-Identifier: GPL-3.0-only
 * Falua core: the sound side of the JIANT firmware (engines, voices, FX, MIDI handling) built for a
 * desktop CPU, the way the firmware's own host tests build it (tests/hostsim.c), behind the C API of
 * jiant_core.h. The firmware's sources are compiled unchanged from third_party/jiant/firmware.
 * Based on tests/hostsim.c, Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hugelton Instruments,
 * and the JIANT fork by juanjiant-bit (GPL-3.0-only). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "jiant_core.h"

/* the firmware's build switches, as the host tests use them */
#ifndef FELUCCA_FM4
#define FELUCCA_FM4 0
#endif
#define FELUCCA_UAC 0
#define FELUCCA_OTA 0
#define FELUCCA_UART 0

#define __attribute__(x)
#define memset felucca_memset
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "felucca_tables.h"
#include "libc.c"
#undef memset
#undef memcpy
#undef memcmp
static struct { volatile uint32_t notes, buttons; } fm1_in;
#include "core.h"
#include "engines.c"
#include "params.c"
#include "mod.c"
#include "voice.c"
#include "slicer.c"
#include "fx.c"
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#include "usb.c"
#include "midi_uart.c"
#include "song_chain.c"
#include "seq.c"

/* ------------------------------------------------------------------ state --- */
static int32_t jc_buf[2 * CTL];      /* one internal block, Q15-ish stereo interleaved */
static uint32_t jc_pos = CTL;        /* frames of jc_buf already handed out */
static uint32_t jc_ms_acc;           /* samples toward the next millisecond (fm1_ms) */

/* a track to its power-on defaults: every common parameter at its default, the steps empty */
static void jc_track_defaults(track_t *t)
{
    uint32_t i;
    for (i = 0; i < P_E0; i++)
        t->p[i] = TP[i].def;
    for (i = 0; i < NSTEP; i++)
        t->step[i].time = ST_REST;
}

/* a factory preset into a track, as ui.c apply_preset_to / tests/hostsim.c host_preset_values do */
static void jc_apply_preset(track_t *t, uint32_t e, uint32_t pi)
{
    static const uint8_t FX_DEF[4] = {0, 24, 28, 36};
    const engine_t *eng = ENGINES[e];
    uint32_t i;
    t->eng_req = (uint8_t)e;
    if (!eng->npresets) {
        t->preset = 0;
        for (i = 0; i < 8u; i++)
            t->p[P_E0 + i] = eng->edit[i].def;
        return;
    }
    pi %= eng->npresets;
    {
        const preset_t *p = &eng->presets[pi];
        t->preset = (uint8_t)pi;
        for (i = 0; i < 8u; i++)
            t->p[P_E0 + i] = p->e[i];
        t->p[P_ATK] = p->env[0];
        t->p[P_DEC] = p->env[1];
        t->p[P_SUS] = p->env[2];
        t->p[P_REL] = p->env[3];
        t->p[P_ED_FLT] = p->fenv;
        t->p[P_VOICE] = p->mono ? V_LEGATO : V_POLY;
        for (i = 0; i < 4u; i++)
            t->p[P_DIST + i] = (int16_t)(p->fx[i] ? p->fx[i] - 1 : FX_DEF[i]);
    }
    fm6_track_loaded(t);
}

void jc_init(void)
{
    uint32_t i;
    memset(trk, 0, sizeof trk);
    for (i = 0; i < G_COUNT; i++)
        song.g[i] = GP[i].def;
    fm6_init();
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        jc_track_defaults(t);
        jc_apply_preset(t, 0, 0);            /* every part: ANALOG, its first preset */
        t->engine = t->eng_req;
    }
    song.sel = 0;
    song.master_q12 = 4096;
    jc_pos = CTL;
    jc_ms_acc = 0;
}

/* ------------------------------------------------------------------ audio --- */
void jc_render(float *left, float *right, int n)
{
    const float k = 1.0f / 32768.0f;
    int f;
    for (f = 0; f < n; f++) {
        if (jc_pos >= CTL) {
            mix_block(jc_buf, CTL);
            jc_pos = 0;
            jc_ms_acc += CTL;
            while (jc_ms_acc >= FS / 1000u) {   /* the firmware's millisecond clock (MIDI timeouts) */
                jc_ms_acc -= FS / 1000u;
                fm1_ms++;
            }
        }
        left[f] = (float)jc_buf[2u * jc_pos] * k;
        right[f] = (float)jc_buf[2u * jc_pos + 1u] * k;
        jc_pos++;
    }
}

/* ------------------------------------------------------------------- MIDI --- */
void jc_midi(uint8_t status, uint8_t d1, uint8_t d2)
{
    uint32_t cin = (status >> 4) & 0x0Fu;       /* USB-MIDI code index = the status's high nibble */
    if (status < 0x80u || status >= 0xF0u)
        return;
    midi_enqueue(cin | ((uint32_t)status << 8) | ((uint32_t)(d1 & 0x7F) << 16) | ((uint32_t)(d2 & 0x7F) << 24), 1u);
}

void jc_all_notes_off(void)
{
    uint32_t c, n;
    for (c = 0; c < NPART; c++)
        for (n = 0; n < 128u; n++)
            trk_note_off(&trk[c], n);
    for (c = 0; c < NPART; c++)
        midi_channel(c)->bend = 0;
}

/* ------------------------------------------------------- engines, presets --- */
int jc_engine_count(void) { return (int)NENG_SHOWN; }
int jc_engine_id(int index) { return (int)eng_vis((uint32_t)(index < 0 ? 0 : index)); }
const char *jc_engine_name(int e) { return e >= 0 && e < NENGINES ? ENGINES[e]->name : ""; }
int jc_preset_count(int e) { return e >= 0 && e < NENGINES ? (int)ENGINES[e]->npresets : 0; }
const char *jc_preset_name(int e, int p)
{
    if (p < 0 || p >= jc_preset_count(e))
        return "";
    return ENGINES[e]->presets[p].name;
}

void jc_set_engine(int part, int e)
{
    if (part < 0 || part >= NPART || !eng_ok((uint32_t)e))
        return;
    trk[part].eng_req = (uint8_t)e;          /* voice.c engine_block fades the part out, then switches */
}
int jc_get_engine(int part) { return part >= 0 && part < NPART ? trk[part].eng_req : 0; }

int jc_preset_values(int e, int pi, int *ids, int *values, int max)
{
    static track_t scratch;
    static const int IDS[] = {P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7,
                              P_ATK, P_DEC, P_SUS, P_REL, P_ED_FLT, P_VOICE, P_DIST, P_CHOR, P_DLY, P_REV};
    int i, n = (int)NELEM(IDS);
    if (!eng_ok((uint32_t)e) || n > max)
        return 0;
    memset(&scratch, 0, sizeof scratch);
    jc_track_defaults(&scratch);
    /* (fm6_track_loaded only reads the track's slot: harmless on the scratch track) */
    jc_apply_preset(&scratch, (uint32_t)e, (uint32_t)(pi < 0 ? 0 : pi));
    for (i = 0; i < n; i++) {
        ids[i] = IDS[i];
        values[i] = scratch.p[IDS[i]];
    }
    return n;
}

/* --------------------------------------------------------------- parameters --- */
static const struct { const char *sym; int id; } JC_TRACK_SYMS[] = {
    {"LEVEL", P_LEVEL}, {"ATK", P_ATK}, {"DEC", P_DEC}, {"SUS", P_SUS}, {"REL", P_REL},
    {"ENV_FLT", P_ED_FLT}, {"ENV_PIT", P_ED_PIT}, {"ENV_SHP", P_ED_SHP}, {"ENV_LOOP", P_ED_FX},
    {"LFO_RATE", P_LRATE}, {"LFO_WAVE", P_LWAVE}, {"LFO_PHASE", P_LPHASE}, {"LFO_FADE", P_LFADE},
    {"LFO_PIT", P_LD_PIT}, {"LFO_FLT", P_LD_FLT}, {"LFO_SHP", P_LD_SHP}, {"LFO_AMP", P_LD_AMP},
    {"DIST", P_DIST}, {"CHOR", P_CHOR}, {"DLY", P_DLY}, {"REV", P_REV},
    {"VOICE", P_VOICE}, {"GLIDE", P_GLIDE}, {"PAN", P_PAN}, {"DETUNE", P_DETUNE},
    {"DTYPE", P_DTYPE}, {"DTONE", P_DTONE}, {"FTYPE", P_FTYPE}, {"FCUT", P_FCUT}, {"FRES", P_FRES},
};

int jc_track_param_count(void) { return P_COUNT; }
int jc_edit_param_id(int k) { return P_E0 + (k & 7); }
int jc_track_param_id(const char *sym)
{
    size_t i;
    for (i = 0; i < NELEM(JC_TRACK_SYMS); i++)
        if (!strcmp(sym, JC_TRACK_SYMS[i].sym))
            return JC_TRACK_SYMS[i].id;
    return -1;
}

static int jc_fill(const param_desc_t *d, jc_param_info *o)
{
    if (!d || !o)
        return 0;
    o->label = d->label ? d->label : "";
    o->min = d->min;
    o->max = d->max;
    o->def = d->def;
    o->is_enum = d->names != 0;
    return 1;
}
int jc_track_param_info(int id, jc_param_info *o)
{
    return id >= 0 && id < P_E0 ? jc_fill(&TP[id], o) : 0;
}
int jc_edit_param_info(int e, int k, jc_param_info *o)
{
    return eng_ok((uint32_t)e) && k >= 0 && k < 8 ? jc_fill(&ENGINES[e]->edit[k], o) : 0;
}
static const char *jc_name(const param_desc_t *d, int v)
{
    int i;
    if (!d->names || v < d->min || v > d->max)
        return 0;
    for (i = 0; i <= v - d->min; i++)       /* (some name lists end early with a 0) */
        if (!d->names[i])
            return 0;
    return d->names[v - d->min];
}
const char *jc_track_value_name(int id, int v) { return id >= 0 && id < P_E0 ? jc_name(&TP[id], v) : 0; }
const char *jc_edit_value_name(int e, int k, int v)
{
    return eng_ok((uint32_t)e) && k >= 0 && k < 8 ? jc_name(&ENGINES[e]->edit[k], v) : 0;
}

static int jc_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
void jc_set_param(int part, int id, int v)
{
    const param_desc_t *d;
    if (part < 0 || part >= NPART || id < 0 || id >= P_COUNT)
        return;
    d = id >= P_E0 ? &ENGINES[eng_live(trk[part].eng_req)]->edit[id - P_E0] : &TP[id];
    trk[part].p[id] = (int16_t)jc_clamp(v, d->min, d->max);
}
int jc_get_param(int part, int id)
{
    return part >= 0 && part < NPART && id >= 0 && id < P_COUNT ? trk[part].p[id] : 0;
}

/* ------------------------------------------------------------------ globals --- */
static const struct { const char *sym; int id; } JC_GLOBAL_SYMS[] = {
    {"TUNE", G_TUNE}, {"DTIME", G_DTIME}, {"DFDBK", G_DFDBK}, {"DCOLOR", G_DCOLOR}, {"DMIX", G_DMIX},
    {"RSIZE", G_RSIZE}, {"RDAMP", G_RDAMP}, {"CRATE", G_CRATE}, {"CDEPTH", G_CDEPTH}, {"RTYPE", G_RTYPE},
    {"CLIP", G_CLIP}, {"WIDTH", G_WIDTH}, {"BPM", G_BPM},
};
int jc_global_id(const char *sym)
{
    size_t i;
    for (i = 0; i < NELEM(JC_GLOBAL_SYMS); i++)
        if (!strcmp(sym, JC_GLOBAL_SYMS[i].sym))
            return JC_GLOBAL_SYMS[i].id;
    return -1;
}
int jc_global_info(int g, jc_param_info *o) { return g >= 0 && g < G_COUNT ? jc_fill(&GP[g], o) : 0; }
void jc_set_global(int g, int v)
{
    if (g >= 0 && g < G_COUNT)
        song.g[g] = (int16_t)jc_clamp(v, GP[g].min, GP[g].max);
}
int jc_get_global(int g) { return g >= 0 && g < G_COUNT ? song.g[g] : 0; }

void jc_set_master(int q12) { song.master_q12 = (uint32_t)jc_clamp(q12, 0, 4096); }
