#ifndef CBIN_FONT_BRIDGE_H_
#define CBIN_FONT_BRIDGE_H_

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_font_t *cbin_font_bridge_create(void *data);
void cbin_font_bridge_destroy(lv_font_t *font);

#ifdef __cplusplus
}
#endif

#endif  // CBIN_FONT_BRIDGE_H_
