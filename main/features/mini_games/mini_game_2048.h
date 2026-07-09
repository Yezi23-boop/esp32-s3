#ifndef MINI_GAME_2048_H
#define MINI_GAME_2048_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MINI_GAME_2048_SIZE 4
#define MINI_GAME_2048_CELL_COUNT \
    (MINI_GAME_2048_SIZE * MINI_GAME_2048_SIZE)

typedef enum {
    MINI_GAME_2048_DIRECTION_UP = 0,
    MINI_GAME_2048_DIRECTION_DOWN,
    MINI_GAME_2048_DIRECTION_LEFT,
    MINI_GAME_2048_DIRECTION_RIGHT,
} mini_game_2048_direction_t;

typedef struct {
    bool moved;
    bool game_over;
    uint32_t gained_score;
} mini_game_2048_move_result_t;

typedef struct {
    uint16_t cells[MINI_GAME_2048_CELL_COUNT];
    uint32_t score;
    uint32_t rng_state;
    bool game_over;
} mini_game_2048_t;

/**
 * @brief 初始化一局新的 2048。
 *
 * @param[out] game 游戏状态对象。
 * @param[in] seed 伪随机种子；传 0 时使用固定非零种子，便于测试复现。
 */
void mini_game_2048_init(mini_game_2048_t *game, uint32_t seed);

/**
 * @brief 重置为新局并生成初始方块。
 *
 * @param[in,out] game 游戏状态对象。
 * @param[in] seed 伪随机种子；传 0 时使用固定非零种子。
 */
void mini_game_2048_reset(mini_game_2048_t *game, uint32_t seed);

/**
 * @brief 执行一次方向移动。
 *
 * 只有棋盘发生变化时才生成一个新方块；无效移动不会改变棋盘或分数。
 *
 * @param[in,out] game 游戏状态对象。
 * @param[in] direction 移动方向。
 * @return 本次移动结果。
 */
mini_game_2048_move_result_t mini_game_2048_move(
    mini_game_2048_t *game, mini_game_2048_direction_t direction);

/**
 * @brief 获取指定格子的值。
 *
 * @param[in] game 游戏状态对象。
 * @param[in] row 行号，范围 0..3。
 * @param[in] col 列号，范围 0..3。
 * @return 格子数值；参数非法时返回 0。
 */
uint16_t mini_game_2048_get_cell(const mini_game_2048_t *game, uint8_t row,
                                 uint8_t col);

/**
 * @brief 获取当前分数。
 *
 * @param[in] game 游戏状态对象。
 * @return 当前分数；参数非法时返回 0。
 */
uint32_t mini_game_2048_get_score(const mini_game_2048_t *game);

/**
 * @brief 判断当前棋盘是否已经结束。
 *
 * @param[in] game 游戏状态对象。
 * @return true 表示没有空格且没有相邻可合并格子。
 */
bool mini_game_2048_is_game_over(const mini_game_2048_t *game);

/**
 * @brief 直接装载棋盘，用于 source tests 或后续恢复局面。
 *
 * @param[out] game 游戏状态对象。
 * @param[in] cells 16 个格子的行优先数组。
 * @param[in] score 初始分数。
 * @param[in] seed 伪随机种子；传 0 时使用固定非零种子。
 */
void mini_game_2048_load_board(mini_game_2048_t *game,
                               const uint16_t cells[MINI_GAME_2048_CELL_COUNT],
                               uint32_t score, uint32_t seed);

#ifdef __cplusplus
}
#endif

#endif /* MINI_GAME_2048_H */
