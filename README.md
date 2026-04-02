# libmucom88-mml

MUCOM88-compatible MML parser and sequencer library for YM2608 (OPNA).

Header-only C++17 library with zero external dependencies.

## Overview

This library provides a complete MML (Music Macro Language) parser and sequencer compatible with [MUCOM88](https://www.ancient.co.jp/~mucom88/), the music driver created by Yuzo Koshiro for the NEC PC-8801.

It converts MML text into YM2608 register writes through an abstract FM engine interface, allowing any YM2608 emulator (fmgen, ymfm, etc.) to be used as the backend.

## Architecture

```
MML text (.muc)
    |
    v
MmlParser  --- parses MML, expands macros, generates events
    |
    v
MmlEngine  --- sequences events, drives Timer-B, writes registers
    |
    v
IFmEngine  --- abstract interface (writeReg, generateInterleaved, ...)
    |
    v
[Your YM2608 emulator]  (fmgen, ymfm, etc.)
```

## Usage

```cpp
#include <mucom88/mml_parser.hpp>
#include <mucom88/mml_engine.hpp>
#include <mucom88/fm_engine_interface.hpp>

// 1. Implement IFmEngine with your YM2608 emulator
class MyFmEngine : public IFmEngine { /* ... */ };

// 2. Parse MML
MmlParser parser;
parser.loadVoiceDat("voice.dat");
auto result = parser.parse(mmlText);

// 3. Set up engine
MyFmEngine fmEngine;
fmEngine.init(44100);

MmlEngine engine;
engine.init(&fmEngine, 44100);
for (auto& [no, patch] : result.patches)
    engine.setPatch(no, patch);
engine.setWholeTick(result.wholeTick);
for (int ch = 0; ch < 11; ch++)
    engine.setEvents(ch, result.channelEvents[ch]);

// 4. Play
engine.play();
while (engine.isPlaying()) {
    engine.advance(256);  // advance by 256 samples
    int16_t buf[512];
    fmEngine.generateInterleaved(buf, 256);
    // ... output buf to audio device
}
```

## Files

| File | Description |
|------|-------------|
| `fm_common.hpp` | FM patch definitions, frequency tables, voice.dat parser |
| `fm_engine_interface.hpp` | `IFmEngine` abstract interface |
| `mml_parser.hpp` | MML parser (MUCOM88 format, 93+ files validated) |
| `mml_engine.hpp` | MML sequencer (Timer-B timing, 11ch, reverb, LFO, portamento) |

## Supported MML Features

- **11 channels**: FM (6ch), SSG (3ch), Rhythm (1ch), ADPCM-B (1ch)
- **Notes**: `cdefgab`, octave `<>o`, sharp `+#`, flat `-`, rest `r`
- **Length**: `l` default, numeric suffix, dot `.` (multiple dots), tie `&`, `^` extension
- **Volume**: `v` (FM: FMVDAT table, SSG: 0-15, ADPCM-B: 0-255), `()` relative
- **Tempo**: `t` (BPM), `T` (Timer-B direct), `C` (clock)
- **Patch**: `@N` (voice.dat), inline voice definitions
- **Loop**: `[...]N`, `/` break, `L` loop point
- **Macro**: `*N{...}` definition, `*N` expansion
- **Effects**: `q` staccato, `D` detune, `M` vibrato (software LFO), `H` hardware LFO
- **Reverb**: `R` (pseudo-reverb via TL decay), `RF` on/off, `Rm` mode
- **Portamento**: `{note1 note2}` pitch slide
- **Echo**: `\=N,M` / `\` echo macro
- **Register**: `y` direct register write, `k` key transpose, `p` pan
- **SSG**: `@N` presets (SSGDAT software envelope), `E` custom ADSR envelope
- **Rhythm**: `@` instrument bitmask, `v` per-instrument levels
- **ADPCM-B**: K track with delta-N pitch, mucompcm.bin sample playback

## Integration

Add as a git submodule:
```bash
git submodule add https://github.com/takamori-tech/libmucom88-mml.git vendor/libmucom88-mml
```

Add include path in CMakeLists.txt:
```cmake
target_include_directories(your_target PRIVATE vendor/libmucom88-mml/include)
```

## Projects Using This Library

- [CLAUDIUS](https://github.com/takamori-tech/rpi5-native-game) - Retro STG game for Raspberry Pi
- [MUCOM88V](https://github.com/takamori-tech/mucom88v) - YM2608 VST/AU plugin

## License

MIT License

## Credits

- MML format: [MUCOM88](https://www.ancient.co.jp/~mucom88/) by Yuzo Koshiro
- Parser/Sequencer: takamori-tech + Claude (Anthropic)
