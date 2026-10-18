/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DigiSwarm's UI: its effect for fxkit, its Mod Menu entry, its screen, its keys and knobs, its project data.
 * DIGISWARM in the Mod Menu makes it fxkit's active effect (it owns the output, the synths muted) and opens its
 * page; MIDI leaves the page with DigiSwarm playing (its entry brings it back), NO turns it off.
 *
 *   six pages, as the Digitone's own (LEFT, RIGHT or PAGE moves between them):
 *     SYN1  1 EFFECT  ACT REP SHP FLT / MIX TIME SPC LOOP        pushing a knob: its default
 *     SYN2  2 SHIFT   GAIN MDEP MRAT RESO / FXVL LSPD VERB FADE  (also FUNC + a knob on page 1)
 *     FLTR  3 LOOPER  HOLD REV BYP ENG / LOOP STOP UNDO CLR      turn A-C for off/on, D for the engine; push
 *                                                                to toggle or act
 *     AMP   the MIXER;  LFO  LFO 1, again LFO 2 (fxkit's: DEST lists DigiSwarm's controls and the mixer's)
 *   RECORD LOCK mode: hold a trig and turn a control, that step's p-lock (FUNC + trig clears it, PAGE the next
 *          16 steps); Hold, Reverse and the engine lock too (page 3's HOLD, REV and ENG)
 *   trig keys 1-11   the engines; 12 HOLD; 13-16 the variation, A-D (outside LOCK mode)
 *   T1 the looper: record, play, overdub (held: undo);  T2 stop (held: erase), as two footswitches
 *   YES SETUP, NO back (from LOCK mode: out of it; from a page: DigiSwarm off);  PLAY, STOP, TEMPO and
 *   LEVEL/DATA stay stock
 *   MIDI: its CCs (the map below), on the auto channel (or any, in SETUP), while DigiSwarm is active */
#pragma GCC optimize ("no-tree-loop-distribute-patterns")
#include "dswarm.h"

#define KEY_FUNC    1
#define KEY_MIDI    2
#define KEY_TEMPO   9
#define KEY_RECORD  10
#define KEY_PLAY    11
#define KEY_STOP    12
#define KEY_YES     13
#define KEY_NO      14
#define KEY_UP      15
#define KEY_DOWN    16
#define KEY_LEFT    17
#define KEY_RIGHT   18
#define KEY_PAGE    19
#define KEY_SYN1    21
#define KEY_LFO     25
#define KEY_STEP1   26
#define KEY_T1      42
#define KEY_PUSH_A  46
#define EV_KEY(ev)   (*(u32 *)((char *)(ev) + 12))
#define EV_FLAGS(ev) (*(u32 *)((char *)(ev) + 16))
#define EV_DELTA(ev) (*(s32 *)((char *)(ev) + 16))
#define COUNTS      4                       /* encoder counts a detent */

typedef void (*rect_fn)(void *bmp, int x0, int y0, int x1, int y1, int colour);
typedef void (*text_fn)(void *bmp, const void *font, int x, int y, int maxlen, const char *fmt, ...);
#define FILLRECT  ((rect_fn)fw_fillrect)
#define FRAMERECT ((rect_fn)fw_framerect)
#define TEXTF     ((text_fn)fw_textf)
#define FONT5     ((const void *)fw_font5)          /* 3 x 5: the stock value boxes' */
#define FONT_LBL  ((const void *)fw_font_label)     /* the stock labels' */
#define FONT_TTL  ((const void *)fw_font_title)     /* the stock title bar's */

static const u8 knob_def[8] = { 64, 64, 0, 127, 64, 48, 32, 100 };
static const u8 shift_def[8] = { 64, 0, 40, 0, 100, 64, 0, 30 };
static const char *const knob_lbl[8] = { "ACT", "REP", "SHP", "FLT", "MIX", "TIME", "SPC", "LOOP" };
static const char *const shift_lbl[8] = { "GAIN", "MDEP", "MRAT", "RESO", "FXVL", "LSPD", "VERB", "FADE" };
static const char *const shapes[4] = { "FLAT", "SWEL", "PERC", "ARCH" };
static const char *const times[6] = { "BAR", "1/2", "1/4", "1/8", "1/16", "1/32" };
static const char *const speeds[5] = { "1/4X", "1/2X", "1X", "2X", "4X" };
static const char *const rooms[4] = { "ROOM", "DARK", "HALL", "AMBI" };
static const char *const eng_title[E_COUNT] = { "Shards", "Seq", "Glide", "Mist", "Drone", "Rake",
                                                "Chops", "Stutter", "Arp", "Pattern", "Warp" };
static const char *const cfg_lbl[G_COUNT] = { "INPUT", "LOOP ROUTE", "LOOPER ONLY", "QUANTIZE", "BURST",
                                              "HOLD", "LOOP ORDER", "MIDI CC" };
static const char *const cfg_val[G_COUNT][2] = { { "STEREO", "MONO" }, { "POST-FX", "PRE-FX" }, { "OFF", "ON" },
                                                 { "OFF", "ON" }, { "OFF", "ON" }, { "LATCH", "MOMENT" },
                                                 { "REC-PLAY", "REC-DUB" }, { "AUTO CH", "ANY CH" } };

static u8 ui_open, ui_setup, ui_pg, ui_func, ui_ready, ui_inited, ui_row, ui_pending;
static u8 ui_flash_ticks, ui_flash_pg, ui_flash_i;   /* a knob's value in the title bar after a turn */
#define NPAGE 6                             /* EFFECT, SHIFT, LOOPER, the MIXER, LFO 1, LFO 2 */
#define NROW (G_COUNT + 1)                  /* SETUP's rows, and LOCK TRACK */
static u8 in_cc;                            /* the MIDI task: the controls change at the next tick */
static volatile u8 dc_on;                   /* DigiSwarm is the active effect */
#define EFF (&dswarm_effect)
static u32 clear_at;                        /* the next address to clear, 0 when nothing is */
static u32 bclear_at;                       /* the overdub layer's clearing, in frames */
static s32 enc_acc[8];

/* ---- the effect: DigiSwarm's controls as fxkit's slots ------------------------------------------------- */
static const char *const eng_short[E_COUNT] = { "SHRD", "SEQ", "GLID", "MIST", "DRON", "RAKE", "CHOP", "STUT",
                                                "ARP", "PATT", "WARP" };
static const char *const var_names[4] = { "A", "B", "C", "D" };
static const char *const off_on[2] = { "OFF", "ON" };
static const struct fx_param dswarm_params[] = {
    { DC_S_KNOB(0), FX_KNOB, 0, 64, "ACT", "Activity", 0 },
    { DC_S_KNOB(1), FX_KNOB, 0, 64, "REP", "Repeats", 0 },
    { DC_S_KNOB(2), FX_KNOB, 0, 0, "SHP", "Shape", 0 },
    { DC_S_KNOB(3), FX_KNOB, 0, 127, "FLT", "Filter", 0 },
    { DC_S_KNOB(4), FX_KNOB, 0, 64, "MIX", "Mix", 0 },
    { DC_S_KNOB(5), FX_KNOB, 0, 48, "TIME", "Time", 0 },
    { DC_S_KNOB(6), FX_KNOB, 0, 32, "SPC", "Space", 0 },
    { DC_S_KNOB(7), FX_KNOB, 0, 100, "LOOP", "Loop Level", 0 },
    { DC_S_SHIFT(0), FX_KNOB, 0, 64, "GAIN", "Input Gain", 0 },
    { DC_S_SHIFT(1), FX_KNOB, 0, 0, "MDEP", "Mod Depth", 0 },
    { DC_S_SHIFT(2), FX_KNOB, 0, 40, "MRAT", "Mod Rate", 0 },
    { DC_S_SHIFT(3), FX_KNOB, 0, 0, "RESO", "Resonance", 0 },
    { DC_S_SHIFT(4), FX_KNOB, 0, 100, "FXVL", "FX Volume", 0 },
    { DC_S_SHIFT(5), FX_KNOB, 0, 64, "LSPD", "Loop Speed", 0 },
    { DC_S_SHIFT(6), FX_KNOB, 0, 0, "VERB", "Reverb", 0 },
    { DC_S_SHIFT(7), FX_KNOB, 0, 30, "FADE", "Loop Fade", 0 },
    { DC_S_ENGINE, FX_OPT, E_COUNT, E_SHARDS, "ENG", "Engine", eng_short },
    { DC_S_VAR, FX_OPT, 4, 0, "VAR", "Variation", var_names },
    { DC_S_REVERSE, FX_OPT, 2, 0, "REV", "Reverse", off_on },
    { DC_S_HOLD, FX_OPT, 2, 0, "HOLD", "Hold", off_on },
    { 0 }
};

/* The render's: this block's values (DigiSwarm's own, a step's lock, an LFO on it) into its controls */
static void dswarm_values(const int16_t *val, uint32_t flags)
{
    int i;
    s32 e = val[DC_S_ENGINE] >> 8, v = val[DC_S_VAR] >> 8;
    for (i = 0; i < 8; i++) {
        s32 k = val[DC_S_KNOB(i)] >> 8, s = val[DC_S_SHIFT(i)] >> 8;
        dswarm.knob[i] = (u8)(k < 0 ? 0 : k > 127 ? 127 : k);
        dswarm.shift[i] = (u8)(s < 0 ? 0 : s > 127 ? 127 : s);
    }
    dswarm.engine = (u8)(e < 0 ? 0 : e >= E_COUNT ? E_COUNT - 1 : e);
    dswarm.var = (u8)(v < 0 ? 0 : v > 3 ? 3 : v);
    dswarm.reverse = (u8)(val[DC_S_REVERSE] >= 128);
    dswarm.hold = (u8)(val[DC_S_HOLD] >= 128);
    dswarm.bypass = (u8)((flags & FX_V_BYPASS) != 0);
}

const struct fx_effect dswarm_effect = {
    FX_ABI, "DIGISWARM", dswarm_params, FX_OWN_MIX | FX_OWN_BYPASS, 0, dswarm_values, dswarm_render,
};

static int own(s32 slot)                    /* a control's own value (what it plays without locks or LFOs) */
{
    return (int)(fx_get(EFF, slot) >> 8);
}

static int shown(s32 slot)                  /* what its control shows: its own value, or the held step's lock */
{
    s32 v = fx_view(slot, fx_get(EFF, slot)) >> 8;
    return v < 0 ? 0 : v > 127 ? 127 : (int)v;
}

/* A control's own value (from the MIDI task, at the next tick); turned on the page in LOCK mode with a step held,
 * that step's lock */
static void set_slot(s32 slot, s32 v)
{
    if (in_cc)
        fx_set_later(EFF, slot, v << 8);
    else
        fx_set(EFF, slot, v << 8);
}

static void turn_slot(s32 slot, s32 v)
{
    if (fx_held_step() >= 0)
        fx_turned(EFF, slot, v << 8);
    else
        set_slot(slot, v);
}

static void set_hold(int on)
{
    dswarm.hold = (u8)on;
    set_slot(DC_S_HOLD, on);
}

static void set_reverse(int on)
{
    dswarm.reverse = (u8)on;
    set_slot(DC_S_REVERSE, on);
}

static void set_bypass(int on)
{
    dswarm.bypass = (u8)on;
    set_slot(FX_M_BYP, on);
}

/* ---- project data: the page, its mixer and LFOs, SETUP, saved with the project ---------------------------
 * 44 bytes (the gap mods share in a project holds 480: core leaves out a block that does not fit). The p-locks
 * are the pattern's. */
struct dc_save {
    u8 ver, cfg, ev, pad;               /* cfg: a bit a SETUP row (eight rows); ev: the engine, the variation << 4,
                                           Reverse 0x40, Hold 0x80 */
    u8 knob[8], shift[8];
    u8 fx[FX_PACK];                     /* fxkit's: the mixer, the LFOs */
} __attribute__((aligned(4)));
static struct dc_save save;
#define SAVE_VER 3

static void defaults(void)
{
    int i;
    for (i = 0; i < 8; i++) {
        set_slot(DC_S_KNOB(i), knob_def[i]);
        set_slot(DC_S_SHIFT(i), shift_def[i]);
    }
    set_slot(DC_S_ENGINE, E_SHARDS);
    set_slot(DC_S_VAR, 0);
    set_slot(DC_S_REVERSE, 0);
    set_slot(DC_S_HOLD, 0);
    fx_unpack(EFF, 0);
    for (i = 0; i < G_COUNT; i++)
        dswarm.cfg[i] = 0;
}

static void dswarm_loaded(int found)
{
    int i;
    if (!found || save.ver != SAVE_VER || (save.ev & 15) >= E_COUNT) {
        defaults();
    } else {
        for (i = 0; i < 8; i++) {
            set_slot(DC_S_KNOB(i), save.knob[i] & 127);
            set_slot(DC_S_SHIFT(i), save.shift[i] & 127);
        }
        set_slot(DC_S_ENGINE, save.ev & 15);
        set_slot(DC_S_VAR, (save.ev >> 4) & 3);
        set_slot(DC_S_REVERSE, (save.ev >> 6) & 1);
        set_slot(DC_S_HOLD, save.ev >> 7);
        fx_unpack(EFF, save.fx);
        for (i = 0; i < G_COUNT; i++)
            dswarm.cfg[i] = (save.cfg >> i) & 1;
    }
    ui_inited = 1;
}

const struct core_projdata dswarm_projdata = { 0x4443534Du /* "DCSM" */, sizeof(struct dc_save), &save,
                                              dswarm_loaded };

static u32 saved_serial = ~0u;

static void keep_save(void)                 /* the page into the project's copy (its controls when they moved) */
{
    int i;
    u8 cfg = 0;
    save.ver = SAVE_VER;
    if (fx_serial(EFF) != saved_serial) {
        saved_serial = fx_serial(EFF);
        save.ev = (u8)(own(DC_S_ENGINE) | own(DC_S_VAR) << 4 | own(DC_S_REVERSE) << 6 | own(DC_S_HOLD) << 7);
        for (i = 0; i < 8; i++) {
            save.knob[i] = (u8)own(DC_S_KNOB(i));
            save.shift[i] = (u8)own(DC_S_SHIFT(i));
        }
        fx_pack(EFF, save.fx);
    }
    for (i = 0; i < G_COUNT; i++)
        cfg |= (u8)((dswarm.cfg[i] & 1) << i);
    save.cfg = cfg;
}

/* ---- opening and closing -------------------------------------------------------------------------- */
static void activate(void)
{
    if (fx_active() != EFF) {               /* (back from MIDI: as it was) */
        set_bypass(0);                      /* switching it on steps on it */
        fx_activate(EFF);
    }
    dc_on = 1;
    ui_pending = 0;
}

static void dswarm_open(void *brain, void *ev, int track)
{
    (void)brain, (void)ev, (void)track;
    ui_open = 1;
    ui_setup = 0;
    ui_pg = 0;
    ui_func = 0;
    if (!ui_ready && !clear_at)
        clear_at = DC_BULK;                 /* the first time: clear what the render reads */
    if (ui_ready)
        activate();
    else
        ui_pending = 1;                     /* at the tick that ends the clearing */
}

static void dswarm_hide(void)                /* the page gone (MIDI), DigiSwarm playing on */
{
    ui_open = 0;
    fx_lock_mode_set(0);
}

static void dswarm_close(void)               /* NO: DigiSwarm off, the synths again */
{
    dswarm_hide();
    ui_pending = 0;
    set_hold(0);
    fx_deactivate(EFF);
    dc_on = 0;
}

/* The Mod Menu's DIGISWARM, with its icon: an orb. */
static const u16 dswarm_icon[16] = {
    0x07e0, 0x1818, 0x2004, 0x43c2, 0x4c32, 0x9009, 0x93c9, 0xa5a5,
    0xa5a5, 0x93c9, 0x9009, 0x4c32, 0x43c2, 0x2004, 0x1818, 0x07e0
};
const struct core_menu_item dswarm_menu = { "DIGISWARM", dswarm_open, CORE_MENU_ICON, dswarm_icon };

/* ---- keys ------------------------------------------------------------------------------------------- */
static void set_knob(int i, int func, s32 v)  /* a knob's own value */
{
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    if (func)
        dswarm.shift[i] = (u8)v;
    else
        dswarm.knob[i] = (u8)v;
    set_slot(func ? DC_S_SHIFT(i) : DC_S_KNOB(i), v);
}

static void turn_knob(int i, int func, s32 v)  /* turned on the page: in LOCK mode with a step held, its lock */
{
    if (fx_held_step() < 0) {
        set_knob(i, func, v);
        return;
    }
    turn_slot(func ? DC_S_SHIFT(i) : DC_S_KNOB(i), v < 0 ? 0 : v > 127 ? 127 : v);
}

static void flash(int pg, int i)              /* the knob's value in the title bar, a second */
{
    ui_flash_pg = (u8)pg;
    ui_flash_i = (u8)i;
    ui_flash_ticks = 30;
}

static void hold_key(void)
{
    set_hold(dswarm.cfg[G_HOLDMOM] ? 1 : !own(DC_S_HOLD));
}

static void loop_key(void)                   /* T1, and LOOP on page 3: record, play, overdub */
{
    dswarm.lcmd = dswarm.cfg[G_BURST] ? C_BURST_DOWN : C_T1;
}

static int page_now(void)                    /* FUNC held on DigiSwarm's pages shows page 2 */
{
    return ui_func && ui_pg < 3 ? 1 : ui_pg;
}

static void push(int i)                      /* an encoder pushed */
{
    int pg = page_now();
    if (pg >= 3) {
        fx_page_push(EFF, pg - 3, i);       /* its value under the knob, as the stock's */
    } else if (pg < 2) {
        turn_knob(i, pg, pg ? shift_def[i] : knob_def[i]);
        flash(pg, i);
    } else if (i == 0) {
        hold_key();
    } else if (i == 1) {
        set_reverse(!own(DC_S_REVERSE));
    } else if (i == 2) {
        set_bypass(!own(FX_M_BYP));
    } else if (i == 3) {
        turn_slot(DC_S_ENGINE, (shown(DC_S_ENGINE) + 1) % E_COUNT);
    } else if (i == 4) {
        loop_key();
    } else if (i == 5) {
        dswarm.lcmd = C_STOP;
    } else if (i == 6 && !dswarm.cfg[G_BURST]) {
        dswarm.lcmd = C_UNDO;
    } else if (i == 7) {
        dswarm.lcmd = C_ERASE;
    }
}

int dswarm_key(void *brain, void *ev)
{
    u32 key = EV_KEY(ev), fl = EV_FLAGS(ev);
    int press = (fl & 1) && !(fl & 8), released = (fl & 0x10) != 0, looper = !ui_setup && page_now() == 2;
    (void)brain;
    if (!ui_open)
        return 0;
    if (key == KEY_FUNC) {                  /* down: flags 1 (5 for a double press); up: 0x10, or 0 after
                                               another key went down meanwhile */
        ui_func = (u8)(fl & 1);
        return 1;
    }
    if (fl & 2)                             /* another key's event: FUNC is held */
        ui_func = 1;
    if (key == KEY_PLAY || key == KEY_STOP || key == KEY_TEMPO)
        return 0;                           /* the sequencer and its tempo stay the Digitone's */
    if (!ui_setup && page_now() >= 3 && fx_page_key(EFF, page_now() - 3, key, fl))
        return 1;                           /* DEST's list on an LFO page */
    if (!ui_setup && fx_lock_key(EFF, key, fl, ui_func))
        return 1;                           /* LOCK mode's trigs and PAGE */
    if (released) {
        if ((key == KEY_STEP1 + 11 || (looper && key == KEY_PUSH_A)) && dswarm.cfg[G_HOLDMOM])
            set_hold(0);
        if ((key == KEY_T1 || (looper && key == KEY_PUSH_A + 4)) && dswarm.cfg[G_BURST])
            dswarm.lcmd = C_BURST_UP;
        return 1;
    }
    if (!press)
        return 1;                           /* repeats; ev_hold sees the track keys held */
    if (key == KEY_MIDI) {
        dswarm_hide();
        return 0;                           /* the stock's MIDI mode, DigiSwarm playing on */
    } else if (key == KEY_RECORD) {
        if (!ui_setup)
            fx_lock_mode_set(!fx_lock_mode());
    } else if (key >= KEY_STEP1 && key < KEY_STEP1 + 16 && (ui_setup || ui_func)) {
        ;                                   /* (0.1's presets) */
    } else if (key >= KEY_STEP1 && key < KEY_STEP1 + E_COUNT) {
        set_slot(DC_S_ENGINE, key - KEY_STEP1);
    } else if (key == KEY_STEP1 + 11) {
        hold_key();
    } else if (key >= KEY_STEP1 + 12 && key <= KEY_STEP1 + 15) {
        set_slot(DC_S_VAR, key - KEY_STEP1 - 12);
    } else if (key == KEY_T1) {
        loop_key();
    } else if (key == KEY_T1 + 1) {
        dswarm.lcmd = C_STOP;
    } else if (key == KEY_YES) {
        ui_setup = !ui_setup;
    } else if (key == KEY_NO) {
        if (ui_setup)
            ui_setup = 0;
        else if (fx_lock_mode())
            fx_lock_mode_set(0);
        else
            dswarm_close();
    } else if (ui_setup && (key == KEY_UP || key == KEY_DOWN)) {
        ui_row = (u8)((ui_row + (key == KEY_UP ? NROW - 1 : 1)) % NROW);
    } else if (ui_setup && (key == KEY_LEFT || key == KEY_RIGHT)) {
        if (ui_row == G_COUNT) {
            fx_lock_track_set((fx_lock_track() + (key == KEY_RIGHT ? 1 : 3)) % 4);
        } else {
            dswarm.cfg[ui_row] = !dswarm.cfg[ui_row];
            if (ui_row == G_HOLDMOM)
                set_hold(0);
        }
    } else if (ui_setup) {
        ;
    } else if (key >= KEY_SYN1 && key <= KEY_LFO) {
        ui_pg = (u8)(key == KEY_LFO ? (ui_pg == 4 ? 5 : 4) : key - KEY_SYN1);
        ui_flash_ticks = 0;
    } else if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_PAGE) {
        ui_pg = (u8)((ui_pg + (key == KEY_LEFT ? NPAGE - 1 : 1)) % NPAGE);
        ui_flash_ticks = 0;
    } else if (key >= KEY_PUSH_A && key < KEY_PUSH_A + 8) {
        push((int)(key - KEY_PUSH_A));
    }
    return 1;
}

/* core-dn1's ev_hold, a track key held on its own: while open, T1 undoes the overdub, T2 erases. */
int dswarm_hold(void *brain, void *ev, int track)
{
    (void)brain, (void)ev;
    if (!ui_open)
        return 0;
    if (track == 0 && !dswarm.cfg[G_BURST])
        dswarm.lcmd = C_UNDO;
    else if (track == 1)
        dswarm.lcmd = C_ERASE;
    return 1;
}

int dswarm_enc(void *brain, void *ev)
{
    u32 e = EV_KEY(ev);
    s32 d;
    int i, pg;
    (void)brain;
    if (!ui_open || e < 1 || e > 8)
        return 0;                           /* LEVEL/DATA stays the Digitone's */
    if (ui_setup)
        return 1;
    i = (int)e - 1;
    enc_acc[i] += EV_DELTA(ev);
    d = enc_acc[i] / COUNTS;
    enc_acc[i] -= d * COUNTS;
    if (!d)
        return 1;
    pg = page_now();
    if (pg >= 3) {
        fx_page_turn(EFF, pg - 3, i, d);    /* its value under the knob, as the stock's */
    } else if (pg < 2) {
        turn_knob(i, pg, shown(pg ? DC_S_SHIFT(i) : DC_S_KNOB(i)) + d);
        flash(pg, i);
    } else if (i == 0 && !dswarm.cfg[G_HOLDMOM]) {  /* page 3: right on, left off (a lock, with a step held) */
        turn_slot(DC_S_HOLD, d > 0);
    } else if (i == 1) {
        turn_slot(DC_S_REVERSE, d > 0);
    } else if (i == 2 && fx_held_step() < 0) {
        set_bypass(d > 0);
    } else if (i == 3) {
        int e = shown(DC_S_ENGINE) + (d > 0 ? 1 : -1);
        turn_slot(DC_S_ENGINE, e < 0 ? 0 : e >= E_COUNT ? E_COUNT - 1 : e);
    }
    return 1;
}

/* ---- MIDI: the CC map (core-dn1 3.2's ev_midi_cc, the MIDI task) -------------------------------------------
 * While DigiSwarm is active, the CCs below are its own when they come on the auto channel (SETUP's MIDI CC:
 * ANY CH, on a track's channel too): the Digitone does not apply them. Every other CC, and every CC while
 * it is off, goes on to the Digitone. A switch is on from 64; a looper CC acts from 64. CC 5 and
 * CC 18 take steps, 0-5: 1/4, 1/2, TAP, 2x, 4x, 8x (and anything above as 8x). The six
 * subdivisions are DigiSwarm's six TIME steps, a bar down to 1/32; the loop speeds are DigiSwarm's five, 8x
 * as 4x. */
static const u8 cc_knob[8] = { K_ACT, K_SHP, K_FLT, K_MIX, K_TIME, K_REP, K_SPC, K_LOOP };          /* CC 6-13 */
static const u8 cc_shift[8] = { S_MRATE, S_RESO, S_FXVOL, S_LSPEED, S_LSPEED, S_MDEP, S_VERB, S_FADE }; /* 14-21 */
static const u8 cc_cfg[4] = { G_PRE, G_ONLY, G_BURST, G_QUANT };                                    /* 24-27 */
static const u8 cc_time[6] = { 10, 32, 53, 74, 96, 117 };       /* CC 5: the middle of each TIME step */
static const u8 cc_lspeed[6] = { 12, 38, 64, 89, 115, 115 };    /* CC 18: of each LSPD step */

static int cc_take(int cc, int value);

int dswarm_cc(int track, int cc, int value, int flags)
{
    int took;
    (void)track;
    if (!dc_on || !ui_ready || (!(flags & CORE_MIDI_CC_AUTO) && !dswarm.cfg[G_MIDI]))
        return 0;
    if (value < 0 || value > 127)
        return 0;
    in_cc = 1;
    took = cc_take(cc, value);
    in_cc = 0;
    return took;
}

static int cc_take(int cc, int value)
{
    int on = value >= 64, s = dswarm.lstate;
    if (cc == 5) {                              /* Subdivision, stepped */
        set_knob(K_TIME, 0, cc_time[value < 5 ? value : 5]);
    } else if (cc == 18) {                      /* Loop Speed, stepped */
        set_knob(S_LSPEED, 1, cc_lspeed[value < 5 ? value : 5]);
    } else if (cc >= 6 && cc <= 13) {
        set_knob(cc_knob[cc - 6], 0, value);
    } else if (cc >= 14 && cc <= 21) {          /* 17 is Loop Speed, 0-127 */
        set_knob(cc_shift[cc - 14], 1, value);
    } else if (cc == 23 || cc == 47) {          /* Reverse */
        set_reverse(on);
    } else if (cc >= 24 && cc <= 27) {          /* the looper's settings */
        dswarm.cfg[cc_cfg[cc - 24]] = (u8)on;
    } else if (cc >= 28 && cc <= 35 && cc != 32 && cc != 33) {
        if (!on)
            return 1;
        if (cc == 28)                           /* Record: start; close; or a new take */
            dswarm.lcmd = s == L_EMPTY || s == L_REC ? C_T1 : C_BURST_DOWN;
        else if (cc == 29)                      /* Play */
            dswarm.lcmd = s == L_REC ? C_BURST_UP : s == L_STOP || s == L_DUB ? C_T1 : C_NONE;
        else if (cc == 30)                      /* Overdub, on and off */
            dswarm.lcmd = s == L_PLAY || s == L_DUB ? C_T1 : C_NONE;
        else if (cc == 31)
            dswarm.lcmd = C_STOP;
        else if (cc == 34)
            dswarm.lcmd = C_ERASE;
        else
            dswarm.lcmd = C_UNDO;
    } else if (cc == 48) {                      /* Hold */
        set_hold(on);
    } else if (cc == 102) {                     /* Bypass: below 64 bypassed */
        set_bypass(!on);
    } else if (cc == 22 || cc == 45 || cc == 46 || cc == 93) {
        /* looper on/off, copy preset, save preset and tap tempo: nothing to do here (the
         * looper is always on, there are no presets, the tempo is the Digitone's), but taken, so
         * the Digitone does not apply them to the active track meanwhile */
    } else {
        return 0;
    }
    return 1;
}

/* ---- the UI task: clearing, the effect, redraws ---------------------------------------------------------- */
void dswarm_tick(void *ctrl)
{
    if (!ui_inited) {                       /* no project load reached us */
        defaults();
        ui_inited = 1;
    }
    if (clear_at) {                         /* 256 KB a tick */
        u32 *p = (u32 *)clear_at, *end = p + 65536;
        if ((u32)end > DC_CLEAR_END)
            end = (u32 *)DC_CLEAR_END;
        while (p < end)
            *p++ = 0;
        clear_at = (u32)end >= DC_CLEAR_END ? 0 : (u32)end;
        if (!clear_at) {
            dswarm_dsp_reset();
            dswarm.bvalid = 1;
            ui_ready = 1;
        }
    }
    if (dswarm.lclear_len) {                 /* undo: the overdub layer, 512 KB a tick */
        u32 *p = (u32 *)(DC_LOOPB) + bclear_at, *end = (u32 *)(DC_LOOPB) + dswarm.lclear_len;
        if (end > p + 131072)
            end = p + 131072;
        while (p < end)
            *p++ = 0;
        bclear_at = (u32)(end - (u32 *)DC_LOOPB);
        if (bclear_at >= dswarm.lclear_len) {
            bclear_at = 0;
            dswarm.lclear_len = 0;
            dswarm.bvalid = 1;
        }
    }
    if (ui_pending && ui_ready)
        activate();
    dc_on = (u8)(fx_active() == EFF);
    if (ui_open && !dc_on && !ui_pending)
        dswarm_hide();                       /* another effect took the audio */
    dswarm.meter[0] = (u16)(dswarm.meter[0] - (dswarm.meter[0] >> 3));
    dswarm.meter[1] = (u16)(dswarm.meter[1] - (dswarm.meter[1] >> 3));
    keep_save();
    if (ui_flash_ticks)
        ui_flash_ticks--;
    if (ui_open)
        *((unsigned char *)ctrl + 0x20) = 1;
}

/* ---- the screen: 128 x 64, y = 0 the bottom row ------------------------------------------------------ */
static void value_text(void *bmp, const void *font, int x, int y, int i, int func)
{
    int v;
    if (func >= 3) {                            /* the MIXER's, the LFOs' */
        char buf[8];
        s32 s = fx_page_slot(func - 3, i);
        fx_text(EFF, s, fx_view(s, fx_get(EFF, s)), buf);
        TEXTF(bmp, font, x, y, -1, "%s", buf);
        return;
    }
    v = shown(func ? DC_S_SHIFT(i) : DC_S_KNOB(i));
    if (!func) {
        if (i == K_SHP)
            TEXTF(bmp, font, x, y, -1, "%s", shapes[v >> 5]);
        else if (i == K_FLT && v == 127)
            TEXTF(bmp, font, x, y, -1, "OPEN");
        else if (i == K_TIME)
            TEXTF(bmp, font, x, y, -1, "%s", times[(v * 6) >> 7]);
        else
            TEXTF(bmp, font, x, y, -1, "%d", v);
        return;
    }
    if (i == S_LSPEED) {
        TEXTF(bmp, font, x, y, -1, "%s", speeds[(v * 5) >> 7]);
    } else if (i == S_VERB) {
        TEXTF(bmp, font, x, y, -1, "%s", rooms[(v * 4) >> 7]);
    } else if (i == S_GAIN) {
        int t = (v - 64) * 3;                   /* tenths of a dB */
        if (!v)
            TEXTF(bmp, font, x, y, -1, "MUTE");
        else
            TEXTF(bmp, font, x, y, -1, "%c%d.%d", t < 0 ? '-' : '+', (t < 0 ? -t : t) / 10, (t < 0 ? -t : t) % 10);
    } else if (i == S_FADE) {
        TEXTF(bmp, font, x, y, -1, "%d.%dS", v / 16, (v % 16) * 10 / 16);
    } else {
        TEXTF(bmp, font, x, y, -1, "%d", v);
    }
}

/* As the Digitone's own parameter pages (traced from its FLTR, AMP and LFO pages): an inverted box (here the
 * tempo) and title bar on top, a column on the left (the looper's state, the input's meter, as SYN and LEV)
 * and two rows of four controls, each 17 pixels square over its label: knobs, value boxes and switches. */
static const u8 col_x[4] = { 33, 59, 86, 112 };
#define ROW_Y(r)    ((r) ? 15 : 42)         /* a control's centre, rows 1 and 2 */
#define LBL_Y(r)    ((r) ? 0 : 27)          /* its label's bottom row */
static const signed char ring[12][2] = { {0, 8}, {1, 8}, {2, 8}, {3, 7}, {4, 7}, {5, 6}, {6, 5}, {7, 4}, {7, 3}, {8, 2},
                                {8, 1}, {8, 0} };             /* a knob's circle, one quadrant */
static const signed char needle[29][2] = {             /* its pointer: 270 degrees from 7:30 to 4:30, radius 6 */
    {-4, -4}, {-5, -3}, {-5, -3}, {-6, -2}, {-6, -1}, {-6, 0}, {-6, 1}, {-6, 2}, {-5, 3}, {-4, 4}, {-4, 5},
    {-3, 5}, {-2, 6}, {-1, 6}, {0, 6}, {1, 6}, {2, 6}, {3, 5}, {4, 5}, {4, 4}, {5, 3}, {6, 2}, {6, 1}, {6, 0},
    {6, -1}, {6, -2}, {5, -3}, {5, -3}, {4, -4} };
static const char *const sw_lbl[8] = { "HOLD", "REV", "BYP", "ENG", "LOOP", "STOP", "UNDO", "CLR" };

static void px(void *bmp, int x, int y, int c)
{
    FILLRECT(bmp, x, y, x, y, c);
}

static void corners(void *bmp, int x0, int y0, int x1, int y1, int c)
{
    px(bmp, x0, y0, c);
    px(bmp, x1, y0, c);
    px(bmp, x0, y1, c);
    px(bmp, x1, y1, c);
}

static void rframe(void *bmp, int x0, int y0, int x1, int y1)    /* a frame with the corners off */
{
    FRAMERECT(bmp, x0, y0, x1, y1, 1);
    corners(bmp, x0, y0, x1, y1, 0);
}

static void line(void *bmp, int x0, int y0, int x1, int y1)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;) {
        px(bmp, x0, y0, 1);
        if (x0 == x1 && y0 == y1)
            break;
        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static int text_w(const char *s)            /* FONT5's capitals, near enough */
{
    int n = 0;
    while (s[n])
        n++;
    return n ? 5 * n - 1 : 0;
}

static void small_text(void *bmp, int cx, int cy, const char *s)    /* font5, centred on cx, cy */
{
    int n = 0;
    while (s[n])
        n++;
    TEXTF(bmp, FONT5, cx - (4 * n - 1) / 2, cy - 2, -1, "%s", s);
}

static void label(void *bmp, int cx, int y, const char *s)
{
    if (*s)
        TEXTF(bmp, FONT_LBL, cx - text_w(s) / 2, y, -1, "%s", s);
}

static void knob(void *bmp, int cx, int cy, int v, int bipolar)
{
    int i, k = (v * 28 + 63) / 127;
    for (i = 0; i < 12; i++) {
        int dx = ring[i][0], dy = ring[i][1];
        px(bmp, cx + dx, cy + dy, 1);
        px(bmp, cx - dx, cy + dy, 1);
        px(bmp, cx + dx, cy - dy, 1);
        px(bmp, cx - dx, cy - dy, 1);
    }
    line(bmp, cx, cy, cx + needle[k][0], cy + needle[k][1]);
    if (bipolar) {                          /* the stock - and + below */
        FILLRECT(bmp, cx - 10, cy - 7, cx - 8, cy - 7, 1);
        FILLRECT(bmp, cx + 8, cy - 7, cx + 10, cy - 7, 1);
        FILLRECT(bmp, cx + 9, cy - 8, cx + 9, cy - 6, 1);
    }
}

static void box(void *bmp, int cx, int cy, const char *s, int on)    /* a value box; on: inverted */
{
    rframe(bmp, cx - 8, cy - 8, cx + 8, cy + 8);
    if (s)
        small_text(bmp, cx, cy, s);
    if (on)
        FILLRECT(bmp, cx - 7, cy - 7, cx + 7, cy + 7, -1);
}

static void shape_icon(void *bmp, int cx, int cy, int sh)   /* Shape's contour, over a dotted floor */
{
    int x, lo = cy - 4, hi = cy + 3;
    for (x = cx - 6; x <= cx + 6; x += 2)
        px(bmp, x, lo, 1);
    if (sh == 0) {                          /* FLAT */
        line(bmp, cx - 6, lo, cx - 6, hi);
        line(bmp, cx - 6, hi, cx + 6, hi);
        line(bmp, cx + 6, hi, cx + 6, lo);
    } else if (sh == 1) {                   /* SWELL */
        line(bmp, cx - 6, lo, cx + 6, hi);
        line(bmp, cx + 6, hi, cx + 6, lo);
    } else if (sh == 2) {                   /* PERC */
        line(bmp, cx - 6, lo, cx - 6, hi);
        line(bmp, cx - 6, hi, cx + 6, lo);
    } else {                                /* ARCH */
        line(bmp, cx - 6, lo, cx, hi);
        line(bmp, cx, hi, cx + 6, lo);
    }
}

static void loop_icon(void *bmp, int cx, int cy, int s)     /* the looper's state, 7 pixels */
{
    int k;
    if (s == L_REC || s == L_DUB) {         /* a dot; with a play arrow beside it, overdub */
        int x = s == L_DUB ? cx - 3 : cx;
        FILLRECT(bmp, x - 1, cy - 3, x + 1, cy + 3, 1);
        FILLRECT(bmp, x - 2, cy - 2, x + 2, cy + 2, 1);
        FILLRECT(bmp, x - 3, cy - 1, x + 3, cy + 1, 1);
        if (s == L_DUB)
            for (k = -3; k <= 3; k++)
                FILLRECT(bmp, cx + 2, cy + k, cx + 2 + (3 - (k < 0 ? -k : k)) / 1, cy + k, 1);
    } else if (s == L_PLAY) {               /* a play arrow */
        for (k = -3; k <= 3; k++)
            FILLRECT(bmp, cx - 2, cy + k, cx - 2 + 3 - (k < 0 ? -k : k), cy + k, 1);
    } else if (s == L_STOP) {               /* a square */
        FILLRECT(bmp, cx - 3, cy - 3, cx + 3, cy + 3, 1);
    } else {                                /* nothing recorded */
        FILLRECT(bmp, cx - 4, cy, cx - 2, cy, 1);
        FILLRECT(bmp, cx + 2, cy, cx + 4, cy, 1);
    }
}

static void title_bar(void *bmp, int pg)
{
    int t = fw_tempo / 120;
    TEXTF(bmp, FONT_TTL, t >= 100 ? 1 : 3, 56, -1, "%d", t);   /* the box: the tempo, as the stock's A01 */
    FILLRECT(bmp, 0, 54, 14, 63, -1);
    corners(bmp, 0, 54, 14, 63, 0);
    if (ui_flash_ticks && !ui_setup && ui_flash_pg != 2) {
        const char *l = ui_flash_pg >= 3 ? fx_page_label(EFF, ui_flash_pg - 3, ui_flash_i)
                        : ui_flash_pg ? shift_lbl[ui_flash_i] : knob_lbl[ui_flash_i];
        TEXTF(bmp, FONT_TTL, 19, 56, -1, "%s", l);
        value_text(bmp, FONT_TTL, 19 + text_w(l) + 6, 56, ui_flash_i, ui_flash_pg);
    } else if (ui_setup) {
        TEXTF(bmp, FONT_TTL, 19, 56, -1, "Setup");
    } else if (fx_lock_mode()) {
        TEXTF(bmp, FONT_TTL, 19, 56, -1, "Lock %d/%d", fx_lock_page() + 1, fx_lock_pages());
    } else if (fx_overload()) {
        TEXTF(bmp, FONT_TTL, 19, 56, -1, "CPU full: bypassed");
    } else if (pg >= 4) {
        TEXTF(bmp, FONT_TTL, 19, 56, -1, "%s", fx_page_title(pg - 3));     /* "LFO (1/2)", as the stock's */
    } else if (pg == 3) {
        TEXTF(bmp, FONT_TTL, 19, 56, -1, "%s (%d/%d)", fx_page_title(pg - 3), pg + 1, NPAGE);
    } else {
        TEXTF(bmp, FONT_TTL, 19, 56, -1, "%s %c (%d/%d)", eng_title[own(DC_S_ENGINE) % E_COUNT],
              'A' + (own(DC_S_VAR) & 3), pg + 1, NPAGE);
    }
    if (fx_lock_mode() && !ui_setup)
        fx_lock_strip(EFF, bmp, 62, 55);    /* the page's 16 steps: filled, a lock of DigiSwarm's */
    else
        TEXTF(bmp, FONT_TTL, 110, 56, -1, "C%d", dswarm.cpu / 10);
    FILLRECT(bmp, 17, 54, 127, 63, -1);
    corners(bmp, 17, 54, 127, 63, 0);
}

static void left_column(void *bmp)
{
    int s = dswarm.lstate, k, l, r, secs;
    rframe(bmp, 0, 27, 14, 50);             /* the looper, as the stock's SYN box */
    loop_icon(bmp, 7, 44, s);
    if (s != L_EMPTY) {
        secs = (int)((s == L_REC ? dswarm.lrec : dswarm.llen) / 48000);
        TEXTF(bmp, FONT5, secs >= 10 ? 3 : 5, 35, -1, "%d", secs);
    }
    if (dswarm.hold)
        small_text(bmp, 3, 31, "H");
    if (dswarm.reverse)
        small_text(bmp, 7, 31, "R");
    if (dswarm.bypass)
        small_text(bmp, 11, 31, "B");
    for (k = 0; k < 5; k++) {               /* the input's meter, as LEV: L and R */
        FILLRECT(bmp, 0, 7 + 4 * k, 1, 7 + 4 * k, 1);
        FILLRECT(bmp, 13, 7 + 4 * k, 14, 7 + 4 * k, 1);
    }
    rframe(bmp, 4, 7, 10, 23);
    l = (dswarm.meter[0] * 13) / 32767;
    r = (dswarm.meter[1] * 13) / 32767;
    if (l)
        FILLRECT(bmp, 6, 9, 6, 8 + l, 1);
    if (r)
        FILLRECT(bmp, 8, 9, 8, 8 + r, 1);
    label(bmp, 7, -1, "IN");
}

static void control(void *bmp, int pg, int i)
{
    int cx = col_x[i & 3], r = i >> 2, cy = ROW_Y(r), v;
    const char *lbl;
    if (pg == 0) {
        v = shown(DC_S_KNOB(i));
        lbl = knob_lbl[i];
        if (i == K_SHP) {
            box(bmp, cx, cy, 0, 0);
            shape_icon(bmp, cx, cy, v >> 5);
        } else if (i == K_TIME) {
            box(bmp, cx, cy, times[(v * 6) >> 7], 0);
        } else {
            knob(bmp, cx, cy, v, 0);
        }
    } else if (pg == 1) {
        v = shown(DC_S_SHIFT(i));
        lbl = shift_lbl[i];
        if (i == S_LSPEED)
            box(bmp, cx, cy, speeds[(v * 5) >> 7], 0);
        else if (i == S_VERB)
            box(bmp, cx, cy, rooms[(v * 4) >> 7], 0);
        else
            knob(bmp, cx, cy, v, i == S_GAIN);
    } else {
        lbl = sw_lbl[i];
        if (i < 3) {                        /* what plays, or the held step's lock */
            v = i == 0 ? (fx_held_step() >= 0 ? shown(DC_S_HOLD) : dswarm.hold)
                : i == 1 ? (fx_held_step() >= 0 ? shown(DC_S_REVERSE) : dswarm.reverse) : dswarm.bypass;
            box(bmp, cx, cy, v ? "ON" : "OFF", v);
        } else if (i == 3) {
            box(bmp, cx, cy, eng_short[shown(DC_S_ENGINE) % E_COUNT], 0);
        } else if (i == 4) {
            box(bmp, cx, cy, 0, 0);
            loop_icon(bmp, cx, cy, dswarm.lstate);
        } else if (i == 5) {
            box(bmp, cx, cy, 0, 0);
            FILLRECT(bmp, cx - 3, cy - 3, cx + 3, cy + 3, 1);
        } else if (i == 6) {                /* an arrow back */
            box(bmp, cx, cy, 0, 0);
            line(bmp, cx - 3, cy, cx + 3, cy);
            line(bmp, cx - 3, cy, cx - 1, cy + 2);
            line(bmp, cx - 3, cy, cx - 1, cy - 2);
            line(bmp, cx + 3, cy, cx + 3, cy - 3);
        } else if (i == 7) {                /* a cross */
            box(bmp, cx, cy, 0, 0);
            line(bmp, cx - 3, cy - 3, cx + 3, cy + 3);
            line(bmp, cx - 3, cy + 3, cx + 3, cy - 3);
        }
    }
    label(bmp, cx, LBL_Y(r), lbl);
}

static void draw_setup(void *bmp)
{
    static const char *const tracks[4] = { "T1", "T2", "T3", "T4" };
    int i;
    for (i = 0; i < NROW; i++) {
        int y = 49 - 6 * i;
        TEXTF(bmp, FONT5, 3, y, -1, "%s", i < G_COUNT ? cfg_lbl[i] : "LOCK TRACK");
        TEXTF(bmp, FONT5, 76, y, -1, "%s", i < G_COUNT ? cfg_val[i][dswarm.cfg[i] & 1] : tracks[fx_lock_track() & 3]);
        if (i == ui_row)
            FILLRECT(bmp, 1, y - 1, 126, y + 4, -1);
    }
}

void dswarm_draw(void *bmp, void *ctrl)
{
    int i, pg = page_now();
    (void)ctrl;
    if (!ui_open)
        return;
    FILLRECT(bmp, 0, 0, 127, 63, 0);
    if (!ui_ready) {
        TEXTF(bmp, FONT5, 30, 30, -1, "DIGISWARM...");
        return;
    }
    title_bar(bmp, pg);
    if (ui_setup) {
        draw_setup(bmp);
        return;
    }
    left_column(bmp);
    if (pg >= 3)
        fx_page_draw(EFF, bmp, pg - 3);
    else
        for (i = 0; i < 8; i++)
            control(bmp, pg, i);
}
