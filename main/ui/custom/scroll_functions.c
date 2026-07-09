/*
 * 滚动功能模块 - 高内聚设计
 * 负责所有滚动动效相关的功能
 * -----------------------------------------------------------------------------
 * 设计原则：高内聚、低耦合
 * - 统一管理所有滚动效果
 * - 提供一致的接口规范
 * - 支持多种滚动方向
 */

#include "scroll_functions.h"
#include "lvgl.h"
#include "gui_guider.h"
#include <string.h>

// 内部使用的全局变量（模块私有）
static uint8_t imgsName[4][20]; // 存储图片名称字符串的二维数组

#if LV_USE_FLEX

/**
 * 垂直滚动效果 - 事件回调函数（私有）
 *
 * @param e LVGL事件对象
 *
 * 功能说明：
 * - 计算每个子项的水平位移和透明度
 * - 基于圆弧模型实现视觉纵深感
 * - 仅处理垂直方向的滚动效果
 */
static void vertical_scroll_event_cb(lv_event_t *e)
{
    lv_obj_t *cont = lv_event_get_target(e); /* 获取触发事件的容器对象 */

    lv_area_t cont_a;                                                       /* 容器坐标区域 */
    lv_obj_get_coords(cont, &cont_a);                                       /* 读取绝对坐标 */
    lv_coord_t cont_y_center = cont_a.y1 + lv_area_get_height(&cont_a) / 2; /* 容器垂直中心 */

    lv_coord_t r = lv_obj_get_width(cont) * 7 / 10;  /* 半径设置（效果强度），按容器宽度 */
    uint32_t i;                                      /* 循环变量 */
    uint32_t child_cnt = lv_obj_get_child_cnt(cont); /* 子项数量 */

    for (i = 0; i < child_cnt; i++)
    {
        lv_obj_t *child = lv_obj_get_child(cont, i); /* 第 i 个子项 */
        lv_area_t child_a;                           /* 子项区域 */
        lv_obj_get_coords(child, &child_a);          /* 获取子项绝对坐标 */

        lv_coord_t child_y_center = child_a.y1 + lv_area_get_height(&child_a) / 2; /* 子项中心 */
        lv_coord_t diff_y = LV_ABS(child_y_center - cont_y_center);                /* 与容器中心的垂直距离 */

        lv_coord_t x; /* 水平位移量 */
        if (diff_y >= r)
            x = r; /* 超出半径取上限 */
        else
        {
            uint32_t x_sqr = r * r - diff_y * diff_y; /* x^2 = r^2 - y^2 */
            lv_sqrt_res_t res;                        /* 开方结果 */
            lv_sqrt(x_sqr, &res, 0x8000);             /* 内置开方 */
            x = r - res.i;                            /* x = r - sqrt(x_sqr) */
        }

        lv_obj_set_style_translate_x(child, x + 30, 0);              /* 子项水平平移（整体右移30像素） */
        lv_opa_t opa = lv_map(x, 0, r, LV_OPA_TRANSP, LV_OPA_COVER); /* 将位移映射为透明度 */
        lv_obj_set_style_opa(child, LV_OPA_COVER - opa, 0);          /* 位移越大越透明 */
    }
}

/**
 * 水平滚动效果 - 事件回调函数（私有）
 *
 * @param e LVGL事件对象
 *
 * 功能说明：
 * - 计算每个子项的垂直位移和透明度
 * - 基于圆弧模型实现视觉纵深感
 * - 仅处理水平方向的滚动效果
 * - 支持图片按钮标签更新
 */
static void horizontal_scroll_event_cb(lv_event_t *e)
{
    lv_obj_t *cont = lv_event_get_target(e); /* 获取触发事件的容器（现有 UI 的容器） */

    lv_area_t cont_a;                                                      /* 容器坐标区域 */
    lv_obj_get_coords(cont, &cont_a);                                      /* 读取绝对坐标 */
    lv_coord_t cont_x_center = cont_a.x1 + lv_area_get_width(&cont_a) / 2; /* 容器水平中心 */

    lv_coord_t r = lv_obj_get_height(cont) * 5 / 10; /* 半径设置（效果强度），按容器高度 */
    uint32_t i;                                      /* 循环变量 */
    uint32_t child_cnt = lv_obj_get_child_cnt(cont); /* 子项数量 */

    for (i = 0; i < child_cnt; i++)
    {
        lv_obj_t *child = lv_obj_get_child(cont, i); /* 第 i 个子项 */
        if (lv_obj_get_class(child) != &lv_image_class)
        {
            continue; // 不是图片按钮，跳过处理
        }
        lv_area_t child_a;                  /* 子项区域 */
        lv_obj_get_coords(child, &child_a); /* 获取子项绝对坐标 */

        lv_coord_t child_x_center = child_a.x1 + lv_area_get_width(&child_a) / 2; /* 子项中心 */
        lv_coord_t diff_x = LV_ABS(child_x_center - cont_x_center);               /* 与容器中心的水平距离 */

        lv_coord_t y; /* 垂直位移量 */
        if (diff_x >= r)
            y = r; /* 超出半径取上限 */
        else
        {
            uint32_t y_sqr = r * r - diff_x * diff_x; /* y^2 = r^2 - x^2 */
            lv_sqrt_res_t res;                        /* 开方结果 */
            lv_sqrt(y_sqr, &res, 0x8000);             /* 内置开方 */
            y = r - res.i;                            /* y = r - sqrt(y_sqr) */
        }

        /* 防抖：仅在中心项变化时更新标签与动画，避免卡顿 */
        static int last_center_idx = -1;
        if (diff_x == 0)
        {
            if (last_center_idx != (int)i)
            {
                last_center_idx = (int)i;
                // 清除同类型动画，避免动画堆积
                lv_anim_del(guider_ui.screen_wallpaper_label_1, (lv_anim_exec_xcb_t)lv_obj_set_y);
                // 更新标签文本为当前选中应用的名称
                lv_label_set_text(guider_ui.screen_wallpaper_label_1, (char *)imgsName[i]);
                // 标签位置动画：创建平滑的位置过渡效果（对标签执行）
                ui_animation(guider_ui.screen_wallpaper_label_1, 300, 0,
                             lv_obj_get_y(guider_ui.screen_wallpaper_label_1), -20,
                             &lv_anim_path_linear, 0, 0, 0, 0,
                             (lv_anim_exec_xcb_t)lv_obj_set_y, NULL, NULL, NULL);
            }
        }

        lv_obj_set_style_translate_y(child, y - 30, 0);              /* 子项垂直平移（整体上移30像素） */
        lv_opa_t opa = lv_map(y, 0, r, LV_OPA_TRANSP, LV_OPA_COVER); /* 将位移映射为透明度 */
        lv_obj_set_style_opa(child, LV_OPA_COVER - opa, 0);          /* 位移越大越透明 */
    }
}

#endif /* LV_USE_FLEX */

/**
 * 设置垂直滚动效果
 *
 * @param cont 容器对象
 *
 * 功能说明：
 * - 配置垂直方向的弹性布局
 * - 绑定垂直滚动事件回调
 * - 设置滚动吸附和裁剪效果
 */
void setup_vertical_scroll(lv_obj_t *cont)
{
    if (!cont)
        return;

    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);                            /* 列式弹性布局（纵向排布） */
    lv_obj_set_style_pad_row(cont, 20, LV_PART_MAIN);                           /* 设置子项行间距为20像素 */
    lv_obj_add_event_cb(cont, vertical_scroll_event_cb, LV_EVENT_SCROLL, NULL); /* 绑定滚动回调 */
    lv_obj_set_style_clip_corner(cont, true, 0);                                /* 超出裁剪，避免越界绘制 */
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);                                    /* 仅垂直方向滚动 */
    lv_obj_set_scroll_snap_y(cont, LV_SCROLL_SNAP_CENTER);                      /* 滚动吸附到容器垂直中心 */
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);                     /* 隐藏滚动条 */

    /* 初次手动刷新位置，确保动效立即生效 */
    lv_obj_send_event(cont, LV_EVENT_SCROLL, NULL);
    if (lv_obj_get_child_cnt(cont) > 0)
    {
        lv_obj_scroll_to_view(lv_obj_get_child(cont, 0), LV_ANIM_OFF);
    }
}

/**
 * 设置水平滚动效果
 *
 * @param cont 容器对象
 *
 * 功能说明：
 * - 配置水平方向的弹性布局
 * - 绑定水平滚动事件回调
 * - 设置应用名称标签和动画效果
 */
void setup_horizontal_scroll(lv_obj_t *cont)
{
    if (!cont)
        return;

    // 初始化应用名称数据
    strcpy((char *)imgsName[0], "小满");   // 视频应用
    strcpy((char *)imgsName[1], "小祁");   // 音乐应用
    strcpy((char *)imgsName[2], "Wechat"); // 微信应用
    strcpy((char *)imgsName[3], "music");  // 游戏应用

    // 设置标签初始文本为当前选中应用的名称
    lv_label_set_text(guider_ui.screen_wallpaper_label_1, (char *)imgsName[0]);
    // 设置标签位置：底部居中，向上偏移20像素
    lv_obj_align(guider_ui.screen_wallpaper_label_1, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_obj_add_event_cb(cont, horizontal_scroll_event_cb, LV_EVENT_SCROLL, NULL);                  /* 绑定滚动回调 */
    lv_obj_set_style_clip_corner(cont, true, 0);                                                   /* 超出裁剪，避免越界绘制 */
    lv_obj_set_scroll_dir(cont, LV_DIR_HOR);                                                       /* 仅水平方向滚动 */
    lv_obj_set_scroll_snap_x(cont, LV_SCROLL_SNAP_CENTER);                                         /* 滚动吸附到容器水平中心 */
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);                                        /* 隐藏滚动条 */
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER); // 子项居中

    /* 初次手动刷新位置，确保动效立即生效 */
    lv_obj_send_event(cont, LV_EVENT_SCROLL, NULL);
    if (lv_obj_get_child_cnt(cont) > 0)
    {
        lv_obj_scroll_to_view(lv_obj_get_child(cont, 0), LV_ANIM_OFF);
    }
}

/**
 * 应用弧形滚动效果（直接调用方式）
 *
 * @param cont 容器对象
 * @param direction 滚动方向（VERTICAL 或 HORIZONTAL）
 *
 * 功能说明：
 * - 不绑定事件，直接计算并应用效果
 * - 适合在GUI事件中单次调用
 * - 支持不同方向的滚动效果
 */
void apply_arc_scroll_effect(lv_obj_t *cont, ScrollDirection direction)
{
    if (!cont)
        return;

    if (direction == VERTICAL)
    {
        lv_area_t cont_a;                                                       /* 容器区域 */
        lv_obj_get_coords(cont, &cont_a);                                       /* 读取容器绝对坐标 */
        lv_coord_t cont_y_center = cont_a.y1 + lv_area_get_height(&cont_a) / 2; /* 计算容器中心 */

        lv_coord_t r = lv_obj_get_width(cont) * 7 / 10;  /* 半径（效果强度），按容器宽度计算 */
        uint32_t child_cnt = lv_obj_get_child_cnt(cont); /* 子项数量 */

        for (uint32_t i = 0; i < child_cnt; i++)
        {
            lv_obj_t *child = lv_obj_get_child(cont, i); /* 第 i 个子项 */
            lv_area_t child_a;                           /* 子项区域 */
            lv_obj_get_coords(child, &child_a);          /* 获取子项绝对坐标 */

            lv_coord_t child_y_center = child_a.y1 + lv_area_get_height(&child_a) / 2; /* 子项中心 */
            lv_coord_t diff_y = LV_ABS(child_y_center - cont_y_center);                /* 与容器中心的垂直距离 */

            lv_coord_t x; /* 水平位移量 */
            if (diff_y >= r)
                x = r; /* 超出半径取上限 */
            else
            {
                uint32_t x_sqr = r * r - diff_y * diff_y; /* x^2 = r^2 - y^2 */
                lv_sqrt_res_t res;                        /* 开方结果 */
                lv_sqrt(x_sqr, &res, 0x8000);             /* 内置开方 */
                x = r - res.i;                            /* x = r - sqrt(x_sqr) */
            }

            lv_obj_set_style_translate_x(child, x, 0);                   /* 应用水平位移 */
            lv_opa_t opa = lv_map(x, 0, r, LV_OPA_TRANSP, LV_OPA_COVER); /* 位移→透明度映射 */
            lv_obj_set_style_opa(child, LV_OPA_COVER - opa, 0);          /* 设置透明度 */
        }
    }
    else if (direction == HORIZONTAL)
    {
        lv_area_t cont_a;                                                      /* 容器区域 */
        lv_obj_get_coords(cont, &cont_a);                                      /* 读取容器绝对坐标 */
        lv_coord_t cont_x_center = cont_a.x1 + lv_area_get_width(&cont_a) / 2; /* 计算容器中心 */

        lv_coord_t r = lv_obj_get_height(cont) * 7 / 10; /* 半径（效果强度），按容器高度计算 */
        uint32_t child_cnt = lv_obj_get_child_cnt(cont); /* 子项数量 */

        for (uint32_t i = 0; i < child_cnt; i++)
        {
            lv_obj_t *child = lv_obj_get_child(cont, i); /* 第 i 个子项 */
            lv_area_t child_a;                           /* 子项区域 */
            lv_obj_get_coords(child, &child_a);          /* 获取子项绝对坐标 */

            lv_coord_t child_x_center = child_a.x1 + lv_area_get_width(&child_a) / 2; /* 子项中心 */
            lv_coord_t diff_x = LV_ABS(child_x_center - cont_x_center);               /* 与容器中心的水平距离 */

            lv_coord_t y; /* 垂直位移量 */
            if (diff_x >= r)
                y = r; /* 超出半径取上限 */
            else
            {
                uint32_t y_sqr = r * r - diff_x * diff_x; /* y^2 = r^2 - x^2 */
                lv_sqrt_res_t res;                        /* 开方结果 */
                lv_sqrt(y_sqr, &res, 0x8000);             /* 内置开方 */
                y = r - res.i;                            /* y = r - sqrt(y_sqr) */
            }

            lv_obj_set_style_translate_y(child, y, 0);                   /* 应用垂直位移 */
            lv_opa_t opa = lv_map(y, 0, r, LV_OPA_TRANSP, LV_OPA_COVER); /* 位移→透明度映射 */
            lv_obj_set_style_opa(child, LV_OPA_COVER - opa, 0);          /* 设置透明度 */
        }
    }
}