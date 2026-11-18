# PAL voices for CTR 3D

`ctr_pal_voices.py` copies the French, German, Italian, Spanish and Dutch speech out of an image of
**your own** PAL Crash Team Racing disc (SCES-02105) into the folder the game reads,
`sdmc:/3ds/ctr_native/mods/pal-voices/`. The game can do the same itself from the VOICES tab; this is
for extracting on a PC instead.

Needs Python 3.8 or newer, nothing else (the window uses Tkinter, which ships with Python on Windows
and macOS; on Linux it may be a separate `python3-tk` package).

## With the window

Run `ctr_pal_voices.py` (double click it, or `python ctr_pal_voices.py`):

1. Pick your PAL image (`.bin`, raw 2352-byte sectors; a cooked `.iso` drops the voice sectors).
2. Pick where the voices go: the SD card's root (they go straight to `3ds/ctr_native/mods/pal-voices`),
   the `ctr_native` folder, or any other folder (then copy the `pal-voices` folder it makes to
   `3ds/ctr_native/mods/` on the SD card).
3. Tick the languages and press Extract. Languages already extracted there are greyed out.

## From a terminal

```
python ctr_pal_voices.py IMAGE.bin DESTINATION [--lang FRN,GRM,ITL,SPN,DCH]
```

`--lang` defaults to all five. Files already extracted with the right size are skipped, so it can be
run again after an interrupted copy.

## What it writes

```
pal-voices/XA/<CODE>.XNF                  track table (data bytes)
pal-voices/XA/<CODE>/EXTRA/Sxx.XA         speech, raw 2352-byte sectors
pal-voices/XA/<CODE>/GAME/Sxx.XA
```

About 34 MB per language. Each file is written as `.part` and renamed when complete, and each
language's `.XNF` goes last, so the game never takes a half-copied language as installed. The output
is byte for byte what the game's own extractor writes.

Nothing from the disc is part of this repository or sent anywhere: the tool only reads your image
and writes where you tell it.
