# ESP-IDF Bridge Pattern For GUI-Guider UI

Use this reference when the project mixes generated LVGL files with hand-written application code.

## Recommended Layer Split

- Generated layer:
  - creates screens and widgets
  - binds default callbacks
  - stores widget handles
  - owns styles and resources
- Hand-written bridge layer:
  - initializes generated UI
  - receives GUI actions
  - translates app state into widget updates
  - serializes LVGL calls onto the correct execution path
- App/business layer:
  - owns state machines
  - owns device/network/storage events
  - never depends directly on generated file names

## Preferred Event Flow

`widget event -> generated callback -> thin bridge function -> app action/state change -> bridge applies UI updates`

## Preferred Update Flow

`device/app event -> app state -> bridge apply function -> specific LVGL object updates`

## What To Keep Out Of Generated Files

- network requests
- sensor polling
- state machine transitions
- long-running logic
- cross-task synchronization logic
- non-trivial formatting or business rules

## What Fits In Thin Hook Files

- extract button or screen identity
- forward to `ui_manager_on_action(...)`
- call a small bridge helper
- map a generated event to a hand-written state update API

## ESP-IDF Guardrails

- Name the UI execution context explicitly.
- If multiple tasks touch UI state, describe the serialization method.
- If the repository already has `ui_manager`, prefer routing all business-facing UI APIs through it.
