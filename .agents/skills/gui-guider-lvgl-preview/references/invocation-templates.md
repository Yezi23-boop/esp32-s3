# Invocation Templates

Use these templates when the user wants to call the skill quickly without re-explaining the workflow each time.

## 1. Child Page Preview

```text
$gui-guider-lvgl-preview
锚点: `screen_main` 上的某个入口按钮，参考 `main/ui/generated/setup_scr_screen_main.c`
目标: page
需求: 帮我做一个独立子页面的 LVGL 预览，先在 `pc_sim` 里画出来。页面包含标题、返回按钮、3 个设置项、一个主操作按钮。支持基础点击态和返回交互。
参考: 视觉风格参考我附的截图；页面组织和命名尽量贴近当前 `main/ui/generated + custom + events_init/controller`
输出: 给我预览代码、自动截图、可运行预览入口，再补一句以后大概该怎么接回 `main/ui`
```

## 2. Local Card Or Region Preview

```text
$gui-guider-lvgl-preview
锚点: `screen_main` 某个卡片区域
目标: region
需求: 帮我把这个区域改成一个状态卡片区，包含主标题、副标题、状态图标、一个小按钮。先通过 `pc_sim` 看效果，不要直接改正式工程接线。
参考: 我会给截图和当前代码锚点
输出: 给我 LVGL 预览代码、截图、运行命令，并说明以后更适合挂到 `generated` 还是 `custom`
```

## 3. Overlay Or Popup Preview

```text
$gui-guider-lvgl-preview
锚点: `screen_main` 顶部状态区
目标: region
需求: 我想试一个下拉状态弹层，先在 `pc_sim` 里做成可预览版本。需要基础开关态、返回或关闭交互，以及几行占位状态文案。
参考: 风格参考我的草图，能力边界不要明显超出当前 LVGL + 板端
输出: 给我预览代码、截图、可运行入口，并总结这版 UI 的反馈重点
```

## 4. Natural-Language Shortcut

Use this when you do not want to fill the full template:

```text
$gui-guider-lvgl-preview
参考 `main/ui/generated/setup_scr_screen_main.c` 里的某个按钮入口，帮我先在 `pc_sim` 里画一个设置子页面。我会先看截图判断这版 UI 合不合理，再继续提修改意见。
```

## 5. Iteration Follow-Up

Use this after the first preview already exists:

```text
继续用 `$gui-guider-lvgl-preview` 迭代上一个主稿：
- 这个卡片太挤
- 标题层级不够明显
- 返回按钮太弱
- 整体再更像我附图一点

保持同一个主稿继续改，同时更新截图和运行入口，不要新开一套方案。
```

## Output Expectation Reminder

When the request is ambiguous, prefer reminding yourself to return:

1. preview unit type
2. generated code paths
3. screenshot path
4. runnable preview command
5. short `main/ui` integration suggestion
