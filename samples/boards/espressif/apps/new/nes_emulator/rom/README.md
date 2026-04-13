# NES ROM Placeholder

Place your iNES format ROM file here as **`nestest.nes`**.

## Recommended test ROM

[nestest.nes](https://www.qmtpro.com/~nes/misc/nestest.nes) — the standard NES
CPU accuracy test ROM by kevtris. It runs silently (black screen) but is ideal
for verifying the emulator core is functioning before loading a real game ROM.

## Changing the ROM

Edit `CMakeLists.txt` and update `NES_ROM_FILE` to point to any iNES `.nes`
file you wish to embed.

> **Legal reminder**: Only use ROMs that you own a legal copy of, or
> publicly-available homebrew / test ROMs.
