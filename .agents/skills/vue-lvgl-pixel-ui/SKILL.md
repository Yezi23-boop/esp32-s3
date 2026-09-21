---
name: vue-lvgl-pixel-ui
description: Use the repository's Vue-to-LVGL pixel UI workflow for new watch pages, major visual redesigns, screenshot or reference-image reproduction, pixel-perfect LVGL work, layered screenshot comparison, host simulator acceptance, and final ESP32-S3 watch validation. Use when work touches tools/ui_prototypes/watch-vue, tools/ui_preview, main/ui layout or styling, or when the user asks for Vue design, LVGL reproduction, pixel comparison, simulator screenshots, or board UI acceptance. Do not invoke the full workflow for logic-only fixes, text-only edits, or changes that cannot affect layout unless the user explicitly requests pixel-level revalidation.
---

# Vue to LVGL Pixel UI

Treat Vue as the visual source, the LVGL host as the reproduction gate, and the physical watch as the final acceptance target. Keep the phases serial and reuse the existing UI owners.

Before changing UI, follow the workflow sections in this skill (classification → Vue baseline → LVGL reproduction → layered comparison → acceptance). When building or capturing the host preview, follow the host-preview steps below. If the repository ships a context library, prefer its UI workflow cards.

## 1. Classify The Request

- Run the full workflow for a new page, major visual redesign, reference-image reproduction, or explicit pixel-perfect request.
- Start from the affected layer when an approved baseline already exists.
- Skip the full workflow for logic-only fixes, text-only edits, or changes proven not to affect layout. Run the narrowest relevant validation instead.
- Stop before editing when the user asks to discuss, review, or design only.
- State the target page, states, fixed test data, affected layers, and success criteria before implementation.

## 2. Design And Freeze In Vue

1. Work in `tools/ui_prototypes/watch-vue` and reuse its Vite app, `WatchScreen`, assets, and interaction model.
2. Keep the visible watch canvas at exactly `410×502`.
3. Define fixed states and data before capture. Include long Chinese, mixed Chinese/English, numbers, punctuation, empty/loading/error states, and scrolling content when relevant.
4. Prefer existing font sizes when they satisfy the design, but do not make existing font assets a visual constraint. Static copy may receive dedicated fonts or subsets. Open dynamic text must not use a page-only subset.
5. Define overflow behavior explicitly: ellipsis, wrapping, horizontal marquee, or page scrolling. Do not shrink fonts at runtime based on string length.
6. Keep Vite running for user and Agent HMR review:

```powershell
Set-Location "<repo-root>\tools\ui_prototypes\watch-vue"
npm run dev
```

7. Capture the `.watch` element after the target state is stable:

```powershell
& "<repo-root>\tools\ui_preview\scripts\capture_vue_watch.ps1" `
  -Url "http://127.0.0.1:8767/" `
  -OutputPath "<repo-root>\tools\ui_preview\artifacts\music\vue-baseline.png"
```

Pass `-SetupScriptPath <file>` when capture requires deterministic navigation or interaction. The file must contain a Playwright function shaped as `async page => { ... }`. Commit a page-specific setup script only when that state will be reused.

8. Present the Vue result to the user. Do not begin formal LVGL reproduction until the user confirms the baseline.
9. Record the approved image and state. Do not change the approved Vue baseline silently during reproduction.

### 2.1 Baseline and interaction parity preflight

Before editing LVGL for any new Vue state, overlay, picker, drawer, or
interactive card, perform a source-and-state parity check. A screenshot alone
is not enough to establish that the two implementations expose the same UI.

- Enumerate every interactive control in the Vue target state: visible,
  hidden, clickable region, action, and return/close path.
- Compare that list with the LVGL object tree before changing LVGL. The two
  sides must not have extra or missing controls, different hit targets, or
  different navigation results.
- Reconcile the current Vue source with user-approved interaction decisions.
  If the Vue prototype is stale or contradictory, fix Vue first, recapture and
  freeze the new baseline; do not make LVGL intentionally diverge from Vue.
- Treat remembered conversation context as a design hint, not as proof of the
  current source state. Re-read the current Vue template and capture the
  target state in this turn.
- For every new state, record a small parity table before LVGL work:

```text
control | Vue visible/action | LVGL visible/action | enter path | back/close path
```

Do not proceed to LVGL while this table contains an unresolved difference.

## 3. Reproduce In LVGL Host

1. Keep production UI in `main/ui/generated` and `main/ui/custom`; keep host-only mocks and artifacts in `tools/ui_preview`.
2. Preserve generated-versus-hand-written ownership. Prefer controllers and custom views over deep edits to generated files unless the generated layout is the requested target.
3. Reproduce and validate in this order:
   - background, physical mask, and safe area
   - primary containers and scrolling geometry
   - cards, buttons, borders, dividers, and spacing
   - images, icons, and static assets
   - typography and dynamic text
   - pressed, selected, disabled, playing, and paused states
   - overlays, scrolling positions, and navigation states
4. Capture the matching LVGL state with the existing host script:

```powershell
& "<repo-root>\tools\ui_preview\scripts\capture_apple_watch_s5_preview.ps1" `
  -OpenMusic `
  -OutputPath "<repo-root>\tools\ui_preview\artifacts\music\lvgl-current.png"
```

For a source-picker or other interactive music state, use the matching
deterministic host entry point instead of manually clicking a different state:

```powershell
& "<repo-root>\tools\ui_preview\scripts\capture_apple_watch_s5_preview.ps1" `
  -OpenMusicPicker `
  -OutputPath "<repo-root>\tools\ui_preview\artifacts\music\lvgl-source-picker.png"
```

5. Use the same data, state, crop, animation point, and `410×502` scale as the Vue baseline.
6. For a new interactive state, exercise the same enter and back/close path
   in Vue and LVGL. A visually similar screenshot does not pass if the
   navigation result differs.

## 4. Compare And Iterate

Compare the full page and every affected layer. Use `-Crop x,y,width,height` for a stable layer or region:

```powershell
& "<repo-root>\tools\ui_preview\scripts\compare_ui_screenshots.ps1" `
  -ReferencePath "<repo-root>\tools\ui_preview\artifacts\music\vue-baseline.png" `
  -ActualPath "<repo-root>\tools\ui_preview\artifacts\music\lvgl-current.png" `
  -OutputDirectory "<repo-root>\tools\ui_preview\artifacts\music\comparison" `
  -LayerName "full-page"
```

Use a maximum per-channel tolerance of `8` and mismatch ratio of `0.5%` as the initial structural baseline. Text must also preserve its bounding box, baseline, line breaks, overflow behavior, and surrounding layout; antialiasing tolerance does not excuse structural text errors.

For each layer:

1. Capture Vue and LVGL in the same state.
2. Verify the parity table: no extra/missing control and no different
   enter/back/close action.
3. Run comparison and inspect the generated PNG, JSON, and Markdown report.
4. Fix the largest structural difference in LVGL.
5. Capture and compare again.
6. Continue without asking the user about each parameter adjustment.
7. Ask for layer acceptance only after the threshold passes and no semantic mismatch remains.

Stop and ask the user when the design is contradictory, an asset or required glyph is missing, LVGL cannot express the approved behavior, dynamic-data behavior is undefined, or hardware behavior must override the Vue design.

## 5. Host Acceptance Gate

Do not proceed to the board until all are true:

- the user approved the Vue baseline
- every affected layer passed comparison
- the full-page comparison passed
- required interactions were exercised in the host
- the user approved the final host screenshot and interaction result

Attach the final LVGL screenshot and comparison report paths to the handoff.

## 6. Build And Validate On The Watch

1. Confirm the ESP-IDF environment using repository `AGENTS.md` rules.
2. Run focused checks, then one final `idf.py build` after the UI shape is stable.
3. Obtain user approval before flashing.
4. Use `idf.py -p <PORT> app-flash` for ordinary UI changes. Do not default to full `flash`.
5. Use `scripts/board/agent_serial_monitor.ps1` or `.py` for serial evidence when needed.
6. Ask the user to validate framing, touch, gestures, scrolling, dynamic text, glyph coverage, images, refresh behavior, and visual artifacts on the watch.
7. Classify board-only differences as design, LVGL reproduction, host-versus-board rendering, or display/touch pipeline issues before changing the baseline.

## User Gates

Pause for user confirmation only at:

1. Vue design freeze.
2. A completed layer that meets automated acceptance.
3. Final LVGL host page and interaction acceptance.
4. Physical watch acceptance.

## Delivery Contract

Report:

- approved Vue baseline path and state
- final LVGL screenshot path and state
- diff image plus JSON/Markdown report paths
- layers and interactions validated
- thresholds used and final mismatch ratio
- build and board validation status
- remaining host-versus-board risks

Do not claim pixel-level completion after only a full-page visual inspection, and do not claim delivery complete before physical watch acceptance.
