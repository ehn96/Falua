/* SPDX-License-Identifier: GPL-3.0-only
 * Falua: a C API around the sound engines of the JIANT firmware for the M-VAVE FM-1
 * (a fork of Felucca by Leo Kuroshita, Hugelton Instruments). See ../README.md and ../NOTICE.md.
 *
 * The firmware keeps all its state in static globals, so there is ONE synth per process.
 * Every function here must be called from the same thread (the audio thread).
 * The core always runs at JC_SAMPLE_RATE; the plugin resamples to the host rate. */
#ifndef JIANT_CORE_H
#define JIANT_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JC_SAMPLE_RATE 44100
#define JC_NUM_PARTS 6        /* the firmware's synth parts (MIDI channels 1..6) */
#define JC_EDIT_PARAMS 8      /* each engine's 8 sound knobs (P_E0..P_E7) */

typedef struct {
    const char *label;        /* short name as printed on the FM-1 screen, e.g. "CUT" */
    int min, max, def;
    int is_enum;              /* 1: value_name() gives the names */
} jc_param_info;

/* life cycle -------------------------------------------------------------------- */
void jc_init(void);                       /* power-on state; safe to call again (full reset) */

/* audio ------------------------------------------------------------------------- */
/* n frames of stereo float at JC_SAMPLE_RATE, any n; events sent before the call land
 * at the start of the next internal block (32 frames, about 0.7 ms) */
void jc_render(float *left, float *right, int n);

/* MIDI: a channel voice message (note on/off, CC, pitch bend, channel pressure).
 * Channel 1..6 play parts 1..6 (status low nibble 0..5). */
void jc_midi(uint8_t status, uint8_t d1, uint8_t d2);
void jc_all_notes_off(void);

/* engines and presets ----------------------------------------------------------- */
int jc_engine_count(void);                /* selectable engines, in the FM-1's display order */
int jc_engine_id(int index);              /* the firmware's engine number for a display index */
const char *jc_engine_name(int engine_id);
int jc_preset_count(int engine_id);
const char *jc_preset_name(int engine_id, int preset);

/* switches a part's engine with the firmware's own fade (no clicks) */
void jc_set_engine(int part, int engine_id);
int jc_get_engine(int part);

/* copies a factory preset's values into out[] (indexed by jc track param id), so the
 * host side can push them through its own parameters. Returns the number of values (0 if none). */
int jc_preset_values(int engine_id, int preset, int *ids, int *values, int max);

/* parameters -------------------------------------------------------------------- */
/* track parameter ids are the firmware's P_* numbers; the 8 engine knobs are
 * jc_edit_param_id(0..7) */
int jc_track_param_count(void);
int jc_edit_param_id(int k);
int jc_track_param_id(const char *symbol);          /* "LEVEL", "ATK", "CHOR", ... (see jiant_core.c) */
int jc_track_param_info(int param_id, jc_param_info *out);
int jc_edit_param_info(int engine_id, int k, jc_param_info *out);
const char *jc_track_value_name(int param_id, int value);       /* 0 if not an enum */
const char *jc_edit_value_name(int engine_id, int k, int value);
void jc_set_param(int part, int param_id, int value);           /* clamped to the range */
int jc_get_param(int part, int param_id);

/* globals (reverb size, delay time, tuning, ...): the firmware's G_* numbers */
int jc_global_id(const char *symbol);               /* "RSIZE", "DTIME", "TUNE", ... */
int jc_global_info(int g, jc_param_info *out);
void jc_set_global(int g, int value);
int jc_get_global(int g);

/* master volume, 0..4096 (Q12, 4096 = unity) */
void jc_set_master(int q12);

#ifdef __cplusplus
}
#endif
#endif
