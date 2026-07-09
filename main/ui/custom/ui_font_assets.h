#ifndef UI_FONT_ASSETS_H_
#define UI_FONT_ASSETS_H_

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

// 运行时优先读取 assets 分区字体，失败时回退到编译字体。
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_font_assets_init(void);
bool ui_font_assets_ready(void);

const lv_font_t *ui_font_assets_title(void);
const lv_font_t *ui_font_assets_body(void);
const lv_font_t *ui_font_assets_meta(void);
const lv_font_t *ui_font_assets_icon(void);

#ifdef __cplusplus
}
#endif

#endif  // UI_FONT_ASSETS_H_
