#ifndef MINI_GAMES_PROGRESS_H
#define MINI_GAMES_PROGRESS_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 需要持久化最高分的小游戏标识。 */
typedef enum {
    MINI_GAMES_PROGRESS_2048 = 0,
    MINI_GAMES_PROGRESS_FLAPPY,
    MINI_GAMES_PROGRESS_DINO,
    MINI_GAMES_PROGRESS_COUNT,
} mini_games_progress_id_t;

/**
 * @brief 启动小游戏最高分 owner。
 *
 * 该接口只创建 FreeRTOS queue/task，不在 LVGL 调用线程执行 NVS I/O。
 * NVS worker 使用 internal RAM 栈，因为 Flash 写入期间 cache 可能被禁用。
 *
 * @return ESP_OK 表示 owner 已运行或启动成功。
 */
esp_err_t mini_games_progress_start(void);

/**
 * @brief 读取内存中的最高分快照。
 *
 * @param[in] game_id 游戏标识。
 * @return 当前最高分；owner 尚未加载完成时返回本次运行期已知值。
 */
uint32_t mini_games_progress_get_high_score(
    mini_games_progress_id_t game_id);

/**
 * @brief 提交可能刷新的最高分，并异步请求 NVS 持久化。
 *
 * 低于或等于现有最高分的值不会触发 Flash 写入。
 *
 * @param[in] game_id 游戏标识。
 * @param[in] score 本局得分。
 */
void mini_games_progress_submit_high_score(
    mini_games_progress_id_t game_id, uint32_t score);

#ifdef __cplusplus
}
#endif

#endif /* MINI_GAMES_PROGRESS_H */
