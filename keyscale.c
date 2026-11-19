/* SPDX-License-Identifier: GPL-2.0-or-later */
/* KEY SCALE for the Digitakt mk1 (OS 1.54, core 4.1, seq-events 0.1).
 *
 * The key track (KEY_TRACK, T9 = seq track 8) sets the key: offset = its note - 60 (C5). Its note
 * trigs are read in ev_fire_notes and then dropped (notes[0] = -1: a MIDI track with no note plays
 * nothing), so the key track is silent. The offset holds until the key track's next note trig, and
 * goes back to 0 on stop.
 *
 * Every fired note of a target track goes through:
 *     note + offset            if the track's TRANSPOSE is on (xpose_mask)
 *     then the scale remap     if its SCALE is on (scale_mask): note - pc + map[pc], folded into the
 *                              input's window
 *     then a clamp to 0..127
 * map[pc] is the output for input pitch class pc, 0..12: 12 is the C above (B -> 12 snaps B up to the
 * next C). The window is the octave from the wrap note (wrap, 0..11, C by default) at or below the
 * input to the wrap note above it, both ends included; an output outside it moves by octaves into it.
 * With wrap C that changes nothing. With wrap G, C -> B gives the B below C (G..G holds it), and
 * G -> F# the F# at the window's top, almost an octave up. A MIDI chord's tones are remapped one by one and handed back as
 * intervals from the remapped root; a tone that lands on the root becomes interval 0, which seq-events
 * reads as "no tone" (a duplicate dropped).
 *
 * Audio tracks fire before T9 within a step, so on the step where the key changes they would get the
 * old key. LOOKAHEAD: the first audio track's step load reads T9's next stored step from the pattern
 * and, if it holds a note trig, the audio targets use that note's key for this step. It assumes T9
 * runs at the audio targets' speed and ignores T9's conditions (a failed condition still moves the
 * audio tracks for that one step; the MIDI targets follow the real fire).
 *
 * UI: FUNC + TRIG opens stock's Quantize first; FUNC + TRIG again (no other key between) opens the
 * overlay on the active track (stock gets that press too, to leave Quantize). FUNC + TRIG on the
 * overlay closes it and goes back to Quantize, so the two alternate.
 *   on KEY_TRACK: the scale editor, two drawn keyboards: IN (remapped notes faded, the selection
 *                 inverted) above OUT (solid where an input lands, empty where none does, a count on
 *                 the notes several inputs land on), an arrow from the selection to its output, a
 *                 mark under OUT at the wrap note, and a dot on the last note any track sent through
 *                 the scale (its input after transpose on IN, its output on OUT), big for a moment
 *                 after it plays. The trig keys are a keyboard in FUNC + TRK's layout
 *                 (9-16 white, 2 3 5 6 7 black): hold a source and press its output to set it; a tap
 *                 only selects. LEFT/RIGHT move the selection; YES resets it to itself, FUNC + YES the
 *                 whole table. Page 1: knobs A-H set the outputs of C..G. UP/DOWN (or TRIG without
 *                 FUNC) flip to page 2: A-D for G#..B, E rotates the wrap note. A strip on the right
 *                 shows the page between UP and DOWN arrows.
 *   on any other: knob A SCALE on/off, knob B TRANSPOSE on/off for that track.
 * NO closes it; any other key but FUNC closes it and then does what it does.
 *
 * Saving: the table and the masks are kept in eMMC block SAVE_BLOCK (mod.json: 0x6004e, claimed as
 * emmc:0x6004e), FLEXMODS' scheme: loaded ~30 ticks after boot, saved 60 ticks after the last change.
 * The block is empty on a fresh drive; that stock never writes it is assumed, not proven.
 *
 * Interrupt level: ks_step and ks_notes (table lookups only). UI task: the rest. */
#include <stdint.h>
#include "digitakt-mk1/core3.h"
#include "digitakt-mk1/seq.h"

#define KEY_TRACK  8                        /* T9; the probe saw tracks fire 0..15 in order */
#define KEY_ROOT   60                       /* C5 (MIDI 60): the key note that means offset 0 */
#define KEY_SILENT 1                        /* drop the key track's notes */
#define LOOKAHEAD  1

/* ---- keys and knobs (digiemu's measured key map, devices/digitakt.toml) -------------------------- */
#define K_FUNC   1
#define K_NO     13
#define K_TRIG   19                         /* the TRIG page key; FUNC + TRIG is stock Quantize */
#define FL_DOWN   0x01
#define FL_FUNC   0x02
#define FL_REPEAT 0x08
#define EV_ID(ev)    (*(uint32_t *)((char *)(ev) + 12))
#define EV_FLAGS(ev) (*(uint32_t *)((char *)(ev) + 16))
#define EV_DELTA(ev) (*(int32_t *)((char *)(ev) + 16))
#define COUNTS   4                          /* raw encoder counts per detent */

/* ---- the pattern (seq-events-dt1/dev.h) ------------------------------------------------------- */
#define PATTERN      (*(const uint8_t *const volatile *)PATTERN_PP)
#define TRACK_STRIDE 911
#define TR_NOTE_LOCK 0x280                  /* s8[64]: note lock 1, -1 = the track's NOTE */
#define TR_DEF_TRIGS 0x382                  /* u16: the default trigs; +1 & 0x80 = default SYN */
#define TR_NOTE      0x384
#define W_TRIG       0x0001
#define W_NEVER      0x0004                 /* a step with this bit never fires */
#define W_SYN        0x0080
#define W_SYN_SET    0x0800
#define IN_DDR(p)    ((uint32_t)(p) >= 0x40000000u && (uint32_t)(p) < 0x48000000u)

#define PLAYING() (*(volatile uint32_t *)TRANSPORT_AT == 1)

/* ---- settings (global) ------------------------------------------------------------------------- */
static volatile uint8_t map[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static volatile uint16_t scale_mask = 0xfe00;   /* T10-T16 */
static volatile uint16_t xpose_mask = 0xfe00;
static volatile uint8_t wrap;               /* the remap window's start, a pitch class (0 = C..C) */

/* ---- playback state ------------------------------------------------------------------------------ */
static volatile int32_t offset;             /* the key, from the key track's last fired note */
static volatile int32_t ahead = -1000;      /* LOOKAHEAD: the key for this step's earlier tracks */
static volatile int32_t ahead_done;
static volatile int32_t key_last = -1;      /* the key track's last loaded step, -1 after stop */

/* Each track's last fired note, for the apply screen: in and out, what moved it, and a count of fires
 * (0: none yet) */
#define FX_XPOSE 1
#define FX_SCALE 2
static volatile uint8_t last_in[SQ_TRACKS], last_out[SQ_TRACKS], last_fx[SQ_TRACKS], last_n[SQ_TRACKS];
/* The last note any track sent through the scale, for the scale editor: its input pitch class (after
 * transpose), the output key it took, and a count of them */
static volatile uint8_t heard_pc, heard_out, heard_n;
static int32_t heard_fresh;                 /* UI ticks left of the mark's flash after a new one */
#define HEARD_FLASH 12

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int32_t remap(int32_t n)
{
    int32_t pc = n % 12, rel, base, out;
    if (pc < 0)
        pc += 12;
    out = n - pc + map[pc];
    rel = (n - wrap) % 12;
    if (rel < 0)
        rel += 12;
    base = n - rel;                         /* the wrap note at or below n */
    while (out < base)
        out += 12;
    while (out > base + 12)
        out -= 12;
    return out;
}

/* The key track's stored note at step s as a key offset, or -1000 when s has no note trig */
static int32_t stored_key(int32_t s)
{
    const uint8_t *pat = PATTERN, *trk;
    uint32_t w;
    int32_t syn, n;
    if (!IN_DDR(pat) || s < 0 || s >= SQ_STEPS)
        return -1000;
    trk = pat + TRACK_STRIDE * KEY_TRACK;
    w = (uint32_t)(trk[2 * s] << 8 | trk[2 * s + 1]);
    if (!(w & W_TRIG) || (w & W_NEVER))
        return -1000;
    syn = (w & W_SYN_SET) ? (w & W_SYN) != 0 : (trk[TR_DEF_TRIGS + 1] & W_SYN) != 0;
    if (!syn)
        return -1000;                       /* a lock trig: no note */
    n = (int8_t)trk[TR_NOTE_LOCK + s];
    if (n < 0)
        n = trk[TR_NOTE] & 127;
    return n - KEY_ROOT;
}

/* ---- ev_fire_step (interrupt level) ------------------------------------------------------------ */
int32_t ks_step(int32_t track, int32_t step, int32_t len)
{
    (void)len;
    if (track == KEY_TRACK) {
        key_last = step;
        ahead = -1000;
        ahead_done = 0;
    }
#if LOOKAHEAD
    else if (track < KEY_TRACK && !ahead_done) {
        struct sq_track info;
        int32_t next = 0;
        ahead_done = 1;
        if (key_last >= 0 && sq_track_info(KEY_TRACK, &info, sizeof info) == 0 && info.len > 0)
            next = (key_last + 1) % info.len;
        ahead = stored_key(next);
    }
#endif
    return step;
}

/* ---- ev_fire_notes (interrupt level) ----------------------------------------------------------- */
void ks_notes(int32_t track, int32_t step, int8_t notes[SQ_NOTES])
{
    uint32_t bit;
    int32_t off, root, n, i;
    (void)step;
    if (track == KEY_TRACK) {
        if (notes[0] >= 0) {
            offset = notes[0] - KEY_ROOT;
#if KEY_SILENT
            for (i = 0; i < SQ_NOTES; i++)
                notes[i] = 0;
            notes[0] = -1;
#endif
        }
        return;
    }
    if ((uint32_t)track >= SQ_TRACKS || notes[0] < 0)
        return;
    bit = 1u << track;
    last_in[track] = (uint8_t)notes[0];
    last_n[track]++;
    if (!((scale_mask | xpose_mask) & bit)) {
        last_out[track] = (uint8_t)notes[0];
        last_fx[track] = 0;
        return;
    }

    off = 0;
    if (xpose_mask & bit) {
        off = offset;
        if (track < KEY_TRACK && ahead > -1000)
            off = ahead;
    }
    root = notes[0] + off;
    n = (scale_mask & bit) ? remap(root) : root;
    last_fx[track] = (uint8_t)((off ? FX_XPOSE : 0) | (n != root ? FX_SCALE : 0));
    if (scale_mask & bit) {
        int32_t pc = root % 12;
        if (pc < 0)
            pc += 12;
        heard_pc = (uint8_t)pc;
        heard_out = map[pc];
        heard_n++;
    }
    n = clampi(n, 0, 127);
    last_out[track] = (uint8_t)n;

    if (track >= SQ_AUDIO) {                /* a MIDI chord: each tone, back as intervals */
        for (i = 1; i < SQ_NOTES; i++) {
            int32_t t;
            if (!notes[i])
                continue;
            t = root + notes[i];
            if (scale_mask & bit)
                t = remap(t);
            notes[i] = (int8_t)clampi(t - n, -127, 127);
        }
    }
    notes[0] = (int8_t)n;
}

/* ---- the overlay (UI task) --------------------------------------------------------------------- */
#define K_YES    12
#define K_UP     14
#define K_DOWN   15
#define K_LEFT   16
#define K_RIGHT  17
#define K_STEP1  24                         /* trig keys 1-16 are 24..39 */

/* Trig key (0-15) -> keyboard key 0..12 (C..the C above), FUNC + TRK's chromatic layout: trigs 9-16 are
 * the white keys, 2 3 5 6 7 the black ones, 1 4 8 nothing. */
static const int8_t STEP_NOTE[16] = {-1, 1, 3, -1, 6, 8, 10, -1, 0, 2, 4, 5, 7, 9, 11, 12};

static int32_t open_t = -1;                 /* the track it edits, -1 closed */
static int32_t page;                        /* the scale editor's knob screen, 0 (A-H: C..G) or 1 (A-D: G#..B) */
static int32_t sel;                         /* the scale editor's selected input pitch class */
static int32_t held = -1;                   /* the keyboard key held as the source, -1 none */
static int32_t func_held, swallow_trig;
static int32_t quant_open;                  /* FUNC + TRIG went to stock (Quantize), no key since */
static int32_t knob_acc[8];
static int32_t save_wait;

static int32_t active(void)
{
    uint32_t t = fw_active_track;
    return t < SQ_TRACKS ? (int32_t)t : 0;
}

static void close_overlay(void)
{
    open_t = -1;
    held = -1;
}

static void set_map(int32_t pc, int32_t out)
{
    map[pc] = (uint8_t)out;
    sel = pc;
    save_wait = 60;
}

/* The scale editor's keys: the trig keys are a keyboard (hold a source, press its output; a tap selects),
 * UP/DOWN flip the knob screen, LEFT/RIGHT move the selection, YES resets the selected note to itself,
 * FUNC + YES the whole table. Returns 1 for a key it takes. */
static int32_t editor_key(uint32_t k, uint32_t fl)
{
    int32_t press = (fl & FL_DOWN) && !(fl & FL_REPEAT), i;
    if (k >= K_STEP1 && k < K_STEP1 + 16) {
        int32_t n = STEP_NOTE[k - K_STEP1];
        if (n < 0 || (fl & FL_REPEAT))
            return 1;
        if (fl & FL_DOWN) {
            if (held >= 0 && n != held) {
                set_map(held % 12, n);
            } else {
                held = n;
                sel = n % 12;
            }
        } else if (n == held) {
            held = -1;
        }
        return 1;
    }
    switch (k) {
    case K_UP:
    case K_DOWN:
        if (press)
            page ^= 1;
        return 1;
    case K_LEFT:
    case K_RIGHT:
        if (fl & FL_DOWN)
            sel = (sel + (k == K_LEFT ? 11 : 1)) % 12;
        return 1;
    case K_YES:
        if (press) {
            if ((fl & FL_FUNC) || func_held) {
                for (i = 0; i < 12; i++)
                    set_map(i, i);
                sel = 0;
            } else {
                set_map(sel, sel);
            }
        }
        return 1;
    }
    return 0;
}

int32_t ks_key(void *brain, void *ev)
{
    uint32_t k = EV_ID(ev), fl = EV_FLAGS(ev);
    int32_t down = (fl & FL_DOWN) && !(fl & FL_REPEAT);
    (void)brain;

    if (k == K_FUNC) {
        if (!(fl & FL_REPEAT))
            func_held = (fl & FL_DOWN) != 0;
        return 0;
    }
    if (k != K_TRIG && down)
        quant_open = 0;                     /* any other key leaves (or was used in) Quantize */
    if (k == K_TRIG) {
        if (swallow_trig) {                 /* the rest of a TRIG press we took */
            if (!(fl & FL_DOWN))
                swallow_trig = 0;
            return 1;
        }
        if (!down)
            return 0;                       /* the rest of a press stock had */
        if ((fl & FL_FUNC) || func_held) {
            if (open_t >= 0) {              /* on ours: close it, stock's Quantize opens */
                close_overlay();
                quant_open = 1;
            } else if (!quant_open) {       /* the first press: stock's Quantize */
                quant_open = 1;
            } else {                        /* the second: ours, and stock gets it to leave Quantize */
                quant_open = 0;
                open_t = active();
                page = 0;
                held = -1;
            }
            return 0;
        }
        if (open_t >= 0) {
            swallow_trig = 1;
            if (open_t == KEY_TRACK)
                page ^= 1;
            return 1;
        }
        return 0;
    }
    if (open_t < 0)
        return 0;
    if (k == K_NO) {
        if (down)
            close_overlay();
        return 1;
    }
    if (open_t == KEY_TRACK && editor_key(k, fl))
        return 1;
    if (down)
        close_overlay();                    /* any other key closes it and does its own thing */
    return 0;
}

int32_t ks_enc(void *brain, void *ev)
{
    uint32_t e = EV_ID(ev);
    int32_t k, d;
    (void)brain;
    if (open_t < 0 || e < 1 || e > 8)
        return 0;
    k = (int32_t)e - 1;
    knob_acc[k] += EV_DELTA(ev);
    d = knob_acc[k] / COUNTS;
    knob_acc[k] -= d * COUNTS;
    if (!d)
        return 1;

    if (open_t == KEY_TRACK) {
        int32_t pc = page ? 8 + k : k;
        if (pc < 12) {
            set_map(pc, clampi(map[pc] + d, 0, 12));
        } else if (pc == 12) {              /* page 2, knob E: rotate the wrap note */
            wrap = (uint8_t)(((wrap + d) % 12 + 12) % 12);
            save_wait = 60;
        }
        return 1;
    } else if (k < 2) {
        volatile uint16_t *m = k == 0 ? &scale_mask : &xpose_mask;
        uint16_t bit = (uint16_t)(1u << open_t);
        *m = (uint16_t)(d > 0 ? *m | bit : *m & ~bit);
    } else {
        return 1;
    }
    save_wait = 60;
    return 1;
}

static const char *const NAMES[13] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
                                      "C'"};

static void track_name(char *buf, int32_t t)
{
    buf[0] = 'T';
    if (t + 1 >= 10) {
        buf[1] = '1';
        buf[2] = (char)('0' + (t + 1) % 10);
        buf[3] = 0;
    } else {
        buf[1] = (char)('1' + t);
        buf[2] = 0;
    }
}

/* Text at x, y in a box; on: the box inverted (lit, the text dark). font5 is 3 x 5 on a 4-pixel pitch. */
static void tag(void *bmp, int32_t x, int32_t y, const char *s, int32_t on)
{
    int32_t n = 0;
    while (s[n])
        n++;
    fw_textf(bmp, fw_font5, x, y, -1, "%s", s);
    if (on)
        fw_fillrect(bmp, x - 2, y - 2, x + 4 * n, y + 6, -1);
}

/* ---- the scale editor's keyboards (bmp is 128 x 64, y 0 the bottom row; rects are inclusive) ------ */
#define KB_X0   20                          /* the left edge of the first white key */
#define KB_W    12                          /* a white key's pitch: 8 of them end at x 116 */
#define IN_YB   32                          /* the input keyboard, y 32..51 */
#define IN_YT   51
#define OUT_YB  3                           /* the output keyboard, y 3..22; the wrap mark below it */
#define OUT_YT  22
#define LINK_Y  27                          /* the selection's link runs between them */
#define PG_X0   119                         /* the page strip on the right, x 119..127 */
#define PG_CX   123
#define STY_EMPTY 0
#define STY_FADE  1                         /* 50% vertical stripes */
#define STY_SOLID 2

/* A key's place: white keys at even numbers (2 x its white index), black keys at the odd one after the
 * white key on their left */
static const uint8_t KB_POS[13] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 14};

static int32_t kb_cx(int32_t k)
{
    int32_t p = KB_POS[k];
    return KB_X0 + (p >> 1) * KB_W + ((p & 1) ? KB_W : KB_W / 2);
}

/* Draws keyboard key k between yb and yt. Black keys must come after the white ones: each clears a
 * margin round itself. count > 1 is printed dark on a solid key; inv inverts the inside. */
static void draw_key(void *bmp, int32_t k, int32_t yb, int32_t yt, int32_t sty, int32_t count, int32_t inv)
{
    int32_t p = KB_POS[k], cx = kb_cx(k), x0, x1, y0, y1, x;
    if (p & 1) {
        fw_fillrect(bmp, cx - 4, yt - 12, cx + 4, yt - 1, 0);
        fw_framerect(bmp, cx - 3, yt - 11, cx + 3, yt, 1);
        x0 = cx - 2;
        x1 = cx + 2;
        y0 = yt - 10;
        y1 = yt - 1;
    } else {
        x0 = KB_X0 + (p >> 1) * KB_W;
        fw_framerect(bmp, x0, yb, x0 + KB_W, yt, 1);
        x1 = x0 + KB_W - 1;
        x0++;
        y0 = yb + 1;
        y1 = yt - 1;
    }
    if (count > 1) {
        fw_textf(bmp, fw_font5, cx - (count > 9 ? 3 : 1), y0 + 1, -1, "%d", (int)count);
        fw_fillrect(bmp, x0, y0, x1, y1, -1);
    } else if (sty == STY_SOLID) {
        fw_fillrect(bmp, x0, y0, x1, y1, 1);
    } else if (sty == STY_FADE) {
        for (x = x0 + 1; x <= x1; x += 2)
            fw_fillrect(bmp, x, y0, x, y1, 1);
    }
    if (inv)
        fw_fillrect(bmp, x0, y0, x1, y1, -1);
}

/* The last-heard mark on key k: a dot in a cleared halo, up in the key (white keys above the count,
 * black keys at their top) so it reads on any fill; big, a solid 5 x 5, while it flashes. */
static void heard_mark(void *bmp, int32_t k, int32_t yb, int32_t yt, int32_t big)
{
    int32_t cx = kb_cx(k), cy = (KB_POS[k] & 1) ? yt - 3 : yb + 11;
    fw_fillrect(bmp, cx - 2, cy - 2, cx + 2, cy + 2, big ? 1 : 0);
    if (!big)
        fw_fillrect(bmp, cx - 1, cy - 1, cx + 1, cy + 1, 1);
}

/* IN: the input notes, drawn as a keyboard (white keys empty, black solid); a remapped note is faded,
 * the selected one inverted. OUT: the outputs, solid where some input lands, empty where none does,
 * with the count printed on a note several inputs land on. A link runs from the selection to its output. */
static void draw_editor(void *bmp)
{
    uint8_t cnt[13];
    int32_t i, k, pass, a, b, lo, hi;
    for (i = 0; i < 13; i++)
        cnt[i] = 0;
    for (i = 0; i < 12; i++)
        cnt[map[i] % 13]++;

    for (pass = 0; pass < 2; pass++)        /* white keys, then black */
        for (k = 0; k < 13; k++) {
            int32_t pc = k % 12;
            if ((KB_POS[k] & 1) != pass)
                continue;
            draw_key(bmp, k, IN_YB, IN_YT, map[pc] != pc ? STY_FADE : pass ? STY_SOLID : STY_EMPTY, 0,
                     pc == sel);
            draw_key(bmp, k, OUT_YB, OUT_YT, cnt[k] ? STY_SOLID : STY_EMPTY, cnt[k], 0);
        }

    a = kb_cx(sel);
    b = kb_cx(map[sel] % 13);
    lo = a < b ? a : b;
    hi = a < b ? b : a;
    fw_fillrect(bmp, a, LINK_Y, a, IN_YB - 1, 1);
    fw_fillrect(bmp, lo, LINK_Y, hi, LINK_Y, 1);
    fw_fillrect(bmp, b, OUT_YT + 1, b, LINK_Y, 1);
    fw_fillrect(bmp, b - 1, OUT_YT + 2, b + 1, OUT_YT + 2, 1);     /* the arrowhead */
    fw_fillrect(bmp, b - 2, OUT_YT + 3, b + 2, OUT_YT + 3, 1);

    if (heard_n) {                          /* the last note through the scale, in and out */
        heard_mark(bmp, heard_pc, IN_YB, IN_YT, heard_fresh > 0);
        heard_mark(bmp, heard_out % 13, OUT_YB, OUT_YT, heard_fresh > 0);
    }

    for (k = 0; k < 13; k++)                /* the wrap mark: a caret under the window's start */
        if (k % 12 == wrap) {
            int32_t c = kb_cx(k);
            fw_fillrect(bmp, c, 2, c, 2, 1);
            fw_fillrect(bmp, c - 1, 1, c + 1, 1, 1);
            fw_fillrect(bmp, c - 2, 0, c + 2, 0, 1);
        }

    fw_textf(bmp, fw_font5, 2, 40, -1, "IN");
    fw_textf(bmp, fw_font5, 2, 13, -1, "OUT");
    fw_textf(bmp, fw_font5, 2, 3, -1, "W:%s", NAMES[wrap]);

    /* the page strip: an UP arrow, pages 1 and 2 (the current one inverted), a DOWN arrow */
    fw_fillrect(bmp, PG_CX, 50, PG_CX, 50, 1);
    fw_fillrect(bmp, PG_CX - 1, 49, PG_CX + 1, 49, 1);
    fw_fillrect(bmp, PG_CX - 2, 48, PG_CX + 2, 48, 1);
    fw_framerect(bmp, PG_X0, 36, 127, 45, 1);
    fw_textf(bmp, fw_font5, PG_CX - 1, 38, -1, "1");
    fw_framerect(bmp, PG_X0, 25, 127, 34, 1);
    fw_textf(bmp, fw_font5, PG_CX - 1, 27, -1, "2");
    if (page)
        fw_fillrect(bmp, PG_X0 + 1, 26, 126, 33, -1);
    else
        fw_fillrect(bmp, PG_X0 + 1, 37, 126, 44, -1);
    fw_fillrect(bmp, PG_CX - 2, 22, PG_CX + 2, 22, 1);
    fw_fillrect(bmp, PG_CX - 1, 21, PG_CX + 1, 21, 1);
    fw_fillrect(bmp, PG_CX, 20, PG_CX, 20, 1);

    fw_textf(bmp, fw_font5, 2, 56, -1, page ? "A-D:G#-B E:WRAP" : "A-H:C-G");
    fw_textf(bmp, fw_font5, 66, 56, -1, "%s>%s", NAMES[sel], NAMES[map[sel] % 13]);
    fw_textf(bmp, fw_font5, 98, 56, -1, "KEY%+d", (int)offset);
    fw_fillrect(bmp, 0, 53, 127, 63, -1);  /* the title bar, inverted */
}

void ks_draw(void *bmp, void *ctrl)
{
    int32_t t = open_t;
    (void)ctrl;
    if (t < 0)
        return;
    fw_fillrect(bmp, 0, 0, 127, 63, 0);

    if (t == KEY_TRACK) {
        draw_editor(bmp);
    } else {
        char name[4];
        uint32_t bit = 1u << t;
        fw_framerect(bmp, 1, 1, 126, 62, 1);
        track_name(name, t);
        fw_textf(bmp, fw_font5, 5, 53, -1, "%s APPLY", name);
        fw_textf(bmp, fw_font5, 80, 53, -1, "KEY %+d", (int)offset);
        fw_textf(bmp, fw_font5, 5, 38, -1, "A  SCALE");
        tag(bmp, 69, 38, (scale_mask & bit) ? "ON" : "OFF", (scale_mask & bit) != 0);
        fw_textf(bmp, fw_font5, 5, 24, -1, "B  TRANSPOSE");
        tag(bmp, 69, 24, (xpose_mask & bit) ? "ON" : "OFF", (xpose_mask & bit) != 0);
        /* the last fired note: in > out, and which of the two moved it (inverted when it did) */
        if (last_n[t]) {
            int32_t a = last_in[t], b = last_out[t];
            fw_textf(bmp, fw_font5, 5, 12, -1, "LAST %s%d>%s%d", NAMES[a % 12], (int)(a / 12),
                     NAMES[b % 12], (int)(b / 12));
        } else {
            fw_textf(bmp, fw_font5, 5, 12, -1, "LAST --");
        }
        tag(bmp, 69, 12, "TRN", (last_fx[t] & FX_XPOSE) != 0);
        tag(bmp, 89, 12, "SCL", (last_fx[t] & FX_SCALE) != 0);
        fw_textf(bmp, fw_font5, 5, 4, -1, "NO: CLOSE");
    }
}

/* ---- saving ------------------------------------------------------------------------------------- */
#ifdef SAVE_BLOCK
typedef int32_t (*emmc_fn)(int32_t block, int32_t bytes, void *buf);
#define EMMC_READ  ((emmc_fn)0x400e2c14)    /* 1.54, from FLEXMODS */
#define EMMC_WRITE ((emmc_fn)0x400e2dfa)
#define MAGIC 0x4b53434cu                   /* "KSCL" */
/* wrap came in 0.3, after the 0.2 fields with its own check (wrap ^ 0x5a), so a 0.2 save still loads */
struct save { uint32_t magic; uint8_t map[12]; uint16_t scale, xpose; uint32_t sum; uint8_t wrap, wrap_chk; };
static uint8_t block[512] __attribute__((aligned(4)));

static uint32_t sum_of(const struct save *s)
{
    uint32_t x = s->magic;
    int32_t i;
    for (i = 0; i < 12; i++)
        x = x * 31 + s->map[i];
    return x * 31 + ((uint32_t)s->scale << 16 | s->xpose);
}

static void load(void)
{
    struct save *s = (struct save *)block;
    int32_t i;
    if (EMMC_READ(SAVE_BLOCK, 512, block) != 0 || s->magic != MAGIC || s->sum != sum_of(s))
        return;
    for (i = 0; i < 12; i++)
        map[i] = (uint8_t)(s->map[i] <= 12 ? s->map[i] : i);
    scale_mask = s->scale;
    xpose_mask = s->xpose;
    if ((s->wrap ^ 0x5a) == s->wrap_chk && s->wrap < 12)
        wrap = s->wrap;
}

static void save(void)
{
    struct save *s = (struct save *)block;
    int32_t i;
    for (i = 0; i < 512; i++)
        block[i] = 0;
    s->magic = MAGIC;
    for (i = 0; i < 12; i++)
        s->map[i] = map[i];
    s->scale = scale_mask;
    s->xpose = xpose_mask;
    s->wrap = wrap;
    s->wrap_chk = (uint8_t)(wrap ^ 0x5a);
    s->sum = sum_of(s);
    EMMC_WRITE(SAVE_BLOCK, 512, block);
}
#endif

#ifdef SHOT
/* Screenshot builds for the README (cflags -DSHOT=1 or 2, never shipped): the overlay opens at boot with
 * a G minor remap, every note snapped down onto G A Bb C D Eb F (so the white keys play G minor), B > A#
 * selected. SHOT 1: the scale editor; SHOT 2: T10's apply screen. */
static void shot_setup(void)
{
    static const uint8_t GMIN[12] = {0, 0, 2, 3, 3, 5, 5, 7, 7, 9, 10, 10};
    static int32_t done;
    int32_t i;
    if (done)
        return;
    done = 1;
    for (i = 0; i < 12; i++)
        map[i] = GMIN[i];
    sel = 11;
    heard_pc = 4;                           /* an E went through the scale and came out Eb */
    heard_out = 3;
    heard_n = 1;
    last_in[9] = 64;                        /* T10: E5 in, Eb5 out */
    last_out[9] = 63;
    last_fx[9] = FX_SCALE;
    last_n[9] = 1;
    open_t = SHOT == 1 ? KEY_TRACK : 9;
}
#endif

/* ---- ev_tick (UI task) ------------------------------------------------------------------------- */
static uint32_t shown = ~0u;               /* the overlay state last drawn, ~0 after a close */
static uint8_t heard_seen;                  /* heard_n when the tick last looked */

void ks_tick(void *ctrl)
{
#ifdef SHOT
    shot_setup();
#endif
#ifdef SAVE_BLOCK
    static int32_t loaded, ticks;
#ifdef SHOT
    loaded = 1;                             /* a screenshot build neither loads nor saves */
    save_wait = 0;
#endif
    if (!loaded && ++ticks > 30) {
        loaded = 1;
        load();
    }
    if (save_wait > 0 && --save_wait == 0)
        save();
#else
    save_wait = 0;
#endif
    if (!PLAYING()) {                       /* stopped: the key goes back to 0 */
        offset = 0;
        key_last = -1;
        ahead = -1000;
        ahead_done = 0;
    }
    if (open_t < 0) {                       /* just closed: one redraw to clear the overlay */
        if (shown != ~0u)
            *((uint8_t *)ctrl + 0x20) = 1;
        shown = ~0u;
    } else {                                /* redraw when what the overlay shows changes */
        uint32_t sig = (uint32_t)open_t, i;
        sig = sig * 31 + (uint32_t)page;
        sig = sig * 31 + wrap;
        sig = sig * 31 + last_n[open_t];   /* the apply screen's LAST row follows the fires */
        sig = sig * 31 + last_fx[open_t];
        if (heard_n != heard_seen) {        /* a new note through the scale: flash its mark */
            heard_seen = heard_n;
            heard_fresh = HEARD_FLASH;
        } else if (heard_fresh > 0) {
            heard_fresh--;
        }
        sig = sig * 31 + heard_n;
        sig = sig * 31 + (heard_fresh > 0);
        sig = sig * 31 + (uint32_t)sel;
        sig = sig * 31 + (uint32_t)offset;
        sig = sig * 31 + ((uint32_t)scale_mask << 16 | xpose_mask);
        for (i = 0; i < 12; i++)
            sig = sig * 31 + map[i];
        if (sig != shown) {
            shown = sig;
            *((uint8_t *)ctrl + 0x20) = 1;
        }
    }
}
