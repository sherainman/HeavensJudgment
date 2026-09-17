# Heaven's Judgment

This project is intended to be a experimental combat framework for Lost Judgment.
Long-term goal is to replace and extend Lost Judgment's combat logic with a custom combat system inspired by the presentation and mechanics from Stranger Than Heaven.

The closeness of how this will actually end up to STH's combat is unknown since I'm pretty much basing this off of my own visual tracking of the combat and assumptions of how it may feel.

## Current Status

Insanely EARLY development.

## Currently implemented

- Native ASI runtime for Lost Judgment, with SRMM
- Runtime combat command interception
- Direct game-thread attack invocation
- Custom controller attack mapping
- Cross-style attack execution without modifying any animation files (will probably create a moveset in the future just for combo/accuracy's sake)
- Support for Snake, Crane, Tiger and Boxer combat commands
- Basic combat tracing/debug logging
- Runtime state tracing for attacks and combat states
- Uses DE's existing animation/damage/combat Fighter_Command.cfc as the backend 

## Planned architecture

- 'Core/' - logging, memory utils, pattern scanning
- 'Engine/' - DE interface
- 'Hooks/' - runtime hooks
- 'Combat/' - the custom combat system
- 'external/ - safetyhook / any third-party dependencies 

## Requirements

- Visual Studio 2022
- x64 build target
- C++23
- Lost Judgment
- SRMM / an ASI loader

## Disclaimer

Not associated with or endorsed by SEGA or RGG Studio. Unofficial modding and reverse engineering project.
