---
name: gui-guider-lvgl-preview
description: "用于为 GUI Guider 或类似 main/ui/generated + custom + events_init/controller 结构生成优先主机预览的 LVGL 代码；当需要把 UI 想法、截图、代码锚点、按钮入口、子页面、卡片区域、overlay、状态面板或设置区域转成 LVGL 预览、截图和板端集成建议时使用。"
metadata:
  scope: project
  owner: ye
  group: esp32-watch
---

# GUI Guider LVGL Preview

## Overview

Use this skill to turn a UI idea into LVGL preview code that can be reviewed in `pc_sim` or another repo-native host preview runner before deciding whether it deserves board-side integration. Default to understanding the current `main/ui` ownership first, then generate preview code, run preview capture, and finish with a short "how to wire this back" note.

## Quick Start

- Accept a minimal anchor from the user:
  - page name
  - button name
  - file path
  - screenshot or sketch
- Inspect the real repository before drawing:
  - generated layer
  - custom layer
  - `events_init` or controller layer
  - existing preview runtime or other host preview surface
- If the current repo has the same `main/ui` shape, read `references/current-repo-defaults.md`.
- If the user wants a ready-to-copy invocation format, read `references/invocation-templates.md`.
- Decide whether the preview unit is a `page` or `region`, then state that choice in the output.

## Workflow

### 1. Reconstruct The Host UI Context

- Find the host anchor and nearby ownership files.
- Identify:
  - host screen
  - entry button or region
  - generated layer files
  - custom/controller/event wiring files
- Keep the generated layer replaceable. Do not recommend large edits inside generated files unless the user explicitly asks for that risk.

### 2. Choose The Preview Scope

- `page`: for child pages, independent detail pages, settings pages, or standalone flows
- `region`: for cards, overlays, status areas, panels, or local content blocks
- Default to matching the user's ask. Do not silently widen a local card request into a full app-shell rewrite.

### 3. Generate Preview-Friendly LVGL Code

- Keep the internal page or region code hand-written and easy to iterate.
- Keep external naming and entry seams close to the host project's GUI Guider semantics so later integration is understandable.
- Prefer code that is easy to regenerate and compare over code that imitates generated output line-for-line.
- Simulate only the UI state needed to judge layout, hierarchy, copy, and interaction feel.
- Do not invent a new backend architecture just to make the preview look alive.

### 4. Run Preview And Capture Evidence

- Produce both:
  - a screenshot for quick review
  - a runnable preview entry for deeper inspection
- If the repo already has a preview runner or screenshot script, reuse it.
- If the repo does not have a preview runner, create the minimum preview runner needed inside the preview workspace and state that you did so.
- If the user explicitly forbids preview-runner scaffolding, stop and report that the preview infrastructure is the blocker instead of pretending the output is complete.
- If preview and board font backends differ, say so explicitly instead of pretending they are the same problem.

### 5. Close With An Integration Note

- Summarize:
  - what was generated
  - what interaction or state was simulated
  - where it would likely connect back into the real UI stack
- Keep integration advice concrete:
  - target files or file classes
  - likely entry event location
  - likely page creation or region mount location
  - return or close behavior if relevant
- Do not directly modify the formal board-side path unless the user explicitly asks.

## Output Contract

When this skill is used, default to this structure:

1. `Preview Unit`
2. `Generated LVGL Code`
3. `Preview Evidence`
4. `Feedback Focus`
5. `Main/UI Integration Suggestion`

Expected content:

- `Preview Unit`
  - `page` or `region`
  - host anchor
  - simulated interaction depth
- `Generated LVGL Code`
  - exact file paths created or updated
- `Preview Evidence`
  - screenshot path
  - runnable preview command or entry
- `Feedback Focus`
  - layout
  - visual hierarchy
  - copy
  - interaction feel
  - board feasibility
- `Main/UI Integration Suggestion`
  - likely target files
  - minimal wiring idea
  - generated-layer overwrite risk if any

## Companion Skills

Use companion skills only when the current request needs them. Do not load every related skill by default.

- Use `esp-idf-gui-guider-lvgl93-bridge` when the task needs future integration advice for GUI Guider files such as `setup_scr_*.c`, `events_init.c`, `custom.c`, `guider_ui`, or controller layers. Refer to the skill name, not its folder path, because local folder naming may mention a different LVGL version.
- Use `lvgl-chinese-ui-fonts` when generated or reviewed UI copy contains Chinese text. This keeps Chinese labels on project Chinese-capable LVGL fonts instead of Montserrat-only fonts.
- Use `esp32-watch-project-guide` only when the user asks to move from host preview toward board-side validation, flash, monitor, touch/display behavior, lifecycle, or performance checks.
- Use `diagnose` when the preview loop itself fails, such as build errors, SDL/runtime DLL issues, missing screenshots, invisible windows, or font rendering problems.
- Use `skill-creator` when editing this skill, adding templates, changing metadata, or validating the skill package.

## Boundaries

- Treat `pc_sim` or another host preview runner as the fast-review surface for:
  - layout
  - hierarchy
  - copy
  - basic interaction feel
- Treat the board as the final truth for:
  - touch behavior
  - tearing or garbling
  - alignment drift
  - lifecycle issues
  - performance
- Do not oversell a preview as "board ready" unless the user asked for and completed board-side validation.
- Default to one evolving main draft. Keep a nearby backup or variant only when the user asks to preserve an alternative direction.

## Minimal Input Contract

Prefer this short template when the user is willing:

```text
锚点:
目标:
需求:
参考:
```

Where:

- `锚点`: page, button, file, or screenshot anchor
- `目标`: `page` or `region`
- `需求`: desired UI and interactions
- `参考`: screenshot, sketch, or style note

If the user gives only natural language, extract the minimum anchor yourself and continue.

## Common Mistakes

- Treating preview code as if it already owns the formal board-side runtime.
- Generating a whole new app shell when the user only asked for a child page or local region.
- Ignoring the generated/custom/controller split and dropping advice into the wrong owner layer.
- Confusing "simulate enough to judge the UI" with "rebuild the full backend flow."
- Claiming board feasibility without separating host-preview truth from board truth.



