#include "cbin_font_bridge.h"

#include "cbin_font.h"

lv_font_t *cbin_font_bridge_create(void *data) {
    return cbin_font_create((uint8_t *)data);
}

void cbin_font_bridge_destroy(lv_font_t *font) {
    if (font != NULL) {
        cbin_font_delete(font);
    }
}
