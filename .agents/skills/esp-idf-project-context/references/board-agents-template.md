# Board AGENTS Template

Use this when a repository needs a documentation-first board entrypoint under `boards/<board>/AGENTS.md`.

```md
# AGENTS.md

## Board Scope
- Current board: `[board name]`
- Target MCU: `[ESP32 variant]`
- Display interface: `[SPI / RGB / MIPI / none]`
- Touch controller: `[chip or none]`
- Audio path: `[brief note or none]`
- Flash / PSRAM: `[capacity if known]`

## Board Rules
- This directory is the board-local guidance layer for this board.
- Do not assume this directory already owns all board code.
- If board mappings still live in `components/`, keep runtime ownership there until the user asks for a code migration.
- Do not hardcode this board's pins into shared components unless the repository already uses that pattern intentionally.
- Preserve reset, delay, power, and init sequence behavior unless there is clear evidence to change it.

## Validation
- At minimum, check boot success and serial logs.
- If applicable, also check display init, touch response, audio path, and connectivity for obvious regression.
- If no hardware test was run, say so explicitly.
```
