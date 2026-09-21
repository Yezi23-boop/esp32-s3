# Current Repo Defaults

Read this file when the working repo clearly uses the same `main/ui/generated + custom + events_init/controller` structure.

## Anchor Map

- Generated layer:
  - `main/ui/generated/*.c`
  - `main/ui/generated/events_init.c`
  - `main/ui/generated/setup_scr_*.c`
- Hand-written layer:
  - `main/ui/custom/*.c`
  - `main/ui/lvgl_task.c`
- Existing preview/runtime helpers:
  - Do not assume `pc_sim/` exists in the current repo snapshot.
  - Do not assume `scripts/pc_sim/capture_preview.ps1` exists in the current repo snapshot.
  - Detect an existing host preview runner first; if none exists, create the minimum preview runner inside the preview workspace and report that choice explicitly.

## Preview Workspace Default

Prefer a preview workspace under:

```text
main/ui/agent_preview/
```

Recommended internal layout:

```text
pages/
regions/
variants/
artifacts/
```

Treat preview UI bodies as Agent-owned. Treat preview runner wiring, screenshot scripts, and long-lived integration seams as Human-owned.

If `main/ui/agent_preview/` does not exist yet, treat it as the default place to create the preview workspace instead of assuming it already exists.

## Output Defaults

For this repo, default to delivering:

1. LVGL preview code
2. Screenshot path
3. Runnable preview command
4. A short note about where this would later connect inside `main/ui`

## Review Truth Split

Use `pc_sim` or another host preview runner to judge:

- layout
- visual hierarchy
- copy
- basic interaction feel

Use the board to judge:

- touch correctness
- display corruption or tearing
- alignment drift
- lifecycle problems
- performance

Do not blur these two verdict layers together.

## Font Note

Do not assume a board-side runtime font asset can be reused unchanged in `pc_sim`.

- Board-side text may come from runtime assets such as `assets/ai-fonts/*.bin` plus a project-specific loader.
- `pc_sim` may need a separate host-friendly font backend behind the same preview seam.

If host preview and board font backends differ, say so explicitly in the result.

## Minimal Request Template

```text
锚点: screen_main / 某按钮 / 某文件 / 某截图
目标: page 或 region
需求: 我想看到什么 UI 和什么交互
参考: 截图、草图、现有页面
```

## Integration Note Default

When suggesting future integration, prefer:

- generated layer keeps the frame and entry object
- custom/controller/events layer owns the wiring
- preview code explains likely target files and minimal event path

Do not default to editing large generated files unless the user explicitly accepts re-export risk.
