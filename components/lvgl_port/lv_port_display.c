/**
 * @file lv_port_display.c
 * @brief LVGL 显示链路实现
 */

#include <inttypes.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "co5300_panel.h"
#include "co5300_panel_defaults.h"
#include "lv_port.h"
#include "lv_port_internal.h"
static uint16_t *s_bounce_bufs[2] = {NULL, NULL};
static size_t s_bounce_buf_size = 0;
static uint8_t s_bounce_buf_idx = 0;

static void lv_port_disp_init_bounce_buf(void)
{
    s_bounce_buf_size = LCD_WIDTH * LV_PORT_FIXED_CHUNK_LINES * sizeof(uint16_t);
    for (int i = 0; i < 2; i++)
    {
        if (s_bounce_bufs[i] == NULL)
        {
            s_bounce_bufs[i] = heap_caps_malloc(s_bounce_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (!s_bounce_bufs[i])
            {
                ESP_LOGE(LV_PORT_TAG, "Failed to allocate display bounce buffer %d of size %zu!", i, s_bounce_buf_size);
            }
            else
            {
                ESP_LOGI(LV_PORT_TAG, "Successfully allocated display bounce buffer %d of size %zu", i, s_bounce_buf_size);
            }
        }
    }
}

static bool lvgl_port_flush_ready_callback(esp_lcd_panel_io_handle_t panel_io,
                                           esp_lcd_panel_io_event_data_t *edata,
                                           void *user_ctx);
static esp_err_t lv_port_flush_area_with_sync(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
static esp_err_t lv_port_flush_area_chunked_simple(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
static void lv_port_rounder_event_cb(lv_event_t *e);

/**
 * @brief 初始化 `small` 路径显示缓冲。
 *
 * - 优先尝试片内 DMA 内存，失败后回退 PSRAM。
 * - 使用双缓冲 PARTIAL 渲染模式，减少刷新等待带来的 UI 停顿。
 *
 * @return 无返回值。
 */
void lv_port_disp_init_small(void)
{
    lv_port_disp_init_bounce_buf();
    // 单块缓冲像素数 = 屏宽 * 片高。
    const size_t disp_buf_size = LCD_WIDTH * LV_PORT_FIXED_CHUNK_LINES1;

    ESP_LOGI(LV_PORT_TAG,
             "Small buffer size: %zu pixels (%.1f KB each)",
             disp_buf_size,
             (disp_buf_size * sizeof(lv_color_t)) / 1024.0f);

    lv_color_t *disp1 = heap_caps_malloc(disp_buf_size * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    lv_color_t *disp2 = heap_caps_malloc(disp_buf_size * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if (!disp1)
    {
        disp1 = heap_caps_malloc(disp_buf_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT);
    }
    if (!disp2)
    {
        disp2 = heap_caps_malloc(disp_buf_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT);
    }

    ESP_LOGI(LV_PORT_TAG,
             "Small Buffer1: %s, Buffer2: %s",
             esp_ptr_external_ram(disp1) ? "PSRAM" : "Internal",
             esp_ptr_external_ram(disp2) ? "PSRAM" : "Internal");

    s_display = lv_display_create(LCD_WIDTH, LCD_HEIGHT);

    /* 路线 B：LVGL 全程 RGB565_SWAPPED，flush 不再手动 swap（真机对照已验证，见 runs/ 记录）。 */
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(s_display, lv_port_disp_flush);
    lv_display_add_event_cb(s_display, lv_port_rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_set_buffers(s_display,
                           disp1,
                           disp2,
                           disp_buf_size * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lvgl_port_flush_ready_callback,
    };
    // 注册底层传输完成回调，flush 结束时由回调触发 `lv_display_flush_ready()`。
    co5300_panel_register_color_done_callback(&cbs, s_display);
}

/**
 * @brief 初始化 `single` 路径显示缓冲。
 *
 * 使用两块 PSRAM 缓冲，适合较大分块、降低片内内存压力。
 *
 * @return 无返回值。
 */
void lv_port_disp_init_single(void)
{
    lv_port_disp_init_bounce_buf();
    const size_t disp_buf_size = LCD_WIDTH * LV_PORT_FIXED_CHUNK_LINES2;

    ESP_LOGI(LV_PORT_TAG,
             "Single buffer size: %zu pixels (%.1f KB)",
             disp_buf_size,
             (disp_buf_size * sizeof(lv_color_t)) / 1024.0f);
    lv_color_t *disp_buf1 = heap_caps_malloc(disp_buf_size * sizeof(lv_color_t), MALLOC_CAP_32BIT | MALLOC_CAP_SPIRAM);
    lv_color_t *disp_buf2 = heap_caps_malloc(disp_buf_size * sizeof(lv_color_t), MALLOC_CAP_32BIT | MALLOC_CAP_SPIRAM);

    if (!disp_buf1)
    {
        ESP_LOGE(LV_PORT_TAG, "单缓存1分配失败");
        return;
    }
    if (!disp_buf2)
    {
        ESP_LOGE(LV_PORT_TAG, "单缓存2分配失败");
        return;
    }

    ESP_LOGI(LV_PORT_TAG,
             "Single Buffer1: %s, Buffer2: %s",
             esp_ptr_external_ram(disp_buf1) ? "PSRAM" : "Internal",
             esp_ptr_external_ram(disp_buf2) ? "PSRAM" : "Internal");

    s_display = lv_display_create(LCD_WIDTH, LCD_HEIGHT);

    /* 路线 B：LVGL 全程 RGB565_SWAPPED，flush 不再手动 swap（真机对照已验证，见 runs/ 记录）。 */
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(s_display, lv_port_disp_flush);
    lv_display_add_event_cb(s_display, lv_port_rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_set_buffers(s_display,
                           disp_buf1,
                           disp_buf2,
                           disp_buf_size * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lvgl_port_flush_ready_callback,
    };
    co5300_panel_register_color_done_callback(&cbs, s_display);

    ESP_LOGI(LV_PORT_TAG,
             "LVGL 9.5 单缓存显示驱动初始化完成 (PARTIAL/%d行, RGB565格式%s字节交换)",
             LV_PORT_FIXED_CHUNK_LINES2,
             LV_PORT_BYTE_SWAP_ENABLE ? "启用" : "禁用");
}

static bool IRAM_ATTR lvgl_port_flush_ready_callback(esp_lcd_panel_io_handle_t panel_io,
                                                     esp_lcd_panel_io_event_data_t *edata,
                                                     void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    if (s_flush_pending_count > 0)
    {
        s_flush_pending_count--;
    }

    if (s_flush_pending_count == 0)
    {
#if CO5300_PANEL_USE_TE_SIGNAL
        s_frame_ctx.frame_start = true;
#endif
        lv_display_t *disp = (lv_display_t *)user_ctx;
        lv_display_flush_ready(disp);
        return true;
    }

    return false;
}

/**
 * @brief 将失效区域扩展到偶数边界。
 *
 * RGB565 和当前总线链路在奇偶边界上更容易暴露传输伪影，
 * 因此在送入 flush 前统一把区域对齐到偶数像素边界。
 *
 * @param[in] e LVGL 事件对象。
 * @return 无返回值。
 */
static void lv_port_rounder_event_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    if (area == NULL)
    {
        return;
    }

    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;
    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;

    // 将更新区域对齐到偶数边界，避免 RGB565 / 总线传输中出现奇偶边界伪影。
    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}

/**
 * @brief LVGL flush 回调。
 *
 * 该入口负责根据区域高度决定是否启用进一步分块，并把完成通知统一延迟到底层 DMA 回调。
 *
 * @param[in] disp LVGL display 对象。
 * @param[in] area 待刷新的矩形区域，边界为闭区间。
 * @param[in] px_map 区域像素数据首地址，格式为 RGB565。
 * @return 无返回值。
 */
void lv_port_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_err_t ret = ESP_OK;

#if LV_PORT_CHUNKED_TRANSFER_ENABLE
    uint32_t area_height = area->y2 - area->y1 + 1;
    if (area_height > LV_PORT_FIXED_CHUNK_LINES)
    {
        // 预先计算分块总数，回调里按块递减到 0 再通知 flush 完成。
        s_flush_pending_count = (area_height + LV_PORT_FIXED_CHUNK_LINES - 1) / LV_PORT_FIXED_CHUNK_LINES;
        ret = lv_port_flush_area_chunked_simple(disp, area, px_map);
    }
    else
    {
        s_flush_pending_count = 1;
        ret = lv_port_flush_area_with_sync(disp, area, px_map);
    }
#else
    s_flush_pending_count = 1;
    ret = lv_port_flush_area_with_sync(disp, area, px_map);
#endif

    if (ret != ESP_OK)
    {
        ESP_LOGW(LV_PORT_TAG, "Display flush failed: %s", esp_err_to_name(ret));
        s_flush_pending_count = 0;
        lv_display_flush_ready(disp);
        return;
    }
}

/**
 * @brief 刷新单个区域并执行必要的同步处理。
 *
 * 当 TE 同步开启时，只在一帧的首个 chunk 等待 TE；
 * 这样既能减少撕裂，又避免每个 chunk 都等待导致刷新率显著下降。
 *
 * @param[in] disp LVGL display 对象，当前实现未直接使用。
 * @param[in] area 待刷新的矩形区域。
 * @param[in] px_map 区域像素数据首地址。
 * @return `ESP_OK` 表示提交成功；其他错误表示底层面板传输失败。
 */
static esp_err_t lv_port_flush_area_with_sync(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)disp;
#if CO5300_PANEL_USE_TE_SIGNAL
    if (s_frame_ctx.frame_start)
    {
        ESP_LOGV(LV_PORT_TAG, "Frame start, waiting for TE signal...");

        esp_err_t te_ret = co5300_panel_wait_te_signal(100);
        if (te_ret == ESP_OK)
        {
            s_frame_ctx.te_sync_count++;
            ESP_LOGV(LV_PORT_TAG, "TE sync OK");
        }
        else
        {
            s_frame_ctx.te_timeout_count++;
            ESP_LOGD(LV_PORT_TAG, "TE timeout (frame start)");
        }

        s_frame_ctx.frame_start = false;
    }

    s_frame_ctx.flush_count++;
#endif

    uint32_t pixel_count = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    uint32_t byte_size = pixel_count * sizeof(uint16_t);
    uint8_t *tx_px_map = px_map;

    if (esp_ptr_external_ram(px_map) && byte_size <= s_bounce_buf_size)
    {
        uint16_t *buf = s_bounce_bufs[s_bounce_buf_idx];
        if (buf != NULL)
        {
            memcpy(buf, px_map, byte_size);
            tx_px_map = (uint8_t *)buf;
            s_bounce_buf_idx = (s_bounce_buf_idx + 1) % 2;
        }
    }

    if (s_byte_swap_enabled)
    {
        lv_draw_sw_rgb565_swap(tx_px_map, pixel_count);
    }

    // `esp_lcd_panel_draw_bitmap()` 的右下角坐标是开区间，因此 `x2/y2` 需要额外 `+1`。
    esp_err_t ret = esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, tx_px_map);
    return ret;
}

/**
 * @brief 将较高的刷新区域拆成多块顺序发送。
 *
 * 顺序分块的目的不是单纯减少拷贝量，而是降低单次传输尺寸，
 * 从而避免面板链路在 DMA 私有缓冲紧张时放大失败概率。
 *
 * @param[in] disp LVGL display 对象。
 * @param[in] area 原始待刷新区域。
 * @param[in] px_map 原始像素首地址。
 * @return `ESP_OK` 表示所有分块都提交成功；其他错误表示其中某一块提交失败。
 */
static esp_err_t lv_port_flush_area_chunked_simple(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t area_width = area->x2 - area->x1 + 1;
    uint32_t area_height = area->y2 - area->y1 + 1;
    uint32_t bytes_per_line = area_width * sizeof(uint16_t);
    uint32_t chunk_lines = LV_PORT_FIXED_CHUNK_LINES;

    ESP_LOGD(LV_PORT_TAG, "Chunked transfer: %lux%lu area, %lu lines per chunk", area_width, area_height, chunk_lines);

    for (uint32_t y_offset = 0; y_offset < area_height; y_offset += chunk_lines)
    {
        // 当前分块实际行数：最后一块可能小于 `chunk_lines`。
        uint32_t current_chunk_lines =
            (y_offset + chunk_lines > area_height) ? (area_height - y_offset) : chunk_lines;

        lv_area_t chunk_area = {
            .x1 = area->x1,
            .y1 = area->y1 + y_offset,
            .x2 = area->x2,
            .y2 = area->y1 + y_offset + current_chunk_lines - 1,
        };

        uint8_t *chunk_px_map = px_map + (y_offset * bytes_per_line);
        esp_err_t ret = lv_port_flush_area_with_sync(disp, &chunk_area, chunk_px_map);
        if (ret != ESP_OK)
        {
            ESP_LOGE(LV_PORT_TAG, "Chunk transfer failed at y_offset %lu", (unsigned long)y_offset);
            return ret;
        }
    }

    return ESP_OK;
}

/**
 * @brief 初始化底层 CO5300 面板并缓存通用 panel 句柄。
 * @return 无返回值。
 */
void lv_port_panel_init(void)
{
    // 初始化 CO5300 面板并提取通用 panel 句柄，供 LVGL flush 路径直接使用。
    if (co5300_panel_init() == ESP_OK)
    {
        struct esp_lcd_panel_io_t *io = NULL;
        struct esp_lcd_panel_t *panel = NULL;
        if (co5300_panel_get_raw(&io, &panel) == ESP_OK)
        {
            s_panel = (esp_lcd_panel_handle_t)panel;
            // 这里的 gap 需要和面板实际可视区域对齐，错误的偏移会表现为显示和触摸热点整体错位。
            ESP_LOGI(LV_PORT_TAG, "设置显示向右偏移20像素");
            esp_err_t ret = esp_lcd_panel_set_gap(s_panel, 23, 0);
            if (ret != ESP_OK)
            {
                ESP_LOGE(LV_PORT_TAG, "设置显示偏移失败: %s", esp_err_to_name(ret));
            }
            else
            {
                ESP_LOGI(LV_PORT_TAG, "CO5300 面板初始化完成");
            }
        }
        else
        {
            ESP_LOGE(LV_PORT_TAG, "获取面板句柄失败");
        }
    }
    else
    {
        ESP_LOGE(LV_PORT_TAG, "CO5300 面板初始化失败");
    }
}
