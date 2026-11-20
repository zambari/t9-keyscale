/* SPDX-License-Identifier: GPL-2.0-or-later */
/* T9-KeyScale for the Digitakt mk1 (OS 1.54, core 4.1, seq-events 0.1).
 *
 * The key track (KEY_TRACK, T9 = seq track 8) sets the key: offset = its note - 60 (C5). Its note
 * trigs are read in ev_fire_notes and still played (KEY_SILENT 0): silence T9 with its SRC channel.
 * KEY_SILENT 1 drops them instead (notes[0] = -1). The offset holds until the key track's next note
 * trig, and goes back to 0 on stop. While T9 is muted (global or pattern mute) the offset holds: its
 * trigs don't change the key (the title bar shows HLD for KEY).
 *
 * bypass (the scale editor's knob D, page 1) turns transpose and scale off for every track at once; the
 * key is still followed, so it is in step when switched back on.
 *
 * Every fired note of a target track goes through:
 *     note + offset            if the track's TRANSPOSE is on
 *     then the scale remap     if its SCALE is on (the switches are per pattern, see get_masks): note - pc + map[pc], folded into the
 *                              input's window
 *     then a clamp to 0..127
 * map[pc] is the output pitch class for input pitch class pc, 0..11. The window is the octave from the
 * wrap note (wrap, 0..11, C by default) at or below the input to the wrap note above it; an output
 * outside it moves by octaves into it, and an output on the wrap note's pitch class takes whichever
 * end is nearer the input (with wrap C: B -> C the C above, D -> C the C below; a tie goes down).
 * With wrap G, C -> B gives the B below C (G..G holds it), and G -> F# the F# at the window's top,
 * almost an octave up. A MIDI chord's tones are remapped one by one and handed back as
 * intervals from the remapped root; a tone that lands on the root becomes interval 0, which seq-events
 * reads as "no tone" (a duplicate dropped).
 *
 * Audio tracks fire before T9 within a step, so on the step where the key changes they would get the
 * old key. LOOKAHEAD: an audio track's step load works out from both tracks' per-track speeds whether
 * T9 loads a step later in the same loop; if so it reads T9's next stored step from the pattern and,
 * if that holds a note trig, uses its key for this step. Exact when T9's speed is a whole fraction of
 * the audio track's (1/2, 1/4, 1/8 against 1x); it ignores T9's conditions (a failed condition still
 * moves the audio tracks for that one step; the MIDI targets follow the real fire).
 *
 * UI: FUNC + TRIG opens the overlay on the active track first (stock never sees that press);
 * FUNC + TRIG on the overlay closes it and stock gets the press, so its Quantize opens. FUNC + TRIG
 * in Quantize (no other key between) opens the overlay again (stock gets that press too, to leave
 * Quantize), so the two alternate.
 *   on KEY_TRACK: the scale editor, two drawn keyboards running from the wrap note: IN (a remapped
 *                 note underlined, the line a pixel short at the end it leaves: the left for up, the
 *                 right for down; the selection inverted) above OUT (drawn like IN: white keys dark, black lit; an X where no input lands, a count on
 *                 the notes several inputs land on, the selection's output inverted), an arrow from the selection to its output, a
 *                 mark under OUT at the wrap note, and a dot on the last note any track sent through
 *                 the scale (its input after transpose on IN, its output on OUT; dark on a lit
 *                 key), taller for a moment after it plays. The trig keys are a keyboard in FUNC + TRK's layout
 *                 (9-16 white, 2 3 5 6 7 black): hold a source and press its output to set it; a tap
 *                 only selects, a double tap resets that note to itself. LEFT/RIGHT move the selection; YES closes the
 *                 overlay, like NO. The note knobs follow the drawn order (from the wrap note; C..B by
 *                 default). Page 1: A moves the selection, B sets the selected note's output, C rotates the
 *                 wrap note, D turns T9-KeyScale off (hinted right of OUT), E-H set the outputs of the last
 *                 four keys (G#..B) (bypass: the keyboards and title bar go, leaving only D's hint, OFF lit, and the pages; going off locks every note again; D on any page turns it
 *                 back on, UP/DOWN/TRIG still flip pages, nothing else does anything). DOWN (or TRIG without FUNC) steps to the next page, UP to the one before, of three: page 2 is
 *                 A-H for the first eight keys (C..G). A knob's first turn only selects its note, unless it is already selected.
 *                 Every note starts locked at power-on: an edit of a locked note's output (its own knob or
 *                 a trig-key pair; B, the cursor's value, is never locked) does nothing and shows PRESS TO UNLOCK at the left of the title bar (over the slot name) for 40 UI ticks. Pushing
 *                 a note's knob unlocks (and selects) it, pushing B unlocks the selected note; it stays
 *                 unlocked until power-off or until T9-KeyScale is turned off. A double tap ignores the locks. FUNC + YES is left to stock (quick save) on every screen. A strip on the right
 *                 shows the page between UP and DOWN arrows.
 *                 Page 3, the slots: four user slots U1-U4 and the presets CMAJ, GMIN (every other note a
 *                 semitone down onto the scale, wrap C) and INIT (every note to itself, wrap C: the reset),
 *                 each a table and a wrap note. UP/DOWN pick one,
 *                 LEFT/RIGHT switch LOAD / SAVE, YES loads it into the live table or saves the live table
 *                 into it (not a preset), makes it the active slot and goes back to page 1. UP on the top
 *                 slot goes back to page 2. Loading the
 *                 active slot again is the revert. The title bar shows the active slot, with a dot after it
 *                 once the live table or wrap note differs from it. Knobs and trig keys do
 *                 nothing on page 3; leave it with TRIG or NO (YES there does the slot).
 *   on any other: knob A SCALE on/off, knob B TRANSPOSE on/off for that track (turn, or push to flip;
 *                 one row of toggles with the track's name on the right; under
 *                 it the semitones each moved the last note, then T9's status with a hint), kept in the pattern's kit
 *                 (T9's CC4-CC8 values).
 * FUNC + trig N switches the overlay to track N (trig 9 the scale editor) without leaving it; the
 * firmware's active track stays as it was (the DT1 SDK has no track select). While FUNC is held the
 * editor's keyboard does nothing, and stock gets no FUNC + trig while the overlay is open.
 * NO closes it; any other key but FUNC closes it and then does what it does.
 *
 * Saving: the table, the wrap note, the on/off, the user slots and the active slot are kept in eMMC block SAVE_BLOCK (mod.json: 0x6004e, claimed as
 * emmc:0x6004e), FLEXMODS' scheme: loaded ~30 ticks after boot, saved 60 ticks after the last change.
 * The block is empty on a fresh drive; that stock never writes it is assumed, not proven.
 *
 * Interrupt level: ks_step and ks_notes (table lookups only). UI task: the rest. */
#include <stdint.h>
#include "digitakt-mk1/core3.h"
#include "digitakt-mk1/seq.h"

#define KEY_TRACK  8                        /* T9; the probe saw tracks fire 0..15 in order */
#define KEY_ROOT   60                       /* C5 (MIDI 60): the key note that means offset 0 */
#define KEY_SILENT 0                        /* 1: drop the key track's notes (else silence T9 by SRC/mute) */
#define LOOKAHEAD  1

/* ---- keys and knobs (digiemu's measured key map, devices/digitakt.toml) -------------------------- */
#define K_FUNC   1
#define K_NO     13
#define K_TRIG   19                         /* the TRIG page key; FUNC + TRIG is stock Quantize */
#define K_PUSH_A 40                         /* the encoder pushes, A..H = 40..47 */
#define FL_DOWN   0x01
#define FL_FUNC   0x02
#define FL_REPEAT 0x08
#define EV_ID(ev)    (*(uint32_t *)((char *)(ev) + 12))
#define EV_FLAGS(ev) (*(uint32_t *)((char *)(ev) + 16))
#define EV_DELTA(ev) (*(int32_t *)((char *)(ev) + 16))
#define COUNTS   10                         /* raw encoder counts per step: higher = less sensitive */
#define ACC_IDLE 15                         /* UI ticks without a turn that drop a knob's leftover counts */

/* ---- the pattern (seq-events-dt1/dev.h) ------------------------------------------------------- */
#define PATTERN      (*(const uint8_t *const volatile *)PATTERN_PP)
#define TRACK_STRIDE 911
#define TR_NOTE_LOCK 0x280                  /* s8[64]: note lock 1, -1 = the track's NOTE */
#define TR_DEF_TRIGS 0x382                  /* u16: the default trigs; +1 & 0x80 = default SYN */
#define TR_NOTE      0x384
#define TR_SPEED     0x38b                  /* u8: the per-track speed, 0 2x .. 2 1x .. 6 1/8x (patdiff) */
#define W_TRIG       0x0001
#define W_NEVER      0x0004                 /* a step with this bit never fires */
#define W_SYN        0x0080
#define W_SYN_SET    0x0800
#define IN_DDR(p)    ((uint32_t)(p) >= 0x40000000u && (uint32_t)(p) < 0x48000000u)

#define PLAYING() (*(volatile uint32_t *)TRANSPORT_AT == 1)

/* ---- the mutes (1.54 disassembly: MIDI CC 94's handler at 0x400d6bd4 -> 0x40020008) ---------------
 * MUTES_PP holds a lazily built singleton S. S + 48 is an object whose vtable slot 40 (0x40141628)
 * returns its field +16, the mute state M: u32 at M + 28 is the mask CC 94 sets and clears (bit = seq
 * track, set = muted); M + 32 is a twin mask with the same setter shape (0x4001d776), taken to be the
 * pattern mutes. M + 220 is a mode (CC 94 does nothing while it is 2). Either mask counts as muted. */
#define MUTE_STATE_OFF 64                   /* S + 48 + 16 */
#define MUTE_A         28
#define MUTE_B         32

static uint32_t key_muted(void)
{
    const uint8_t *s = *(const uint8_t *const volatile *)MUTES_PP, *m;
    if (!IN_DDR(s))
        return 0;
    m = *(const uint8_t *const volatile *)(s + MUTE_STATE_OFF);
    if (!IN_DDR(m))
        return 0;
    return ((*(const volatile uint32_t *)(m + MUTE_A) | *(const volatile uint32_t *)(m + MUTE_B)) >>
            KEY_TRACK) & 1;
}

/* ---- settings (global) ------------------------------------------------------------------------- */
static volatile uint8_t map[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static volatile uint8_t wrap;               /* the remap window's start, a pitch class (0 = C..C) */
static volatile uint8_t bypass;             /* 1: transpose and scale off on every track */

/* ---- the per-track switches (per pattern): the key track's CC4-CC8 values in the kit -----------------
 * The kit (fw_kit) is per pattern. A MIDI track's parameters are 16-bit words, 0x70 bytes per track; T9's
 * SRC page (CHAN BANK PROG SBNK PB AT MW BC) starts at 0x5bc, then CC1-4 (FLTR: A-D the CC numbers, E-H the
 * values) and CC5-8 (AMP, the same). A 7-bit value is the word's high byte, the byte at its even address
 * (assumed, as for the audio sounds' words; the low byte is left alone).
 * The switches are stored as their difference from the defaults (SCALE and TRANSPOSE on for T10-T16, off
 * for T1-T8), so a fresh pattern's zeros read as the defaults:
 *     CC4  SCALE     T10-T16 (bit 0 = T10)      CC6  SCALE     T1-T7 (bit 0 = T1)
 *     CC5  TRANSPOSE T10-T16                    CC7  TRANSPOSE T1-T7
 *     CC8  T8: bit 0 SCALE, bit 1 TRANSPOSE
 * Their CC numbers are left alone: if T9's channel is on and CC4-CC8 have numbers, stock may send the
 * values as CCs. */
#define KEY_KIT     (0x5bc + 0x70 * (KEY_TRACK - 8))
#define CC_VAL(n)   (KEY_KIT + ((n) <= 4 ? 0x18 + 2 * ((n) - 1) : 0x28 + 2 * ((n) - 5)))
#define DEF_MASK    0xfe00u                 /* T10-T16 */

static void get_masks(uint32_t *sc, uint32_t *xp)
{
    const uint8_t *k = fw_kit;
    uint32_t v4 = 0, v5 = 0, v6 = 0, v7 = 0, v8 = 0;
    if (IN_DDR(k)) {
        v4 = k[CC_VAL(4)] & 0x7f;
        v5 = k[CC_VAL(5)] & 0x7f;
        v6 = k[CC_VAL(6)] & 0x7f;
        v7 = k[CC_VAL(7)] & 0x7f;
        v8 = k[CC_VAL(8)] & 3;
    }
    *sc = (DEF_MASK ^ v4 << 9) | v6 | (v8 & 1) << 7;
    *xp = (DEF_MASK ^ v5 << 9) | v7 | (v8 >> 1 & 1) << 7;
}

static void set_masks(uint32_t sc, uint32_t xp)
{
    uint8_t *k = fw_kit;
    if (!IN_DDR(k))
        return;
    k[CC_VAL(4)] = (uint8_t)((sc ^ DEF_MASK) >> 9 & 0x7f);
    k[CC_VAL(5)] = (uint8_t)((xp ^ DEF_MASK) >> 9 & 0x7f);
    k[CC_VAL(6)] = (uint8_t)(sc & 0x7f);
    k[CC_VAL(7)] = (uint8_t)(xp & 0x7f);
    k[CC_VAL(8)] = (uint8_t)((sc >> 7 & 1) | (xp >> 7 & 1) << 1);
}

/* ---- playback state ------------------------------------------------------------------------------ */
static volatile int32_t offset;             /* the key, from the key track's last fired note */
static volatile int32_t ahead[SQ_AUDIO];   /* LOOKAHEAD: the key for an audio track's step, -1000 none */
static volatile int32_t since_key[SQ_AUDIO];   /* an audio track's step loads since the key track's last */
static volatile int32_t key_last = -1;      /* the key track's last loaded step, -1 after stop */

/* Each track's last fired note, for the apply screen: how far transpose and the scale moved it (in
 * semitones), what moved it, and a count of fires (0: none yet) */
#define FX_XPOSE 1
#define FX_SCALE 2
static volatile int8_t last_dx[SQ_TRACKS], last_ds[SQ_TRACKS];
static volatile uint8_t last_fx[SQ_TRACKS], last_n[SQ_TRACKS];
/* The key track's notes, for the apply screen's status line: the last one, a count, and whether one
 * came since PLAY */
static volatile uint8_t key_note, key_n, key_got;
static int32_t key_fresh;                   /* UI ticks left of the key dot's flash after a new note */
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
    if (out == base || out == base + 12)    /* on the wrap note: the one nearer the input */
        out = rel <= 6 ? base : base + 12;
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

/* A track's step length from its per-track speed (TR_SPEED, measured with the patdiff probe:
 * 0 2x, 1 3/2x, 2 1x, 3 3/4x, 4 1/2x, 5 1/4x, 6 1/8x), in units where a 1x step is 48 */
static const int16_t STEP_LEN[7] = {24, 32, 48, 64, 96, 192, 384};

static int32_t step_len(int32_t track)
{
    const uint8_t *pat = PATTERN;
    uint32_t s;
    if (!IN_DDR(pat))
        return 48;
    s = pat[TRACK_STRIDE * track + TR_SPEED];
    return s < 7 ? STEP_LEN[s] : 48;
}

/* ---- ev_fire_step (interrupt level) ------------------------------------------------------------ */
int32_t ks_step(int32_t track, int32_t step, int32_t len)
{
    (void)len;
    if (track == KEY_TRACK) {
        int32_t i;
        key_last = step;
        for (i = 0; i < SQ_AUDIO; i++)
            since_key[i] = 0;
    }
#if LOOKAHEAD
    /* An audio track's step loads before the key track's in the same loop. The key track loads in
     * this loop too when the time since its last load (this track's loads since then, times its step
     * length) has reached the key track's step length; only then does this step take the key track's
     * next stored note. Otherwise it keeps the key in force, so a slow key track doesn't move the
     * audio targets early. Exact for whole ratios (T9 at 1/2, 1/4, 1/8 against 1x); with others
     * (3/4x against 1x) a step can still change key up to one audio step late or early. */
    else if ((uint32_t)track < SQ_AUDIO) {
        struct sq_track info;
        int32_t next = 0, n = ++since_key[track];
        ahead[track] = -1000;
        if (key_muted()) {
            /* the key track is muted: the key holds, nothing to look ahead at */
        } else if (key_last < 0) {                 /* the first step after PLAY: every track is at 0 */
            ahead[track] = stored_key(0);
        } else if (n * step_len(track) >= step_len(KEY_TRACK)) {
            if (sq_track_info(KEY_TRACK, &info, sizeof info) == 0 && info.len > 0)
                next = (key_last + 1) % info.len;
            ahead[track] = stored_key(next);
        }
    }
#endif
    return step;
}

/* ---- ev_fire_notes (interrupt level) ----------------------------------------------------------- */
void ks_notes(int32_t track, int32_t step, int8_t notes[SQ_NOTES])
{
    uint32_t bit, scale_mask, xpose_mask;
    int32_t off, root, n, i;
    (void)step;
    if (track == KEY_TRACK) {
        if (notes[0] >= 0) {
            key_note = (uint8_t)notes[0];
            key_n++;
            key_got = 1;
            if (!key_muted())               /* muted: the key holds */
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
    get_masks(&scale_mask, &xpose_mask);
    last_n[track]++;
    if (bypass || !((scale_mask | xpose_mask) & bit)) {
        last_dx[track] = 0;
        last_ds[track] = 0;
        last_fx[track] = 0;
        return;
    }

    off = 0;
    if (xpose_mask & bit) {
        off = offset;
        if (track < SQ_AUDIO && ahead[track] > -1000)
            off = ahead[track];
    }
    root = notes[0] + off;
    n = (scale_mask & bit) ? remap(root) : root;
    last_fx[track] = (uint8_t)((off ? FX_XPOSE : 0) | (n != root ? FX_SCALE : 0));
    last_dx[track] = (int8_t)clampi(off, -99, 99);
    last_ds[track] = (int8_t)(n - root);
    if (scale_mask & bit) {
        int32_t pc = root % 12;
        if (pc < 0)
            pc += 12;
        heard_pc = (uint8_t)pc;
        heard_out = map[pc];
        heard_n++;
    }
    n = clampi(n, 0, 127);

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

/* Trig key (0-15) -> keyboard key 0..11 (C..B), FUNC + TRK's chromatic layout: trigs 9-15 are the
 * white keys, 2 3 5 6 7 the black ones, 1 4 8 16 nothing. */
static const int8_t STEP_NOTE[16] = {-1, 1, 3, -1, 6, 8, 10, -1, 0, 2, 4, 5, 7, 9, 11, -1};

static int32_t open_t = -1;                 /* the track it edits, -1 closed */
static int32_t page;                        /* the scale editor's knob screen, 0 (A-D the utilities, E-H the
                                               last 4 keys), 1 (A-H: the first 8 keys) or PG_SLOTS */
static int32_t sel;                         /* the scale editor's selected input pitch class */
static int32_t held = -1;                   /* the keyboard key held as the source, -1 none */
static int32_t func_held, swallow_trig;
static uint16_t trig_taken;                 /* bit n: trig key n's press went to a FUNC + trig switch */
static int32_t quant_open;                  /* FUNC + TRIG went to stock (Quantize), no key since */
#define PG_SLOTS 2                          /* page 3: the slots */
static int32_t knob_acc[8];                 /* raw counts toward the next step, per knob */
static uint32_t knob_at[8];                 /* ui_ticks at each knob's last turn */
static int32_t save_wait;
static uint32_t ui_ticks;                   /* ks_tick's count, for the double tap and knob idle */
static int32_t tap_key = -1;
static uint32_t tap_at;        /* the last plain tap on the keyboard and when */
#define DTAP_TICKS 20                       /* a second tap of the same key within this resets it */
static uint16_t unlocked;                   /* per input pitch class: its output may be edited (RAM only,
                                               so every note starts locked at power-on) */
static int32_t locked_msg;                  /* UI ticks left of the title bar's PRESS TO UNLOCK */
#define LOCK_MSG 40

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

/* An edit of pc's output: done if pc is unlocked, otherwise PRESS TO UNLOCK shows for a while */
static void edit_map(int32_t pc, int32_t out)
{
    if (unlocked & (1u << pc))
        set_map(pc, out);
    else
        locked_msg = LOCK_MSG;
}

/* ---- slots: four user slots and the presets, each a table and a wrap note ----------------------- */
#define NSLOTS   4
#define NPRESETS 3
#define NALL     (NSLOTS + NPRESETS)
/* INIT is last so a saved active slot (cur) from before it keeps its index */
static const char *const SLOT_NAMES[NALL] = {"U1", "U2", "U3", "U4", "CMAJ", "GMIN", "INIT"};
/* every note outside the scale snapped a semitone down onto it; wrap C */
static const uint8_t PRESETS[NPRESETS][12] = {
    {0, 0, 2, 2, 4, 5, 5, 7, 7, 9, 9, 11},  /* C major */
    {0, 0, 2, 3, 3, 5, 5, 7, 7, 9, 10, 10}, /* G minor: G A Bb C D Eb F */
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, /* INIT: every note to itself, the power-on table */
};
static uint8_t slots[NSLOTS][12], slot_wraps[NSLOTS];
static uint8_t slot_used;                   /* bit s: user slot s holds a table */
static int32_t cur = -1;                    /* the slot last loaded or saved, -1 none */
static int32_t slot_sel;                    /* page 3's highlighted slot */
static int32_t slot_save;                   /* page 3's mode: 0 LOAD, 1 SAVE */
static uint32_t slot_gen;                   /* bumped on a slot save, for the redraw */

/* Slot s's table, or 0 for an empty user slot */
static const uint8_t *slot_map(int32_t s)
{
    if (s >= NSLOTS)
        return PRESETS[s - NSLOTS];
    return (slot_used >> s) & 1 ? slots[s] : 0;
}

static int32_t slot_wrap(int32_t s)
{
    return s < NSLOTS ? slot_wraps[s] : 0;
}

/* 1 when the live table or wrap note differs from the active slot */
static int32_t slot_dirty(void)
{
    const uint8_t *m;
    int32_t i;
    if (cur < 0)
        return 0;
    m = slot_map(cur);
    if (!m || slot_wrap(cur) != wrap)
        return 1;
    for (i = 0; i < 12; i++)
        if (m[i] != map[i])
            return 1;
    return 0;
}

static void set_page(int32_t p)
{
    page = p;
    held = -1;
    if (p == PG_SLOTS) {                    /* page 3 opens on the active slot, in LOAD */
        slot_sel = cur >= 0 ? cur : 0;
        slot_save = 0;
    }
}

/* YES on page 3: load the highlighted slot into the live table, or save the live table into it (user
 * slots only). Either makes it the active slot and goes back to page 1; an empty slot or a preset in
 * SAVE does nothing. */
static void slot_do(void)
{
    int32_t s = slot_sel, i;
    if (slot_save) {
        if (s >= NSLOTS)
            return;
        for (i = 0; i < 12; i++)
            slots[s][i] = map[i];
        slot_wraps[s] = wrap;
        slot_used |= (uint8_t)(1u << s);
        slot_gen++;
    } else {
        const uint8_t *m = slot_map(s);
        if (!m)
            return;
        for (i = 0; i < 12; i++)
            map[i] = m[i];
        wrap = (uint8_t)slot_wrap(s);
    }
    cur = s;
    save_wait = 60;
    set_page(0);
}

/* Page 3's keys: UP/DOWN pick a slot, LEFT/RIGHT switch LOAD/SAVE, YES does it. The trig keys do
 * nothing here (FUNC + YES goes to stock before this). */
static int32_t slots_key(uint32_t k, uint32_t fl)
{
    int32_t press = (fl & FL_DOWN) && !(fl & FL_REPEAT);
    if (k >= K_STEP1 && k < K_STEP1 + 16)
        return 1;
    switch (k) {
    case K_UP:
    case K_DOWN:
        if (!(fl & FL_DOWN))
            return 1;
        if (k == K_UP && slot_sel == 0) {   /* UP on the top slot: back to page 2 (not on a held repeat) */
            if (!(fl & FL_REPEAT))
                set_page(1);
        } else {
            slot_sel = (slot_sel + (k == K_UP ? NALL - 1 : 1)) % NALL;
        }
        return 1;
    case K_LEFT:
    case K_RIGHT:
        if (press)
            slot_save ^= 1;
        return 1;
    case K_YES:
        if (press && !(fl & FL_FUNC) && !func_held)
            slot_do();
        return 1;
    }
    return 0;
}

/* The scale editor's keys: the trig keys are a keyboard (hold a source, press its output; a tap selects),
 * UP/DOWN flip the knob screen, LEFT/RIGHT move the selection (YES closes it, in ks_key).
 * Returns 1 for a key it takes. */
static int32_t editor_key(uint32_t k, uint32_t fl)
{
    int32_t press = (fl & FL_DOWN) && !(fl & FL_REPEAT);
    if (bypass) {                           /* off: only UP/DOWN flip pages; the rest do nothing, but stay ours */
        if ((k == K_UP || k == K_DOWN) && press)
            set_page((page + (k == K_DOWN ? 1 : 2)) % 3);
        return (k >= K_STEP1 && k < K_STEP1 + 16) || k == K_UP || k == K_DOWN || k == K_LEFT ||
               k == K_RIGHT;
    }
    if (page == PG_SLOTS)
        return slots_key(k, fl);
    if (k >= K_STEP1 && k < K_STEP1 + 16) {
        int32_t n = STEP_NOTE[k - K_STEP1];
        if (n < 0 || (fl & FL_REPEAT))
            return 1;
        if (fl & FL_DOWN) {
            if (held >= 0 && n != held) {
                edit_map(held % 12, n);
                tap_key = -1;               /* a source/target pair is no tap */
            } else if (n == tap_key && ui_ticks - tap_at <= DTAP_TICKS) {
                set_map(n % 12, n % 12);    /* a double tap: the note back to itself */
                tap_key = -1;
                held = -1;
            } else {
                held = n;
                sel = n % 12;
                tap_key = n;
                tap_at = ui_ticks;
            }
        } else if (n == held) {
            held = -1;
        }
        return 1;
    }
    switch (k) {
    case K_UP:
    case K_DOWN:
        if (press)                          /* DOWN: the next page, UP: the one before */
            set_page((page + (k == K_DOWN ? 1 : 2)) % 3);
        return 1;
    case K_LEFT:
    case K_RIGHT:
        if (fl & FL_DOWN)
            sel = (sel + (k == K_LEFT ? 11 : 1)) % 12;
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
            if (open_t >= 0) {              /* on ours: close it, stock gets it and opens Quantize */
                close_overlay();
                quant_open = 1;
                return 0;
            }
            open_t = active();
            set_page(0);
            if (quant_open) {               /* in Quantize: ours, and stock gets it to leave Quantize */
                quant_open = 0;
                return 0;
            }
            swallow_trig = 1;               /* the first press: ours only, stock never sees it */
            return 1;
        }
        if (open_t >= 0) {
            swallow_trig = 1;
            if (open_t == KEY_TRACK)
                set_page((page + 1) % 3);
            return 1;
        }
        return 0;
    }
    if (k >= K_STEP1 && k < K_STEP1 + 16) {
        uint16_t bit = (uint16_t)(1u << (k - K_STEP1));
        if (trig_taken & bit) {             /* the rest of a press a FUNC + trig switch took */
            if (!(fl & FL_DOWN))
                trig_taken &= (uint16_t)~bit;
            return 1;
        }
        if (open_t >= 0 && down && ((fl & FL_FUNC) || func_held)) {
            int32_t t = (int32_t)(k - K_STEP1);     /* FUNC + trig: the overlay moves to that track */
            trig_taken |= bit;
            held = -1;
            if (t == KEY_TRACK && open_t != KEY_TRACK)
                set_page(0);
            open_t = t;
            return 1;
        }
    }
    if (open_t < 0)
        return 0;
    if (k == K_NO) {
        if (down)
            close_overlay();
        return 1;
    }
    if (k == K_YES && ((fl & FL_FUNC) || func_held))
        return 0;                           /* FUNC + YES is stock's quick save: pass it, stay open */
    if (k == K_YES && !(open_t == KEY_TRACK && page == PG_SLOTS && !bypass)) {
        if (down)                           /* YES closes it like NO (page 3 keeps YES for its slots) */
            close_overlay();
        return 1;
    }
    if (k >= K_PUSH_A && k < K_PUSH_A + 8) {    /* the encoder pushes are ours while it is open */
        int32_t kk = (int32_t)(k - K_PUSH_A);
        if (!down)
            return 1;
        if (open_t != KEY_TRACK) {          /* the apply screen: push A / B flips SCALE / TRANSPOSE */
            if (kk < 2) {
                uint32_t sc, xp, bit = 1u << open_t;
                get_masks(&sc, &xp);
                if (kk == 0)
                    sc ^= bit;
                else
                    xp ^= bit;
                set_masks(sc, xp);
            }
        } else if (!bypass && page != PG_SLOTS) {
            /* the scale editor: a push unlocks a note for editing, B's the selected one, a note
             * knob's its own (and selects it) */
            if (!page && kk < 4) {
                if (kk == 1)
                    unlocked |= (uint16_t)(1u << sel);
            } else {
                int32_t pc = (wrap + (page ? kk : 4 + kk)) % 12;
                unlocked |= (uint16_t)(1u << pc);
                sel = pc;
            }
            locked_msg = 0;
        }
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
    d = EV_DELTA(ev);
    /* start a fresh step after a pause or a reversal, so leftover counts never tip a later nudge */
    if (ui_ticks - knob_at[k] > ACC_IDLE || (d ^ knob_acc[k]) < 0)
        knob_acc[k] = 0;
    knob_at[k] = ui_ticks;
    knob_acc[k] += d;
    d = knob_acc[k] / COUNTS;
    knob_acc[k] -= d * COUNTS;
    if (!d)
        return 1;

    if (open_t == KEY_TRACK) {
        /* the note knobs follow the keyboards' drawn order, which starts at the wrap note: page 1's
         * E-H are its last four keys, page 2's A-H its first eight */
        int32_t util = !page && k < 4, pc = (wrap + (page ? k : 4 + k)) % 12;
        if ((!page || bypass) && k == 3) { /* knob D (page 1, or any page while off): on/off */
            if (d < 0 && !bypass) {         /* going off locks every note again */
                unlocked = 0;
                locked_msg = 0;
            }
            bypass = d < 0;
            save_wait = 60;
        } else if (bypass) {
            /* off: the table's knobs do nothing */
        } else if (page == PG_SLOTS) {
            /* the slots page: no knobs */
        } else if (util && k == 0) {        /* page 1, A: the selection */
            sel = ((sel + d) % 12 + 12) % 12;
        } else if (util && k == 1) {        /* B: the selected note's output */
            /* endless: rolls past B into C and on round the keyboard; never locked */
            set_map(sel, ((map[sel] + d) % 12 + 12) % 12);
        } else if (util && k == 2) {        /* C: rotate the wrap note */
            wrap = (uint8_t)(((wrap + d) % 12 + 12) % 12);
            save_wait = 60;
        } else if (pc != sel) {             /* the first turn of a note's knob only selects it */
            sel = pc;
        } else {
            edit_map(pc, clampi(map[pc] + d, 0, 11));
        }
        return 1;
    } else if (k < 2) {                     /* into the pattern's kit, not the global save */
        uint32_t sc, xp, *m = k == 0 ? &sc : &xp, bit = 1u << open_t;
        get_masks(&sc, &xp);
        *m = d > 0 ? *m | bit : *m & ~bit;
        set_masks(sc, xp);
    }
    return 1;
}

static const char *const NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

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
#define KB_W    11                          /* a white key's pitch: 7 of them end at x 97 (a black last key
                                               reaches x 101) */
#define IN_YB   32                          /* the input keyboard, y 32..51 */
#define IN_YT   51
#define OUT_YB  3                           /* the output keyboard, y 3..22; the wrap mark below it */
#define OUT_YT  22
#define LINK_Y  27                          /* the selection's link runs between them */
#define PG_X0   119                         /* the page strip on the right, x 119..127 */
#define PG_CX   123
#define HINT_X  102                         /* page 1's knob hints, clear of a black last key */

static const uint8_t IS_BLACK[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
static int16_t kb_left[12], kb_c[12];       /* per pitch class: a white key's left edge; a key's centre */

/* Lays the keyboards out from the wrap note to the one below the next wrap note: white keys side by
 * side, a black key centred on the edge between its neighbours (a black first key hangs off the left) */
static void kb_layout(void)
{
    int32_t i, w = 0;
    for (i = 0; i < 12; i++) {
        int32_t pc = (wrap + i) % 12, x = KB_X0 + w * KB_W;
        if (IS_BLACK[pc]) {
            kb_c[pc] = (int16_t)x;
        } else {
            kb_left[pc] = (int16_t)x;
            kb_c[pc] = (int16_t)(x + KB_W / 2);
            w++;
        }
    }
}

/* Which way input pitch class pc moves in the window: 1 up, -1 down, 0 not remapped */
static int32_t map_dir(int32_t pc)
{
    int32_t n = 60 + pc;                    /* any octave will do: the window moves with it */
    if (map[pc] == pc)
        return 0;
    return remap(n) > n ? 1 : -1;
}

/* Draws keyboard key k between yb and yt (kb_layout first). Black keys must come after the white ones:
 * each clears a margin round itself. count > 1 is printed dark on a solid key; inv inverts the inside.
 * dir != 0 (a remapped note): a line one pixel above the key's bottom, a pixel short at the end it
 * leaves (left for up, right for down), dark on a solid key. */
static void draw_key(void *bmp, int32_t k, int32_t yb, int32_t yt, int32_t solid, int32_t count, int32_t inv,
                     int32_t dir)
{
    int32_t cx = kb_c[k], x0, x1, y0, y1;
    if (IS_BLACK[k]) {
        fw_fillrect(bmp, cx - 4, yt - 12, cx + 4, yt - 1, 0);
        fw_framerect(bmp, cx - 3, yt - 11, cx + 3, yt, 1);
        x0 = cx - 2;
        x1 = cx + 2;
        y0 = yt - 10;
        y1 = yt - 1;
    } else {
        x0 = kb_left[k];
        fw_framerect(bmp, x0, yb, x0 + KB_W, yt, 1);
        x1 = x0 + KB_W - 1;
        x0++;
        y0 = yb + 1;
        y1 = yt - 1;
    }
    if (count > 1) {                        /* lit on an empty key, dark on a solid one */
        fw_textf(bmp, fw_font5, cx - (count > 9 ? 3 : 1), y0 + 1, -1, "%d", (int)count);
        if (solid)
            fw_fillrect(bmp, x0, y0, x1, y1, -1);
    } else if (solid) {
        fw_fillrect(bmp, x0, y0, x1, y1, 1);
    }
    if (dir)
        fw_fillrect(bmp, x0 + (dir > 0), y0 + 1, x1 - (dir < 0), y0 + 1, solid ? 0 : 1);
    if (inv)
        fw_fillrect(bmp, x0, y0, x1, y1, -1);
}

/* The last-heard mark on key k: a 3 x 3 dot up in the key (white keys above the count, black keys at
 * their top), 3 x 5 while it flashes. lit: the key's inside is drawn lit (or striped), so the dot is
 * dark there and lit on an empty key. 3 wide so it stays between the black keys' frames. */
static void heard_mark(void *bmp, int32_t k, int32_t yb, int32_t yt, int32_t big, int32_t lit)
{
    int32_t cx = kb_c[k], cy = IS_BLACK[k] ? yt - 3 : yb + 11, r = big ? 2 : 1;
    fw_fillrect(bmp, cx - 1, cy - r, cx + 1, cy + r, lit ? 0 : 1);
}

/* A 5 x 5 X on OUT key k, which no input lands on: white keys low down (below the black keys), black
 * keys in the middle of their inside. lit: the key's inside is lit, so the X is dark. */
static void dead_mark(void *bmp, int32_t k, int32_t yb, int32_t yt, int32_t lit)
{
    int32_t cx = kb_c[k], cy = IS_BLACK[k] ? yt - 6 : yb + 4, d;
    for (d = -2; d <= 2; d++) {
        fw_fillrect(bmp, cx + d, cy + d, cx + d, cy + d, lit ? 0 : 1);
        fw_fillrect(bmp, cx + d, cy - d, cx + d, cy - d, lit ? 0 : 1);
    }
}

/* Both keyboards run from the wrap note. IN: the input notes (white keys empty, black solid); a
 * remapped note is underlined (the line points the way it moves), the selected one inverted. OUT: the
 * outputs, drawn like IN with only the selection's output inverted, an X where no input lands, the count printed on a note
 * several inputs land on. A link runs from the selection to its output. */
/* Page 3: the slots top to bottom (the highlighted one inverted, the active one marked on the left), each
 * with its scale as 12 cells C..B (a block where some input lands, a dot where none does) and its wrap
 * note; LOAD / SAVE on the right, and what YES will do at the bottom. */
static void draw_slots(void *bmp)
{
    int32_t s, i;
    for (s = 0; s < NALL; s++) {
        int32_t y = 46 - 6 * s;             /* 7 rows, 46..10, clear of the status line at y 1 */
        const uint8_t *m = slot_map(s);
        int32_t n = 0;
        while (SLOT_NAMES[s][n])
            n++;
        fw_textf(bmp, fw_font5, 4, y, -1, "%s", SLOT_NAMES[s]);
        if (s == slot_sel)                  /* tag's box is 9 tall, too tall for a 6-pixel pitch */
            fw_fillrect(bmp, 2, y - 1, 4 + 4 * n, y + 5, -1);
        if (s == cur)
            fw_fillrect(bmp, 0, y + 1, 1, y + 3, 1);
        if (!m) {
            fw_textf(bmp, fw_font5, 26, y, -1, "EMPTY");
            continue;
        }
        for (i = 0; i < 12; i++) {
            int32_t j, x = 26 + 4 * i, used = 0;
            for (j = 0; j < 12; j++)
                used |= m[j] == i;
            if (used)
                fw_fillrect(bmp, x, y + 1, x + 2, y + 3, 1);
            else
                fw_fillrect(bmp, x + 1, y + 1, x + 1, y + 1, 1);
        }
        fw_textf(bmp, fw_font5, 76, y, -1, "%s", NAMES[slot_wrap(s) % 12]);
    }
    tag(bmp, 95, 45, "LOAD", !slot_save);
    tag(bmp, 95, 36, "SAVE", slot_save);
    if (slot_save && slot_sel >= NSLOTS)
        fw_textf(bmp, fw_font5, 4, 1, -1, "PRESETS ARE READ ONLY");
    else if (!slot_save && !slot_map(slot_sel))
        fw_textf(bmp, fw_font5, 4, 1, -1, "%s IS EMPTY", SLOT_NAMES[slot_sel]);
    else
        fw_textf(bmp, fw_font5, 4, 1, -1, "YES: %s %s", slot_save ? "SAVE TO" : "LOAD",
                 SLOT_NAMES[slot_sel]);
}

/* Page 1's utility knobs, right of OUT. While T9-KeyScale is off only D's OFF shows, lit: the others do nothing then. */
static void draw_hints(void *bmp)
{
    if (!bypass) {
        fw_textf(bmp, fw_font5, HINT_X, 18, -1, "A SEL");
        fw_textf(bmp, fw_font5, HINT_X, 12, -1, "B MAP");
        fw_textf(bmp, fw_font5, HINT_X, 6, -1, "C W:%s", NAMES[wrap]);
    }
    fw_textf(bmp, fw_font5, HINT_X, 0, -1, "D OFF");
    if (bypass)                             /* lit under OFF only (tag's box would reach below y 0) */
        fw_fillrect(bmp, HINT_X + 6, 0, HINT_X + 20, 6, -1);
}

static void draw_keyboards(void *bmp)
{
    uint8_t cnt[12];
    int32_t i, k, pass, a, b, lo, hi, tgt = map[sel] % 12;
    kb_layout();
    for (i = 0; i < 12; i++)
        cnt[i] = 0;
    for (i = 0; i < 12; i++)
        cnt[map[i] % 12]++;

    for (pass = 0; pass < 2; pass++)        /* white keys, then black */
        for (k = 0; k < 12; k++) {
            if (IS_BLACK[k] != pass)
                continue;
            draw_key(bmp, k, IN_YB, IN_YT, pass, 0, k == sel, map_dir(k));
            /* OUT like IN (white keys dark, black lit), only the selection's output inverted */
            draw_key(bmp, k, OUT_YB, OUT_YT, pass, cnt[k], k == tgt, 0);
            if (!cnt[k])
                dead_mark(bmp, k, OUT_YB, OUT_YT, pass != (k == tgt));
        }

    a = kb_c[sel];
    b = kb_c[tgt];
    lo = a < b ? a : b;
    hi = a < b ? b : a;
    fw_fillrect(bmp, a, LINK_Y, a, IN_YB - 1, 1);
    fw_fillrect(bmp, lo, LINK_Y, hi, LINK_Y, 1);
    fw_fillrect(bmp, b, OUT_YT + 1, b, LINK_Y, 1);
    fw_fillrect(bmp, b - 1, OUT_YT + 2, b + 1, OUT_YT + 2, 1);     /* the arrowhead */
    fw_fillrect(bmp, b - 2, OUT_YT + 3, b + 2, OUT_YT + 3, 1);

    if (heard_n) {                          /* the last note through the scale, in and out */
        int32_t pc = heard_pc % 12, o = heard_out % 12;
        /* IN: a black key is lit, and the selection inverts it */
        int32_t lit = IS_BLACK[pc] != (pc == sel);
        heard_mark(bmp, pc, IN_YB, IN_YT, heard_fresh > 0, lit);
        /* OUT: likewise, the selection's output inverting it */
        heard_mark(bmp, o, OUT_YB, OUT_YT, heard_fresh > 0, IS_BLACK[o] != (o == tgt));
    }

    {                                       /* the wrap mark: a caret under the window's start */
        int32_t c = kb_c[wrap];
        fw_fillrect(bmp, c, 2, c, 2, 1);
        fw_fillrect(bmp, c - 1, 1, c + 1, 1, 1);
        fw_fillrect(bmp, c - 2, 0, c + 2, 0, 1);
    }

    fw_textf(bmp, fw_font5, 2, 40, -1, "IN");
    fw_textf(bmp, fw_font5, 2, 13, -1, "OUT");
    fw_textf(bmp, fw_font5, 2, 3, -1, "W:%s", NAMES[wrap]);

    if (!page)
        draw_hints(bmp);
}

/* the page strip: an UP arrow, pages 1-3 (the current one inverted), a DOWN arrow */
static void draw_pages(void *bmp)
{
    int32_t p;
    fw_fillrect(bmp, PG_CX, 50, PG_CX, 50, 1);
    fw_fillrect(bmp, PG_CX - 1, 49, PG_CX + 1, 49, 1);
    fw_fillrect(bmp, PG_CX - 2, 48, PG_CX + 2, 48, 1);
    for (p = 0; p < 3; p++) {
        int32_t y = 39 - 9 * p;            /* boxes y 39..46, 30..37, 21..28 */
        fw_framerect(bmp, PG_X0, y, 127, y + 7, 1);
        fw_textf(bmp, fw_font5, PG_CX - 1, y + 1, -1, "%d", (int)(p + 1));
        if (p == page)
            fw_fillrect(bmp, PG_X0 + 1, y + 1, 126, y + 6, -1);
    }
    fw_fillrect(bmp, PG_CX - 2, 19, PG_CX + 2, 19, 1);
    fw_fillrect(bmp, PG_CX - 1, 18, PG_CX + 1, 18, 1);
    fw_fillrect(bmp, PG_CX, 17, PG_CX, 17, 1);
}

static void draw_editor(void *bmp)
{
    if (page == PG_SLOTS)
        draw_slots(bmp);
    else
        draw_keyboards(bmp);
    draw_pages(bmp);

    if (locked_msg > 0)                     /* an edit of a locked note: push its knob (or B) first; it takes
                                               the slot name's place, x 2..61 */
        fw_textf(bmp, fw_font5, 2, 56, -1, "PRESS TO UNLOCK");
    else if (cur >= 0) {                         /* the active slot, a dot after it once the live table differs */
        int32_t n = 0;
        while (SLOT_NAMES[cur][n])
            n++;
        fw_textf(bmp, fw_font5, 2, 56, -1, "%s", SLOT_NAMES[cur]);
        if (slot_dirty())
            fw_fillrect(bmp, 3 + 4 * n, 57, 4 + 4 * n, 58, 1);
    }
    fw_textf(bmp, fw_font5, 74, 56, -1, "%s>%s", NAMES[sel], NAMES[map[sel] % 12]);
    fw_textf(bmp, fw_font5, 98, 56, -1, "%s%s%d", key_muted() ? "HLD" : "KEY", offset > 0 ? "+" : "",
             (int)offset);
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
        if (bypass) {                       /* off: no keyboards or title bar, just D's hint (OFF lit) and pages */
            draw_hints(bmp);
            draw_pages(bmp);
        } else {
            draw_editor(bmp);
        }
    } else {
        char name[4];
        uint32_t bit = 1u << t, scale_mask, xpose_mask;
        int32_t off = offset;
        const char *hint = 0;
        get_masks(&scale_mask, &xpose_mask);
        fw_framerect(bmp, 1, 1, 126, 62, 1);
        track_name(name, t);
        /* row 1: the two switches (knob A scale, knob B transpose), the track on the right */
        fw_textf(bmp, fw_font5, 5, 53, -1, "SCALE");
        tag(bmp, 29, 53, (scale_mask & bit) ? "ON" : "OFF", (scale_mask & bit) != 0);
        fw_textf(bmp, fw_font5, 49, 53, -1, "TRANS");
        tag(bmp, 73, 53, (xpose_mask & bit) ? "ON" : "OFF", (xpose_mask & bit) != 0);
        fw_textf(bmp, fw_font5, t + 1 >= 10 ? 111 : 115, 53, -1, "%s", name);
        /* row 2: how far each moved this track's last fired note, under its switch */
        fw_textf(bmp, fw_font5, 5, 43, -1, "LAST");
        if (last_n[t]) {
            int32_t ds = last_ds[t], dx = last_dx[t];
            fw_textf(bmp, fw_font5, 29, 43, -1, "%s%d", ds > 0 ? "+" : "", (int)ds);
            fw_textf(bmp, fw_font5, 73, 43, -1, "%s%d", dx > 0 ? "+" : "", (int)dx);
        } else {
            fw_textf(bmp, fw_font5, 29, 43, -1, "--");
            fw_textf(bmp, fw_font5, 73, 43, -1, "--");
        }
        fw_fillrect(bmp, 2, 37, 125, 37, 1);

        /* the key track's status, and what to check when the key isn't moving */
        if (bypass) {
            fw_textf(bmp, fw_font5, 5, 29, -1, "ALL OFF: NOTHING IS MOVED");
            hint = "T9 OVERLAY, D: BACK ON";
        } else if (!PLAYING()) {
            fw_textf(bmp, fw_font5, 5, 29, -1, "STOPPED  KEY 0");
            hint = "PLAY: T9 NOTES SET THE KEY";
        } else if (key_muted()) {
            fw_textf(bmp, fw_font5, 5, 29, -1, "T9 MUTED  HLD %s%d", off > 0 ? "+" : "", (int)off);
            hint = "UNMUTE T9 TO FOLLOW IT";
        } else if (!key_got) {
            fw_textf(bmp, fw_font5, 5, 29, -1, "NO NOTES FROM T9");
            hint = "ADD NOTE TRIGS ON T9";
        } else {
            int32_t k = key_note;
            fw_framerect(bmp, 5, 29, 7, 31, 1);              /* flashes on each T9 note */
            if (key_fresh > 0)
                fw_fillrect(bmp, 5, 29, 7, 31, 1);
            fw_textf(bmp, fw_font5, 11, 29, -1, "T9 %s%d  KEY %s%d", NAMES[k % 12], (int)(k / 12),
                     off > 0 ? "+" : "", (int)off);
            hint = "C5 ON T9 = KEY 0";
        }
        fw_textf(bmp, fw_font5, 5, 20, -1, "%s", hint);
        /* bottom: a dot per track in two rows of 8 like the trig keys (T1..T8 above, T9..T16 below),
         * filled where SCALE or TRANS is on, a pixel where both are off; this track is framed */
        {
            uint32_t on = scale_mask | xpose_mask;
            int32_t i;
            for (i = 0; i < 16; i++) {
                int32_t x = 13 + (i & 7) * 14, y = i < 8 ? 13 : 5;
                if (on & (1u << i))
                    fw_fillrect(bmp, x, y, x + 2, y + 2, 1);
                else
                    fw_fillrect(bmp, x + 1, y + 1, x + 1, y + 1, 1);
                if (i == t)
                    fw_framerect(bmp, x - 2, y - 2, x + 4, y + 4, 1);
            }
        }
    }
}

/* ---- saving ------------------------------------------------------------------------------------- */
#ifdef SAVE_BLOCK
typedef int32_t (*emmc_fn)(int32_t block, int32_t bytes, void *buf);
#define EMMC_READ  ((emmc_fn)0x400e2c14)    /* 1.54, from FLEXMODS */
#define EMMC_WRITE ((emmc_fn)0x400e2dfa)
#define MAGIC 0x4b53434cu                   /* "KSCL" */
/* wrap came in 0.3, after the 0.2 fields with its own check (wrap ^ 0x5a), so a 0.2 save still loads;
 * bypass likewise in 0.6 (bypass ^ 0xa5; an older save reads as on) */
/* The slots came in 0.11, after those, with their own sum (an older save has zeros there: no slots) */
#define SLOT_MAGIC 0x534c4f54u              /* "SLOT", seeds slot_sum */
struct save {
    uint32_t magic; uint8_t map[12]; uint16_t scale, xpose; uint32_t sum; uint8_t wrap, wrap_chk;
    uint8_t bypass, bypass_chk;
    uint8_t slots[NSLOTS][12], slot_wraps[NSLOTS], slot_used, cur, pad[2]; uint32_t slot_sum;
};

static uint32_t slot_sum_of(const struct save *s)
{
    uint32_t x = SLOT_MAGIC;
    const uint8_t *p = &s->slots[0][0];
    int32_t i;
    for (i = 0; i < NSLOTS * 13 + 2; i++)   /* slots, slot_wraps, slot_used, cur */
        x = x * 31 + p[i];
    return x;
}
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
        map[i] = (uint8_t)(s->map[i] <= 12 ? s->map[i] % 12 : i);
    /* s->scale, s->xpose: the switches before 0.12, now per pattern in the kit; ignored */
    if ((s->wrap ^ 0x5a) == s->wrap_chk && s->wrap < 12)
        wrap = s->wrap;
    if ((s->bypass ^ 0xa5) == s->bypass_chk && s->bypass < 2)
        bypass = s->bypass;
    if (s->slot_sum != slot_sum_of(s))
        return;
    slot_used = s->slot_used & ((1u << NSLOTS) - 1);
    for (i = 0; i < NSLOTS; i++) {
        int32_t j, ok = s->slot_wraps[i] < 12;
        for (j = 0; j < 12; j++)
            ok &= s->slots[i][j] < 12;
        if (!ok)
            slot_used &= (uint8_t)~(1u << i);
        if (!((slot_used >> i) & 1))
            continue;
        for (j = 0; j < 12; j++)
            slots[i][j] = s->slots[i][j];
        slot_wraps[i] = s->slot_wraps[i];
    }
    cur = s->cur < NALL && slot_map(s->cur) ? s->cur : -1;
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
    s->wrap = wrap;
    s->wrap_chk = (uint8_t)(wrap ^ 0x5a);
    s->bypass = bypass;
    s->bypass_chk = (uint8_t)(bypass ^ 0xa5);
    s->sum = sum_of(s);
    for (i = 0; i < NSLOTS; i++) {
        int32_t j;
        for (j = 0; j < 12; j++)
            s->slots[i][j] = slots[i][j];
        s->slot_wraps[i] = slot_wraps[i];
    }
    s->slot_used = slot_used;
    s->cur = (uint8_t)(cur < 0 ? 0xff : cur);
    s->slot_sum = slot_sum_of(s);
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
    cur = NSLOTS + 1;                       /* the GMIN preset */
    heard_pc = 4;                          /* an E went through the scale and came out Eb */
    heard_out = 3;
    heard_n = 1;
    last_dx[9] = 0;                         /* T10: E5 in, Eb5 out */
    last_ds[9] = -1;
    last_fx[9] = FX_SCALE;
    last_n[9] = 1;
    open_t = SHOT == 1 ? KEY_TRACK : 9;
}
#endif

/* ---- ev_tick (UI task) ------------------------------------------------------------------------- */
static uint32_t shown = ~0u;               /* the overlay state last drawn, ~0 after a close */
static uint8_t heard_seen;                  /* heard_n when the tick last looked */
static uint8_t key_seen;                    /* key_n likewise */

void ks_tick(void *ctrl)
{
#ifdef SHOT
    shot_setup();
#endif
    ui_ticks++;
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
        int32_t i;
        offset = 0;
        key_last = -1;
        key_got = 0;
        for (i = 0; i < SQ_AUDIO; i++) {
            ahead[i] = -1000;
            since_key[i] = 0;
        }
    }
    if (open_t < 0) {                       /* just closed: one redraw to clear the overlay */
        if (shown != ~0u)
            *((uint8_t *)ctrl + 0x20) = 1;
        shown = ~0u;
    } else {                                /* redraw when what the overlay shows changes */
        uint32_t sig = (uint32_t)open_t, i;
        sig = sig * 31 + (uint32_t)page;
        sig = sig * 31 + wrap;
        sig = sig * 31 + bypass;
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
        if (key_n != key_seen) {            /* a new key track note: flash the status dot */
            key_seen = key_n;
            key_fresh = HEARD_FLASH;
        } else if (key_fresh > 0) {
            key_fresh--;
        }
        sig = sig * 31 + key_n;
        sig = sig * 31 + (key_fresh > 0);
        sig = sig * 31 + key_got;
        sig = sig * 31 + (PLAYING() != 0);
        if (locked_msg > 0)
            locked_msg--;
        sig = sig * 31 + (locked_msg > 0);
        sig = sig * 31 + unlocked;
        sig = sig * 31 + (uint32_t)sel;
        sig = sig * 31 + (uint32_t)cur;
        sig = sig * 31 + (uint32_t)slot_sel;
        sig = sig * 31 + (uint32_t)slot_save;
        sig = sig * 31 + slot_gen;
        sig = sig * 31 + (uint32_t)offset;
        sig = sig * 31 + key_muted();
        {
            uint32_t sc, xp;
            get_masks(&sc, &xp);
            sig = sig * 31 + (sc << 16 | xp);
        }
        for (i = 0; i < 12; i++)
            sig = sig * 31 + map[i];
        if (sig != shown) {
            shown = sig;
            *((uint8_t *)ctrl + 0x20) = 1;
        }
    }
}
