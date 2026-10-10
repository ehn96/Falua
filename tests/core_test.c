/* SPDX-License-Identifier: GPL-3.0-only
 * Renders short phrases through the public API only (jc_midi + jc_render) to WAV files, and checks
 * that every engine and preset makes sound, stays finite and does not clip hard.
 *   core_test OUTDIR */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "jiant_core.h"

static void wav(const char *path, const float *l, const float *r, int n)
{
    FILE *f = fopen(path, "wb");
    unsigned v; unsigned short s;
    int i;
    if (!f) return;
    fwrite("RIFF", 1, 4, f); v = 36 + n * 4; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    s = 1; fwrite(&s, 2, 1, f); s = 2; fwrite(&s, 2, 1, f);
    v = JC_SAMPLE_RATE; fwrite(&v, 4, 1, f); v = JC_SAMPLE_RATE * 4; fwrite(&v, 4, 1, f);
    s = 4; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); v = n * 4; fwrite(&v, 4, 1, f);
    for (i = 0; i < n; i++) {
        float a = l[i], b = r[i];
        short o[2];
        a = a > 1 ? 1 : a < -1 ? -1 : a; b = b > 1 ? 1 : b < -1 ? -1 : b;
        o[0] = (short)(a * 32767); o[1] = (short)(b * 32767);
        fwrite(o, 2, 2, f);
    }
    fclose(f);
}

/* a 3 s phrase: C3 bass note, then a C minor chord, then a short run; render in odd block sizes */
static int phrase(float *L, float *R, int drums)
{
    static const int RUN[] = {60, 63, 67, 70, 72, 70, 67, 63};
    const int sr = JC_SAMPLE_RATE, total = 3 * sr;
    int pos = 0, ev = 0;
    while (pos < total) {
        int t = pos * 1000 / sr, n = 37 + (pos / 37) % 200;     /* irregular host block sizes */
        if (n > total - pos) n = total - pos;
        if (drums) {
            if (ev < 12 && t >= ev * 250) {
                jc_midi(0x90, ev % 4 == 0 ? 36 : ev % 4 == 2 ? 38 : 42, 110);
                ev++;
            }
        } else {
            if (ev == 0 && t >= 0) { jc_midi(0x90, 48, 100); ev++; }
            if (ev == 1 && t >= 500) { jc_midi(0x80, 48, 0); ev++; }
            if (ev == 2 && t >= 600) { jc_midi(0x90, 60, 90); jc_midi(0x90, 63, 90); jc_midi(0x90, 67, 90); ev++; }
            if (ev == 3 && t >= 1400) { jc_midi(0x80, 60, 0); jc_midi(0x80, 63, 0); jc_midi(0x80, 67, 0); ev++; }
            if (ev >= 4 && ev < 12 && t >= 1500 + (ev - 4) * 125) {
                if (ev > 4) jc_midi(0x80, RUN[ev - 5], 0);
                jc_midi(0x90, RUN[ev - 4], 100);
                ev++;
            }
            if (ev == 12 && t >= 2500) { jc_midi(0x80, RUN[7], 0); ev++; }
        }
        jc_render(L + pos, R + pos, n);
        pos += n;
    }
    return total;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    static float L[3 * JC_SAMPLE_RATE], R[3 * JC_SAMPLE_RATE];
    int i, fails = 0, ne = jc_engine_count();
    char path[512];
    jc_init();
    printf("%d engines\n", ne);
    for (i = 0; i < ne; i++) {
        int e = jc_engine_id(i), np = jc_preset_count(e), p;
        printf("  %-7s %2d presets:", jc_engine_name(e), np);
        for (p = 0; p < np; p++) {
            int ids[32], vals[32], k, nv, n, s;
            double peak = 0, rms = 0;
            jc_init();
            jc_set_engine(0, e);
            nv = jc_preset_values(e, p, ids, vals, 32);
            for (k = 0; k < nv; k++)
                jc_set_param(0, ids[k], vals[k]);
            n = phrase(L, R, !strcmp(jc_engine_name(e), "DRUM"));
            for (s = 0; s < n; s++) {
                double a = fabs(L[s]) > fabs(R[s]) ? fabs(L[s]) : fabs(R[s]);
                if (!isfinite(L[s]) || !isfinite(R[s])) { a = 99; }
                if (a > peak) peak = a;
                rms += L[s] * L[s];
            }
            rms = sqrt(rms / n);
            if (peak < 0.01 || peak > 1.5) {
                printf("\n    FAIL %s / %s: peak %.3f", jc_engine_name(e), jc_preset_name(e, p), peak);
                fails++;
            }
            if (p == 0 || !strcmp(jc_engine_name(e), "ANALOG")) {
                snprintf(path, sizeof path, "%s/%s_%02d_%s.wav", dir, jc_engine_name(e), p, jc_preset_name(e, p));
                for (k = (int)strlen(dir) + 1; path[k]; k++) if (path[k] == ' ' || path[k] == '/') path[k] = '_';
                wav(path, L, R, n);
            }
            (void)rms;
        }
        printf(" ok\n");
    }
    {   /* every ANALOG knob: label, range, names */
        jc_param_info pi;
        for (i = 0; i < 8; i++) {
            jc_edit_param_info(0, i, &pi);
            printf("  ANALOG E%d %-5s %4d..%-4d def %4d%s\n", i, pi.label, pi.min, pi.max, pi.def, pi.is_enum ? " (enum)" : "");
        }
    }
    printf(fails ? "%d FAILED\n" : "all engines and presets sound\n", fails);
    return fails != 0;
}
