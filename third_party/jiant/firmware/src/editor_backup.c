/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Complete musical archive: no raw addresses are accepted. Small objects are staged
 * in main-loop RAM and fully validated before the existing A/B commit path writes.
 * Ids 32..34 (Felucca's user sample slots) are gone with the slots (JIANT has none).
 * Id 8 (the FM6 patch bank of 1.0..1.0.2) is listed empty since 1.0.3; a PUT of it from an older archive moves its
 * patches into the user presets restored before it (ids 6, 7), as the first boot after the update does (up_fm6.c).
 * Id 9 is the user presets' FM6 patches (up_fm6.c), appended in 1.0.3.
 * (JIANT) Id 10 is the song index (song_chain.c: every song's rows and scenes), ids 40..71 every song's sections
 * (40 + 4 x song + section: the current song's from RAM, the others read from flash as they are asked for, into the
 * storage driver's own buffer: no RAM of their own). Ids 2..5 stay the current song's sections, as before.
 */
#define ED_BK_SONG 40u                /* (JIANT) the first song section's id */
static const uint8_t ED_BK_IDS[11 + NSONG * 4u] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71};
_Static_assert(NSONG == 8u && sizeof(song_index_t) <= sizeof proj_wire_u.raw, "the songs' ids, the index staged");
static int proj_store_len(uint32_t len);
/* the flash object of song s's section k (storage.c: song 1 the four project slots) */
static uint32_t ed_bk_song_obj(uint32_t s, uint32_t k) { return s ? OBJ_SONG1 + (s - 1u) * 4u + k : OBJ_PROJECT0 + k; }
#define ED_BK_N ((uint32_t)sizeof ED_BK_IDS)
#define ED_BK_MAX ((uint32_t)sizeof proj_wire_u.raw)
#define ED_BK_RAW (proj_wire_u.raw)  /* reuse the existing serialized main-loop scratch */
static uint8_t ed_bk_buf[256];       /* a PUT's chunk, unpacked */
_Static_assert(sizeof proj_wire_u.raw >= sizeof(upf_t) && sizeof proj_wire_u.raw >= sizeof(fm6_bank_t), "backup staging");
static persist_t ed_bk_settings;
static uint8_t ed_bk_valid, ed_bk_put, ed_bk_id, ed_bk_gen;
static uint32_t ed_bk_len, ed_bk_crc, ed_bk_pos, ed_bk_ms, ed_bk_usb;
static void ed_bk_u32(uint32_t n) { for (uint32_t i = 0; i < 5u; i++) ed_b((n >> (i * 7u)) & 127u); }
static uint32_t ed_bk_r32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 7 | (uint32_t)p[2] << 14 | (uint32_t)p[3] << 21 | (uint32_t)p[4] << 28;
}
static void ed_bk_pack(const uint8_t *p, uint32_t n)
{
    while (n) {
        uint32_t k = n > 7u ? 7u : n, mask = 0;
        for (uint32_t i = 0; i < k; i++) mask |= (uint32_t)(p[i] >> 7) << i;
        ed_b(mask);
        for (uint32_t i = 0; i < k; i++) ed_b(p[i]);
        p += k; n -= k;
    }
}
static const uint8_t *ed_bk_object(uint32_t id, uint32_t *len)
{
    *len = 0;
    if (id == 0u) { *len = sizeof(project_store_t); return ED_BK_RAW; }
    if (id == 1u) { *len = sizeof ed_bk_settings; return (const uint8_t *)&ed_bk_settings; }
    if (id >= 2u && id <= 5u) {
        if (project_used(id - 2u)) *len = sizeof proj_slot[0];
        return (const uint8_t *)&proj_slot[id - 2u];
    }
    if (id == 6u || id == 7u) {
        if (up_bank[id - 6u].magic == UP_BANK_MAGIC) *len = sizeof up_bank[0];
        return (const uint8_t *)&up_bank[id - 6u];
    }
    if (id == 8u)                                   /* the retired FM6 patch bank: always empty */
        return ED_BK_RAW;
    if (id == 9u) {                                 /* the user presets' FM6 patches */
        if (upf_valid(&upf)) *len = sizeof upf;
        return (const uint8_t *)&upf;
    }
    if (id == 10u) {                                /* (JIANT) the song index, the current rows in it */
        song_idx.rows[song_cur] = chain_config;
        *len = sizeof song_idx;
        return (const uint8_t *)&song_idx;
    }
    if (id >= ED_BK_SONG && id < ED_BK_SONG + NSONG * 4u) {   /* (JIANT) a song's section */
        uint32_t s = (id - ED_BK_SONG) / 4u, k = (id - ED_BK_SONG) % 4u;
        if (s == song_cur) {
            if (project_used(k)) *len = sizeof proj_slot[0];
            return (const uint8_t *)&proj_slot[k];
        }
#if FELUCCA_FLASH
        {
            st_hdr_t h;
            if (st_current(ed_bk_song_obj(s, k), &h) >= 0 && proj_store_len(h.len))
                *len = h.len;                       /* (its body in st_buf until the next flash access) */
        }
        return st_buf;
#else
        return ED_BK_RAW;                           /* (no flash: only the current song's) */
#endif
    }
    return 0;
}
static uint32_t ed_bk_capture(void)
{
    if (ed_flash_stop()) return 3;
    project_capture(&proj_scratch);
    if (!proj_pack((project_store_t *)ED_BK_RAW, &proj_scratch)) return 2;
    ed_bk_gen = ++proj_wire_gen;
#if FELUCCA_FLASH
    ed_bk_settings = persist_saved;                 /* fields absent from this build survive */
#else
    memset(&ed_bk_settings, 0, sizeof ed_bk_settings);
#endif
    settings_export(&ed_bk_settings);
    ed_bk_put = 0; ed_bk_valid = 1; ed_bk_usb = usb.resets;
    return 0;
}
static int ed_bk_panel_valid(const panel_t *p)
{
    uint32_t b = 0, e = 0;
    if (p->magic != PANEL_MAGIC) return 0;
    for (uint32_t i = 0; i < NB; i++) {
        if (p->btn[i] >= NB || (b & (1u << p->btn[i]))) return 0;
        b |= 1u << p->btn[i];
    }
    for (uint32_t i = 0; i < NE; i++) {
        if (p->enc[i] >= NE || (e & (1u << p->enc[i])) || (p->dir[i] != 1 && p->dir[i] != -1)) return 0;
        e |= 1u << p->enc[i];
    }
    return 1;
}
static uint32_t ed_bk_commit(void)
{
    uint8_t *raw = ED_BK_RAW;
    uint32_t obj;
    if (ed_bk_pos != ed_bk_len || st_crc32(raw, ed_bk_len) != ed_bk_crc) return 2;
    if (ed_bk_id == 0u || (ed_bk_id >= 2u && ed_bk_id <= 5u)) {
        if (ed_bk_len && !proj_import(&proj_scratch, raw, (int)ed_bk_len)) return 2;
        if (ed_bk_len) {                            /* an older format becomes FUN9 inside its ranges */
            proj_bound(&proj_scratch);
            if (!proj_pack((project_store_t *)raw, &proj_scratch)) return 2;
            ed_bk_len = sizeof(project_store_t);
        }
        if (ed_bk_id == 0u) {
            if (!ed_bk_len) return 2;
            return project_restore_runtime(&proj_scratch) ? 2u : 0u;
        }
        obj = proj_obj(ed_bk_id - 2u);             /* (the current song's sections) */
    } else if (ed_bk_id == 1u) {
        const persist_t *p = (const persist_t *)raw;
        if (ed_bk_len != sizeof *p || p->magic != PERSIST_MAGIC || !palette_stored_ok(p->palette) ||
            p->lowcut > 2u || !leds_stored_ok(p->zoom) || !hold_stored_ok(p->bold) || p->favorites.filter > 1u || !ed_bk_panel_valid(&p->panel)) return 2;
        obj = OBJ_SETTINGS;
    } else if (ed_bk_id == 6u || ed_bk_id == 7u) {
        const up_bank_t *p = (const up_bank_t *)raw;
        if (ed_bk_len && (ed_bk_len != sizeof *p || p->magic != UP_BANK_MAGIC ||
            p->rsize != sizeof(up_rec_t) || p->nslot != UP_PER_BANK)) return 2;
        /* Unknown record versions remain inert bytes, preserving future/older bank data. */
        obj = OBJ_UPRESET0 + ed_bk_id - 6u;
    } else if (ed_bk_id == 8u) {                    /* an older archive's FM6 bank: into the restored user presets */
        if (!ed_bk_len) return 0;
        if (ed_bk_len != sizeof(fm6_bank_t) || !fm6_bank_valid((const fm6_bank_t *)raw)) return 2;
        upf_migrate((const fm6_bank_t *)raw);
        sync_reload = 1; ui.force = 1;
        return upf_save() == 2 ? 4u : 0u;
    } else if (ed_bk_id == 9u) {                    /* the user presets' FM6 patches (0: none) */
        const upf_t *u = (const upf_t *)raw;
        if (ed_bk_len && (ed_bk_len != sizeof *u || !upf_valid(u) || (u->used >> (UP_SLOTS - 1u) >> 1))) return 2;
        if (ed_bk_len) memcpy(&upf, raw, sizeof upf); else upf_empty();
        sync_reload = 1; ui.force = 1;
        return upf_save() == 2 ? 4u : 0u;           /* (a failed write keeps the restored RAM copy) */
    } else if (ed_bk_id == 10u) {                   /* (JIANT) the song index: rows and scenes; the current song stays */
        const song_index_t *p = (const song_index_t *)raw;
        uint32_t k;
        if (ed_bk_len != sizeof *p || p->magic != SONG_MAGIC || p->version != 2u) return 2;
        for (k = 0; k < NSONG; k++)
            if (!chain_valid(&p->rows[k])) return 2;
        obj = OBJ_SONGIDX;
    } else if (ed_bk_id >= ED_BK_SONG && ed_bk_id < ED_BK_SONG + NSONG * 4u) {   /* (JIANT) a song's section */
        uint32_t s = (ed_bk_id - ED_BK_SONG) / 4u, k = (ed_bk_id - ED_BK_SONG) % 4u;
        if (ed_bk_len) {                            /* an older format becomes today's */
            if (!proj_import(&proj_scratch, raw, (int)ed_bk_len)) return 2;
            proj_bound(&proj_scratch);
            if (!proj_pack((project_store_t *)raw, &proj_scratch)) return 2;
            ed_bk_len = sizeof(project_store_t);
        }
        obj = ed_bk_song_obj(s, k);
    } else return 1;
#if FELUCCA_FLASH
    if (!flash_ok || st_save(obj, raw, ed_bk_len)) return 4;
#else
    (void)obj;
#endif
    if (ed_bk_id == 10u) {                          /* (JIANT) into RAM: the rows now, the song playing kept */
        uint8_t cur = song_cur;
        memcpy(&song_idx, raw, sizeof song_idx);
        song_idx.cur = cur;
        chain_config = song_idx.rows[cur];
        ui.song_row = 0;
    } else if (ed_bk_id >= ED_BK_SONG) {            /* (JIANT) a song's section: the current song's into RAM too */
        uint32_t s = (ed_bk_id - ED_BK_SONG) / 4u, k = (ed_bk_id - ED_BK_SONG) % 4u;
        if (s == song_cur) {
            memset(&proj_slot[k], 0, sizeof proj_slot[0]);
            if (proj_cur == k)
                proj_cur = PROJ_NO_SLOT;
            if (ed_bk_len) memcpy(&proj_slot[k], raw, ed_bk_len);
        }
    } else if (ed_bk_id >= 2u && ed_bk_id <= 5u) {
        memset(&proj_slot[ed_bk_id - 2u], 0, sizeof proj_slot[0]);
        if (proj_cur == ed_bk_id - 2u)
            proj_cur = PROJ_NO_SLOT;                     /* (another project there now: the music keeps its name) */
        if (ed_bk_len) memcpy(&proj_slot[ed_bk_id - 2u], raw, ed_bk_len);
    } else if (ed_bk_id == 1u) {
        memcpy(&ed_bk_settings, raw, sizeof ed_bk_settings);
        settings_import(&ed_bk_settings, sizeof ed_bk_settings);
        panel_init(); settings_init(); palette_set(settings.palette);
#if FELUCCA_FLASH
        persist_saved = ed_bk_settings; persist_pending = 0;
#endif
    } else {
        uint32_t b = ed_bk_id - 6u;
        memset(&up_bank[b], 0, sizeof up_bank[b]);
        if (ed_bk_len) memcpy(&up_bank[b], raw, ed_bk_len);
        up_bank_check(b, (int)ed_bk_len); up_gen++;
    }
    sync_reload = 1; ui.force = 1;
    return 0;
}
/* a project object's length: FUN9 (today's), FUN8 (before the DRUM lane levels, 1.0.x), FUN7 */
static int proj_store_len(uint32_t len)
{
    return len == sizeof(project_store_t) || len == PROJ_STORE_VA || len == PROJ_STORE_V9 || len == PROJ_STORE_V8 ||
           len == PROJ_STORE_V7;
}
static uint32_t ed_bk_write(const uint8_t *a, uint32_t n)
{
    /* the arguments first: a malformed request never stops the transport */
    if (n < 2u || a[0] > 3u || (a[1] > 10u && (a[1] < ED_BK_SONG || a[1] >= ED_BK_SONG + NSONG * 4u))) return 1;
    if (a[0] == 0u) {
        if (n != 12u || a[6] > 15u || a[11] > 15u) return 1;
        uint32_t len = ed_bk_r32(a + 2);
        if (len > ED_BK_MAX || (a[1] == 0u && !proj_store_len(len)) ||
            (a[1] == 1u && len != sizeof(persist_t)) ||
            (a[1] >= 2u && a[1] <= 5u && len && !proj_store_len(len)) ||
            ((a[1] == 6u || a[1] == 7u) && len && len != sizeof(up_bank_t)) ||
            (a[1] == 8u && len && len != sizeof(fm6_bank_t)) || (a[1] == 9u && len && len != sizeof(upf_t)) ||
            (a[1] == 10u && len != sizeof(song_index_t)) || (a[1] >= ED_BK_SONG && len && !proj_store_len(len))) return 1;
        if (ed_flash_stop()) return 3;
        ed_bk_valid = 0; ed_bk_put = 1; ed_bk_id = a[1]; ed_bk_len = len; ed_bk_gen = ++proj_wire_gen;
        ed_bk_crc = ed_bk_r32(a + 7); ed_bk_pos = 0;
        ed_bk_usb = usb.resets; ed_bk_ms = fm1_ms;
        return 0;
    }
    if (!ed_bk_put || ed_bk_id != a[1] || ed_bk_usb != usb.resets || fm1_ms - ed_bk_ms > 15000u ||
        ed_bk_gen != proj_wire_gen) {                 /* (a project save / load reused the staging RAM) */
        ed_bk_put = 0; return 5;
    }
    ed_bk_ms = fm1_ms;
    if (a[0] == 3u) { ed_bk_put = 0; return n == 2u ? 0u : 1u; }
    if (a[0] == 2u) {
        if (n != 2u) return 1;
        if (ed_flash_stop()) return 3;
        uint32_t rc = ed_bk_commit(); ed_bk_put = 0; return rc;
    }
    if (n < 9u || a[6] > 15u || ed_bk_r32(a + 2) != ed_bk_pos) return 1;
    uint32_t count = ed_unpack7(a + 7, n - 7u, ed_bk_buf, sizeof ed_bk_buf);
    if (!count || count > ed_bk_len - ed_bk_pos) return 1;
    if (ed_flash_stop()) return 3;
    memcpy(ED_BK_RAW + ed_bk_pos, ed_bk_buf, count); ed_bk_pos += count;
    return 0;
}
static int ed_backup_handle(uint32_t cmd, const uint8_t *a, uint32_t n)
{
    if (cmd == ED_BACKUP_LIST) {
        uint32_t rc = n ? 1u : ed_bk_capture();
        ed_b(1); ed_b(rc); ed_b(rc ? 0u : ED_BK_N);
        if (!rc) for (uint32_t i = 0; i < ED_BK_N; i++) {
            uint32_t len;
            const uint8_t *p = ed_bk_object(ED_BK_IDS[i], &len);
            ed_b(ED_BK_IDS[i]); ed_bk_u32(len); ed_bk_u32(st_crc32(p, len));
            fm1_wdt_feed();
        }
        return 1;
    }
    if (cmd == ED_BACKUP_GET) {
        uint32_t len = 0, off = n >= 6u ? ed_bk_r32(a + 1) : 0u;
        uint32_t count = n == 8u ? (uint32_t)a[6] | (uint32_t)a[7] << 7 : 0u;
        const uint8_t *p = n ? ed_bk_object(a[0], &len) : 0;
        uint32_t rc = (!ed_bk_valid || ed_bk_usb != usb.resets || (n && !a[0] && ed_bk_gen != proj_wire_gen)) ? 5u : transport_busy() ? 3u :
            n != 8u || a[5] > 15u || !p || !count || count > 256u || off > len || count > len - off ? 1u : 0u;
        ed_b(n ? a[0] : 127u); ed_b(rc); ed_bk_u32(off); ed_b(rc ? 0u : count & 127u); ed_b(rc ? 0u : count >> 7);
        if (!rc) ed_bk_pack(p + off, count);
        return 1;
    }
    if (cmd == ED_BACKUP_PUT) {
        uint32_t rc = ed_bk_write(a, n);
        ed_b(n ? a[0] : 127u); ed_b(n >= 2u ? a[1] : 127u); ed_b(rc);
        return 1;
    }
    return 0;
}
