#ifndef SCROLL_FUNCTIONS_H
#define SCROLL_FUNCTIONS_H

// 滚动功能模块头文件
// 提供基于圆弧模型的视觉滚动效果

#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // 滚动方向枚举
    typedef enum
    {
        VERTICAL,  // 垂直滚动
        HORIZONTAL // 水平滚动
    } ScrollDirection;

    /**
     * @brief 设置垂直滚动效果
     * @param cont 目标容器对象
     *
     * 功能：子项垂直排列，基于圆弧模型的水平位移和透明度渐变效果
     * 注意：容器必须包含子项才能看到效果
     */
    void setup_vertical_scroll(lv_obj_t *cont);

    /**
     * @brief 设置水平滚动效果
     * @param cont 目标容器对象
     *
     * 功能：子项水平排列，基于圆弧模型的垂直位移和透明度渐变效果
     * 注意：仅对图片按钮类子项生效
     */
    void setup_horizontal_scroll(lv_obj_t *cont);

    /**
     * @brief 应用弧形滚动效果
     * @param cont 目标容器对象
     * @param direction 滚动方向
     *
     * 使用场景：事件处理中手动触发效果或外部控制滚动时机
     */
    void apply_arc_scroll_effect(lv_obj_t *cont, ScrollDirection direction);

#ifdef __cplusplus
}
#endif

#endif // SCROLL_FUNCTIONS_H