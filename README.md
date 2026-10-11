# Heaven's Judgment

This project is intended to be a experimental combat framework for Lost Judgment.
Long-term goal is to replace and extend Lost Judgment's combat logic with a custom combat system inspired by the presentation and mechanics from Stranger Than Heaven.

The closeness of how this will actually end up to STH's combat is unknown since I'm pretty much basing this off of my own visual tracking of the combat and assumptions of how it may feel.

## Current Status

Very EARLY development.

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

## Planned architecture

- 'Core/' - logging, memory utils, pattern scanning
- 'Engine/' - DE interface
- 'Hooks/' - runtime hooks
- 'Combat/' - the custom combat system
- 'external/' - safetyhook / any third-party dependencies 

## Requirements

- Visual Studio 2022
- x64 build target
- C++23
- Lost Judgment
- SRMM / an ASI loader

## Disclaimer

Not associated with or endorsed by SEGA or RGG Studio. Unofficial modding and reverse engineering project.
