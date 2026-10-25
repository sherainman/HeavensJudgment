# Heaven's Judgment

This project is intended to be an experimental combat framework for Lost Judgment.
Long-term goal is to replace and extend Lost Judgment's combat logic with a custom combat system inspired by the presentation and mechanics from Stranger Than Heaven.

The closeness of how this will actually end up to STH's combat is unknown since I'm pretty much basing this off of my own visual tracking of the combat and assumptions of how it may feel.

## Current Status

Development build. Not a 1.0 release.

The combat framework, vinyl HUD, directional guard, and camera anchor are
implemented and playable. The presentation and test tooling are still being
hardened: 17 of the 49 move nodes still use `SELECT ANIMATION`, and the full
unattended harness is phased in after the offline HUD/log/probe gate.

## Currently implemented

- Native ASI runtime for Lost Judgment, loaded through SRMM
- Hooks into DE's combat/Fighter_Command at runtime
- Custom controller layout
  - RB - Right Hand
  - LB - Left Hand
  - RT - Right Leg
  - LT - Left Leg
  - A - Evade
  - LT + RT - Tackle
- Custom input buffering for attacks
- Hand/leg combo routes and branching between them
- Running attacks
- Trigger chord handling so RT/LT can still be used as kicks while LT + RT is tackle
- Direct native evade requests instead of forcing evade through Fighter_Command
- Stops some vanilla inputs/actions from leaking into HJ's controls
- Cross-style move execution without needing to edit animation files
- Runtime attack tracking
- Basic attack steering/commitment stuff on RH1
- Player combat object/entity tracking
- Player position/movement direction tracking
- Vanilla style switching blocked
- Combat tracing/debug logging
- Runtime tracing for Fighter_Command checks, attacks and combat states
- Still uses DE's own Fighter_Command.cfc, animations, damage and combat backend
- Vinyl-record HUD with the native player HP strip removed; the white label is
  the sole player HP gauge and the outer yellow-green waveform is EX
- Directional guard with live guard-side switching during a hold (neutral IDs
  `5559 / 13919 / 300`)
- Camera anchor/assist for the boxing stance
- Offline `tools/hjbench` HUD/startup-log/probe-contract gate and
  `tools/hj_core` logic tests

## Planned architecture

- 'Core/' - logging, memory utils, pattern scanning
- 'Engine/' - DE interface
- 'Hooks/' - runtime hooks
- 'Combat/' - the custom combat system
- 'external/' - safetyhook / any third-party dependencies 

## Requirements

- Visual Studio 2026 (v145 toolset)
- x64 build target
- C++23
- Lost Judgment
- SRMM / an ASI loader

## Testing

The offline HUD/log gate lives in [`tools/hjbench`](tools/hjbench/README.md).
It requires Python 3.10+ with `numpy`, `Pillow`, and `PyYAML`, and does not
attach to the game:

```powershell
cd tools/hjbench
python -m unittest discover -s tests -t . -v
python hjbench.py run scenarios/smoke.yaml
python hjbench.py probe check fixtures/probe_session.jsonl --require-capture
powershell -ExecutionPolicy Bypass -File gate.ps1
```

The game-independent C++ logic seam lives in
[`tools/hj_core`](tools/hj_core/README.md) and is built with CMake:

```powershell
cd tools/hj_core
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
powershell -ExecutionPolicy Bypass -File gate.ps1
```

Both gates can be run together from the `tools` directory:

```powershell
powershell -ExecutionPolicy Bypass -File gate.ps1
```

## Disclaimer

Not associated with or endorsed by SEGA or RGG Studio. Unofficial modding and reverse engineering project.

Lost Judgment and everything in it — characters, animation, audio, trademarks — belongs to SEGA / RGG Studio. None of it is claimed here, and none of it is redistributed. The MIT license covers this repository's own source code and nothing else.

## License

MIT. Copyright (c) 2026 sherainman.

The MIT license covers this repository's own source code and nothing else. See
the disclaimer above. Third-party code keeps its own license: safetyhook is
Boost 1.0 and Zydis is MIT, both under `external/` with their notices. No game
asset archives, extracted animation data or other SEGA/RGG Studio content is
included in this repository, apart from the single user-supplied HUD
regression screenshot under `tools/hjbench/fixtures/`; that frame is not
covered by the MIT license and should be replaced with a synthetic or
clean-room fixture before any public distribution that requires one.

