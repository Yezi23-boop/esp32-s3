---
name: lvgl-chinese-ui-fonts
description: "本仓库（AI Memory Watch / esp32-s3）的 LVGL 中文字体规则与生成流程。用于新增或修改中文 UI、动态文本、字符集、C 字体和页面大字号子集时，按项目现有字库策略完成绑定、按需生成和验证。"
---

# 项目 LVGL 中文字体

本 Skill 只适用于本仓库（AI Memory Watch）。它负责把中文文本路由到项目现有字体，或根据实际使用点生成最小的静态子集，避免每个页面重复携带大字库。

## 字体基线

- 普通动态 UI 使用 `common_5500`：字符集为 `tools/lvgl_fonts/charsets/charset_tghz_common_5500.txt`，当前编译字号为 16px 和 22px。
- 固定文案使用页面级子集；27px 及以上的大字号必须按使用点生成，当前示例为 `tghz_static_27_4`。
- `main/ui/custom/ui_chinese_fonts.h` 是字体符号声明入口，控件源码通过它绑定字体；生成的 C 字体放在 `main/ui/custom/fonts/`。
- Hermes 等运行时 `.bin` 字体必须位于 `resources/fonts/` 并进入 `build/resources.bin`；仅执行 `app-flash` 不会更新 `resources` 分区。
- 音乐标题、歌手名、通知正文等运行时内容属于动态文本，不能使用页面小子集。字符覆盖不足时扩充通用 5500 字符集并重新生成对应字号。
- 不为普通 LVGL UI 引入字体 fallback（回退链）。`xiaozhi` AI 字体和 Hermes 的运行时字库是各自功能 owner 的专用路径，不要把它们改造成通用回退链。

## 路由流程

1. 先确认文本是动态还是固定，并检查实际使用点、字号和已有字体声明。
2. 动态中文文本绑定 `common_5500` 的 16px/22px 字体；缺字时修改通用字符集，再生成受影响字号，不能为单个动态页面创建小子集。
3. 固定中文文案在 27px 及以上字号使用页面子集：收集该页面全部固定字符，写入 `tools/lvgl_fonts/charsets/` 下的字符集文件，再生成对应 C 字体。
4. 更新 `ui_chinese_fonts.h` 和控件引用，删除因本次替换产生的旧声明、旧引用和旧生成文件；不要删除仍被其他页面使用的字体。

## 生成命令

通用字体字符集或规格发生变化时：

```powershell
uv run python scripts/lvgl_fonts/ensure_lvgl_compiled_fonts.py --force
```

固定大字号子集使用：

```powershell
uv run python scripts/lvgl_fonts/build_lvgl_cfont.py --size <字号> --bpp 4 --name <子集名> --charset tools/lvgl_fonts/charsets/<字符集文件>.txt --output main/ui/custom/fonts/lv_font_montserrat_lxgw_<子集名>_<字号>_4.c
```

先用 `scripts/lvgl_fonts/scan_lvgl_chinese_text.py` 检查源码中的中文使用点，再决定扩充通用字符集还是生成固定文案子集。不要凭单页截图猜字符覆盖范围。

## 验证闭环

完成字体改动后，至少执行：

```powershell
uv run pytest tests/test_ui_chinese_fonts_source.py tests/test_lvgl_chinese_font_scripts_source.py -q
uv run python scripts/lvgl_fonts/scan_lvgl_chinese_text.py
git diff --check
```

涉及 C 字体或 LVGL 源码时，再执行项目规定的 `idf.py build`，确认应用分区余量；涉及布局或交互时，使用 `vue-lvgl-pixel-ui` 完成 host 截图和对比。若覆盖范围或构建余量不达标，报告具体缺字和体积，不要静默降级字号或换成页面子集。

新增或替换运行时 `.bin` 后，必须确认 `build/resources.bin` 已重新生成并执行资源分区烧录；不能用只更新应用分区的 `app-flash` 作为完成依据。
