# GUI-Guider Generated Layout Cues

Use this reference when the repository looks like a GUI-Guider export or a partially integrated export.

## Common Generated File Roles

- `ui.c`, `gui_guider.c`, `guider_ui.c`
  - top-level UI init or global object holder entry
- `ui.h`, `gui_guider.h`, `guider_ui.h`
  - exported object handles, screen structs, public setup helpers
- `setup_scr_<name>.c`
  - screen object construction and local widget tree assembly
- `events_init.c`
  - event binding from widgets to callbacks
- `custom.c`
  - user hook area or thin helper layer intended to survive some re-exports
- `ui_img_*`, `ui_font_*`, `*_data.c`
  - generated assets and resources

## Ownership Heuristic

Ask this question for each file:

`If GUI-Guider re-exported the UI tomorrow, should this file be safely replaceable?`

- If yes, treat it as generated-layer owned.
- If no, treat it as bridge-layer or application-layer owned.

## Practical Reading Order

1. Find the top-level UI init entry.
2. Find where each screen is constructed.
3. Find how object handles are stored.
4. Find where callbacks are bound.
5. Find whether `custom.c` or similar files are invoked.
6. Find whether an outer hand-written manager wraps the generated entrypoints.
