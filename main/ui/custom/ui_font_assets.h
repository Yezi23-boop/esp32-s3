#ifndef UI_FONT_ASSETS_H_
#define UI_FONT_ASSETS_H_

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 映射并校验 AI/Hermes 的 raw Noto 字体资产。
 *
 * 字体位图始终留在 assets 分区的 Flash 映射中；调用失败时没有中文字体回退链。
 *
 * @return 映射和元数据校验成功返回 ESP_OK，否则返回具体错误码。
 */
esp_err_t ui_font_assets_init(void);

/** @brief 查询两个运行时 Noto 字体是否均已可用。 */
bool ui_font_assets_ready(void);

/** @brief 获取 AI 对话的 Noto common 20px 字体；不可用时返回 NULL。 */
const lv_font_t *ui_font_assets_text(void);

/** @brief 获取 Hermes 对话的 Noto common 16px 字体；不可用时返回 NULL。 */
const lv_font_t *ui_font_assets_hermes(void);

/** @brief 获取固定图标文本使用的编译期符号字体。 */
const lv_font_t *ui_font_assets_icon(void);

#ifdef __cplusplus
}
#endif

#endif  // UI_FONT_ASSETS_H_
