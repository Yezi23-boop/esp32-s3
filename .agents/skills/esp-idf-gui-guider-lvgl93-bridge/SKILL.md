---
name: esp-idf-gui-guider-lvgl93-bridge
description: "用于分析、解释或集成 ESP-IDF 项目中的 GUI Guider 导出 LVGL C 代码；当代码包含 ui.c、ui.h、events_init.c、setup_scr_*.c、custom.c、guider_ui，或需要把生成页面与手写 ui_manager / controller / 业务逻辑衔接时使用。"
metadata:
  scope: project
  owner: ye
  group: esp32-watch
---

# ESP-IDF GUI-Guider LVGL 9.3 Bridge

## Overview

Use this skill to treat GUI-Guider exported code as a replaceable UI structure layer, then explain how that generated layer should connect to hand-written ESP-IDF UI logic. Default to structure analysis first, then recommend safe integration points and small follow-up edits.

## Quick Start

- Inspect the real repository first. Do not assume the exact export layout.
- If the project contains GUI-Guider-shaped files, read `references/gui-guider-generated-layout.md`.
- If the project also has a hand-written UI facade such as `ui_manager`, page controllers, or business event adapters, read `references/esp-idf-bridge-pattern.md`.
- Answer in this order:
  - generated layer structure
  - hand-written layer structure
  - current bridge points
  - recommended edit locations
  - re-generation risk

## Workflow

### 1. Classify Files By Ownership

- Treat these as likely generated-layer signals:
  - `ui.c`
  - `ui.h`
  - `events_init.c`
  - `setup_scr_*.c`
  - `custom.c`
  - `gui_guider.c`
  - `guider_ui.c`
  - `guider_ui` / `lv_ui` object holders
  - generated image/font/resource files
- Treat these as likely hand-written bridge-layer signals:
  - `ui_manager.*`
  - `screen_manager.*`
  - `page_controller.*`
  - app event dispatchers
  - state adapters
  - task/queue based UI updaters

If a file mixes both, say so explicitly and identify which sections are structural versus business-owned.

### 2. Reconstruct The UI Shape

- Identify:
  - screen creation entrypoints
  - page tree and parent/child ownership
  - object handle storage
  - event registration path
  - screen switching path
  - custom user hook points
- When useful, summarize the flow as:
  - `ui_init/setup_scr -> events_init -> callback/custom hook -> ui_manager/bridge -> app logic -> UI update`

### 3. Separate Generated Responsibilities From Hand-Written Responsibilities

- Generated layer should usually own:
  - object creation
  - layout
  - styles
  - static text defaults
  - resource wiring
  - thin callback entrypoints
- Hand-written bridge layer should usually own:
  - business state
  - device events
  - ESP-IDF task/thread handoff
  - data formatting
  - page switching policy
  - error/retry/timeout behavior
  - dynamic runtime UI population

Default recommendation: keep the generated directory replaceable after re-export, and keep business logic in a stable hand-written layer outside the generated files.

### 4. Recommend Safe Edit Points

- Prefer these edit points first:
  - hand-written `ui_manager` or controller layer
  - bridge callbacks that translate GUI actions into app events
  - dedicated UI state application functions
- Use generated-layer hook files such as `custom.c` only for thin bridge code:
  - extract IDs
  - forward events
  - call hand-written APIs
- Warn before recommending direct edits inside generated screen build files.
- If direct edits inside generated files are unavoidable, say exactly:
  - which file to touch
  - why it is risky
  - what will likely be overwritten by a future GUI-Guider export

### 5. Apply ESP-IDF-Specific Guardrails

- Do not assume LVGL can be updated safely from arbitrary tasks or ISR context.
- Call out when the project needs:
  - a dedicated UI task
  - queue-based handoff
  - mutex/serialized LVGL access
  - `lv_async` or equivalent deferred update path
- Prefer a single hand-written UI facade between app logic and generated UI code.
- Do not recommend editing the LVGL library itself unless the task explicitly targets LVGL internals.

## Output Contract

When this skill is used, default to this structure:

1. `Generated Layer`
2. `Hand-Written Layer`
3. `Current Bridge`
4. `Recommended Edit Locations`
5. `Re-Export Risk`
6. `Next Safe Step`

Keep it concrete. Name exact files, symbols, and callback paths when they exist.

## Common Mistakes

- Treating every `custom.c` line as safe permanent business code.
- Mixing business state transitions into generated callback files.
- Updating LVGL directly from unrelated ESP-IDF tasks without explaining the thread model.
- Recommending deep edits in generated files when the same behavior belongs in a bridge layer.
- Explaining LVGL generically without reconstructing the actual generated project structure first.

## When Not To Use

- Do not use this skill for generic LVGL widget styling or accessibility review when GUI-Guider structure is not part of the task.
- Do not use this skill for non-ESP-IDF platforms unless the user only wants high-level generated-layer versus hand-written-layer guidance.



