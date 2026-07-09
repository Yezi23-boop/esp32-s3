#ifndef UI_CHINESE_FONTS_H_
#define UI_CHINESE_FONTS_H_

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 编译进固件的通用中文 UI 字体声明。
 *
 * 这些字体由 `scripts/lvgl_fonts/ensure_lvgl_compiled_fonts.py` 生成，覆盖
 * 《通用规范汉字表》一级 3500 字和 ASCII 字符。中文 label 可直接使用
 * `&lv_font_montserrat_lxgw_tghz_level1_3500_24_4` 这类地址，不要改用
 * `lv_font_montserrat*`，否则会缺中文字形。
 */
LV_FONT_DECLARE(lv_font_montserrat_lxgw_tghz_level1_3500_16_4);
LV_FONT_DECLARE(lv_font_montserrat_lxgw_tghz_level1_3500_22_4);
LV_FONT_DECLARE(lv_font_montserrat_lxgw_tghz_level1_3500_27_4);

#ifdef __cplusplus
}
#endif

#endif  // UI_CHINESE_FONTS_H_
