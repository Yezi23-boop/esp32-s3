#ifndef UI_CHINESE_FONTS_H_
#define UI_CHINESE_FONTS_H_

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 编译进固件的通用中文 UI 字体声明。
 *
 * 16/22px 通用字体使用仓库内 5500 字符集，覆盖开放动态文本和 ASCII 字符；
 * 27px 只用于固定月历和 Hermes 标题，使用按需子集以节省 Flash。中文 label
 * 可直接使用对应字体地址，不要改用纯 Montserrat 字体，否则会缺中文字形。
 */
LV_FONT_DECLARE(lv_font_montserrat_lxgw_common_5500_16_4);
LV_FONT_DECLARE(lv_font_montserrat_lxgw_common_5500_22_4);
LV_FONT_DECLARE(lv_font_montserrat_lxgw_tghz_static_27_4);
LV_FONT_DECLARE(lv_font_montserrat_lxgw_music_ui_8_4);
LV_FONT_DECLARE(lv_font_montserrat_lxgw_music_ui_12_4);
LV_FONT_DECLARE(lv_font_montserrat_lxgw_music_ui_13_4);
LV_FONT_DECLARE(lv_font_montserrat_lxgw_music_ui_14_4);
LV_FONT_DECLARE(lv_font_montserrat_lxgw_music_ui_20_4);

#ifdef __cplusplus
}
#endif

#endif  // UI_CHINESE_FONTS_H_
