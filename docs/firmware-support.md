# Running PS5VR on other firmware

PS5VR ships export tables for **13.60** and **7.20**, both tested on hardware.

| Firmware | Headset / tracker | Hand tracking |
|---|---|---|
| 13.60 | Export tables, tested on hardware | Export table and model import patch |
| 7.20 | Export tables, tested on hardware  | No export table supplied |

The 7.20 Hmd2 library does not export `sceHmd2InternalImageOpenBySlot` or
`sceHmd2InternalLedDetectorOpenBySlot`. These camera diagnostics are optional
and do not prevent headset playback. Gaze and vibration exports are present.
Experimental hand tracking is not covered by the 7.20 tables.

This page also explains how to add another firmware using its own libraries.

## Why there is a table at all

The PS VR2 system libraries (`libSceHmd2`, `libSceVrTracker2`, `libSceVrHand`)
keep the functions PS5VR needs out of their dynamic symbol tables, so there is
nothing to `sceKernelDlsym`. The app calls them at fixed offsets inside the
library instead, and those offsets are specific to one build of one firmware.

Each table carries an *anchor*: a string that must sit at a known offset in
the library's read-only data. If it
does not match, that table's offsets are not used. The app tries named exports
with `sceKernelDlsym` and stops VR setup if a required function is unavailable.
Missing optional camera, gaze or vibration functions do not stop playback.

Each module selects its own matching table. The tracker base comes from the
loader's module information, and the 13.60 hand-model import patch is used only
when both the hand and tracker libraries match their 13.60 anchors.

## Why a 13.60 console cannot produce another firmware's table

The offsets are read out of the decrypted library itself. A console only has
its own firmware's copy, so a 13.60 machine can only ever generate the 13.60
table. Supporting 12.40 needs the library from a 12.40 console.

## What is needed from you

Two decrypted libraries from a console on your firmware are needed for headset
playback:

- `libSceHmd2.sprx`
- `libSceVrTracker2.sprx`

For experimental hand tracking, also supply `libSceVrHand.sprx` if it exists on
your firmware.

They live in `/system/common/lib/` and are encrypted on disk. Decrypt them
with `ps5-self-pager`, which is what the tooling here expects as input.

Open an issue saying which firmware you are on and attach them, or the
generated table if you would rather run the generator yourself:

```sh
python3 tools/gen_export_table.py --suffix <fw> --names app/src/vr_exports_1360.h \
    libSceHmd2.sprx      sceHmd2Initialize sceHmd2Open ... \
    -- libSceVrTracker2.sprx  sceVrTracker2Initialize ... \
    -- libSceVrHand.sprx      sceVrHandInitialize ... \
  > app/src/vr_exports_<fw>.h
```

Run `python3 tools/ps5_dynlib.py <lib.sprx>` first to list what a library
exports on your firmware. The names PS5VR needs are the ones already listed in
`app/src/vr_exports_1360.h`. Use a suffix without punctuation (`720` for 7.20)
so multiple tables can coexist. Pass `--suffix` and `--names` before the first
library. `--names` resolves the requested names by their NIDs without requiring
a pre-existing `tools/names.txt`. A missing required export must be investigated;
never copy its offset from a different firmware.

Include the new header in `app/src/app_vr.c` and register each generated library
in `kExportProfiles` with its firmware label and suffix. A generated header
alone does not register a firmware.

Please only send libraries from your own console.

## Then what

The app picks its table by anchor, so adding one is additive: a new firmware
cannot affect 13.60. Once a table is in, the only remaining work is a run
through the library and playback on that firmware to confirm nothing else
moved.
