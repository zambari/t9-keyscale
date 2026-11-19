### Details and limits

- **Sequenced trigs only.** Live playing and MIDI in are not transposed or scaled.
- The key goes back to 0 on stop. A chord on T9 uses its root.
- **Muting T9 holds the key**  The current key stays in force and T9's trigs
  are ignored until you unmute it. The overlay's title bar shows `HLD` in place of `KEY` while it holds.
  Stopping still resets the key to 0, muted or not.
- **Audio tracks and T9's speed:** audio tracks fire before T9 within a step. So that they don't lag one
  step behind a key change, they read T9's *next* stored step ahead of time, on the steps where T9's
  per-track speed says T9 moves on too. This is exact when T9's speed is a whole fraction of the audio
  track's (T9 at 1/2, 1/4 or 1/8 against 1x). With other ratios (T9 at 3/4x against 1x), an audio track
  can change key up to one of its own steps early or late. The look-ahead ignores T9's trig conditions
  for that step. MIDI targets (T10–T16) follow T9 exactly at any speed.
- **The per-track SCALE / TRANSPOSE switches are per pattern.** They're kept in the pattern's kit as **T9's CC4–CC8
  values** (FLTR page H, AMP page E–H), so they save, copy and reload with the pattern. Don't use those CC values on
  T9 for anything else. Stock may send them as CCs if T9's MIDI channel is on and CC4–CC8 have CC numbers, so keep T9's
  channel off (it's the key track anyway). A fresh pattern gives the defaults.
- The table, the wrap note, the on/off state, the user slots and which slot is active are global (not per pattern). They are saved
  to the +Drive's eMMC **block 0x6004e** about a second after the last change, and loaded at boot. That
  block is empty on a fresh drive, but that the stock OS never writes it is assumed, not proven.
  The mod claims the block as `emmc:0x6004e`, so elekloader refuses a mod set where another mod
  claims the same block.


  
## Building

Needs [elekloader](https://github.com/elekloader/elekloader) and an m68k cross GCC (see elekloader's
docs). Requires **core ≥ 4.1** and **seq-events** (the DT1 one).

```bash
python3 -m elekloader.sdk.build path/to/keyscale --stock Digitakt_OS1.54.syx
# -> path/to/keyscale/out/keyscale-<ver>.elemod
```

Load the `.elemod` into the [elekloader builder](https://elekloader.github.io/elekloader/) together with
core and seq-events, or link locally:

```bash
python3 -m elekloader.patch --stock Digitakt_OS1.54.syx \
    --mod mods/core/out/core-4.4-os1.54.elemod \
    --mod mods/seq-events-dt1/out/seq-events-0.1-os1.54.elemod \
    --mod path/to/keyscale/out/keyscale-<ver>.elemod \
    --out keyscale.syx --version 1.5K
```

The README screenshots are digiemu `--check` snapshots of a build with `-DSHOT=1` (scale editor;
`-DSHOT=2` is the apply screen) added to `cflags`. Those builds open the overlay at boot with the G minor table and never save. Don't ship them.

No firmware is in this repo: not the stock OS, and not built `.syx` or `.elemod` files.


### The scale editor (on T9) details

| control | does |
|---|---|
| knob **A** (page 1) | move the selection |
| knob **B** (page 1) | the output of the selected note (pick the note with **A**, the trig keys or LEFT/RIGHT) |
| knob **C** (page 1) | move the wrap note |
| knob **D** (page 1) | turn KeyScale **off** for all tracks (a bypass). The editor stays, drawn inverted, with its hints and pages; turn **D** up (on any page) to switch it back on |
| knobs **E–H** (page 1) | the output note of the last four keys (**G♯, A, A♯, B** with wrap C). A knob's first turn only selects its note, so a stray touch doesn't edit; keep turning to change it |
| **DOWN** (or **TRIG**) / **UP** | the next / previous of three pages. Page 2: knobs **A–H** for the first eight keys (**C, C♯, D, D♯, E, F, F♯, G** with wrap C). Page 3: the slots (below) |
| knob **push** | **unlock** a note for editing. Every note starts locked at power-on; editing a locked one (with its knob, **B** or the trig keys) shows `LOCKED` in the title bar instead. Push the note's own knob (this also selects it), or push **B** for the selected note. Notes stay unlocked until power-off. YES and a double tap still reset locked notes |
| trig keys | a keyboard in FUNC + TRK's layout (9–15 white keys, 2 3 5 6 7 black). **Hold a source note and press its new output** to set it. A tap only selects; a **double tap** resets that note to itself |
| **LEFT / RIGHT** | move the selection |
| **YES** | reset the whole table (a double tap on a trig key resets one note). FUNC + YES is left to stock's quick save |

An output stays near its input, in a window that runs from the **wrap note** up to the wrap note
an octave above (C to C by default, marked under OUT and shown as `W:`). Both keyboards are drawn
from the wrap note, and the note knobs follow that drawn order: with wrap G, page 2's A–H are
G..D and page 1's E–H are D♯..F♯. An output on the wrap
note itself goes to whichever end is nearer the input, so with wrap C, B → C snaps B up to the next
C, and D → C goes down to the C below.

## Firmware findings (DT1 OS 1.54)

What KeyScale needed to know about the Digitakt mk1's RAM, written down for other mod authors. **Every address
and offset is for OS 1.54 only** and is likely to move in any other OS version. "Seen" means found in digiemu by
diffing the pattern and kit in RAM before and after one edit at a time (a small probe mod, `patdiff`, not part of
this repo). "Disasm" means read from the 1.54 code. "Assumed" means used by KeyScale but not proven.

### Global addresses

| what | address | source | notes |
|---|---|---|---|
| pattern pointer | `*(u8**)0x4199ec38` | seq-events | the pattern the sequencer plays: 16 tracks × 911 bytes, pattern-level data after them |
| transport state | `u32 @ 0x4199ec2c` | seen | 1 = playing |
| `EMMC_READ` / `EMMC_WRITE` | `0x400e2c14` / `0x400e2dfa` | FLEXMODS | raw +Drive block access |
| mute singleton | `*(void**)0x421fab90` | disasm | see "Mutes" |

### Pattern track struct (911 bytes, track *n* at `pattern + n × 911`)

| offset | type | what | source |
|---|---|---|---|
| `0x100 + step` | s8 | microtiming per step, in ticks (`0xff` = one tick early) | seen |
| `0x280 + step` | s8[64] | note lock 1 (`-1` = the track's NOTE) | seq-events |
| `0x382` | u16 | the default trigs (`+1 & 0x80` = default SYN) | seq-events |
| `0x384` | u8 | the track's NOTE | seq-events |
| `0x38b` | u8 | per-track speed: 0 2x, 1 3/2x, 2 1x, 3 3/4x, 4 1/2x, 5 1/4x, 6 1/8x | seen |

Open: in the *normal* (not per-track) scale mode, `0x38b` may hold a stale per-track value.

### Kit (`fw_kit + off`, one per pattern)

A MIDI track's parameters are 16-bit words, **`0x70` bytes per MIDI track** (T9 at the offsets below, T10 at
`+0x70`, and so on). All seen, from T9 and T10.

| offset (T9) | what |
|---|---|
| `0x5bc` | SRC CHAN (0 = off) |
| `0x5be` | SRC BANK |
| `0x5c0` | SRC PROG |
| `0x5c2` | SRC SBNK (0 is shown as 1) |
| `0x5c4` / `0x5c5` | SRC PB (14 bits) |
| `0x5c6` | SRC AT |
| `0x5c8` | SRC MW |
| `0x5ca` | SRC BC |
| `0x5cc..0x5d2` | FLTR page A–D: the CC1–CC4 numbers |
| `0x5d4..0x5da` | FLTR page E–H: the CC1–CC4 values |
| `0x5dc..0x5e2` | AMP page A–D: the CC5–CC8 numbers |
| `0x5e4..0x5ea` | AMP page E–H: the CC5–CC8 values |

- A CC number is stored as number + `0x20` (CC9 is `0x29`).
- KeyScale assumes a 7-bit CC value is the word's high byte (the byte at the even address). That's not confirmed
  yet, and neither is that a fresh pattern's values read 0.
- A byte at `0x609` looked like a bitmask that enables each message type, but it behaved erratically.
- Unknown: how "--" (no CC number) is encoded.

### Mutes (disasm)

The handler for MIDI CC 94 is at `0x400d6bd4`, and it calls `0x40020008`. `S = *(void**)0x421fab90` is a singleton
that's built lazily, so check it for null. `S + 48` is an object whose vtable slot 40 (`0x40141628`) returns its
field `+16`, which is the mute state `M = *(void**)(S + 64)`.

| `M +` | what |
|---|---|
| 28 | u32 mask that CC 94 sets and clears; bit = seq track, set = muted |
| 32 | a twin mask with the same kind of setter (`0x4001d776`); assumed to be the pattern mutes |
| 220 | a mode; CC 94 does nothing while it's 2 |

Mutes live outside the pattern, so a pattern diff never sees them.

### Sequencer behaviour (seen, with seq-events 0.1)

- `ev_fire_step` runs for all 16 tracks every step, in order 0 → 15: the audio tracks T1–T8, then the MIDI tracks
  T9–T16. Note fires come in ascending order too, so audio tracks fire before T9 within a step.
- `ev_fire_notes` still runs for muted tracks; the mute is applied later, at the voice or the MIDI out.
- Changing `notes[0]` on an audio track really changes the sample's pitch (heard in the digiemu GUI).
- `ev_fire_notes` runs at interrupt level, so keep its handlers short.

### eMMC

On a fresh digiemu `plusdrive.img`, `0x60000..0x6005f` is all zero. FLEXMODS uses blocks `0x60040..0x6004c`, and
KeyScale uses `0x6004e`. That only shows the blocks are empty on a fresh drive, not that the stock OS never
writes them.
