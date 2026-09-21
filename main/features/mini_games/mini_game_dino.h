#ifndef MINI_GAME_DINO_H
#define MINI_GAME_DINO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DINO_MAX_OBSTACLES 2
#define DINO_PLAY_AREA_H 420
#define DINO_PLAY_AREA_W 410
#define DINO_GROUND_Y 374
#define DINO_X 60
#define DINO_WIDTH 20
#define DINO_HEIGHT 24

typedef enum {
    DINO_OBSTACLE_CACTUS = 0,
    DINO_OBSTACLE_PTERODACTYL
} dino_obstacle_type_t;

typedef struct {
    float x;
    int16_t w;
    int16_t h;
    bool active;
    bool passed;
    uint8_t type;
} dino_obstacle_t;

typedef struct {
    float dino_y;
    float dino_vy;
    bool started;
    bool is_jumping;
    bool is_ducking;
    uint32_t score;
    bool game_over;
    uint32_t rng_state;
    dino_obstacle_t obstacles[DINO_MAX_OBSTACLES];
    uint32_t frame_count;
    float speed;
} mini_game_dino_t;

void mini_game_dino_init(mini_game_dino_t *game, uint32_t seed);
void mini_game_dino_reset(mini_game_dino_t *game, uint32_t seed);
void mini_game_dino_jump(mini_game_dino_t *game);
/**
 * @brief 设置按住下蹲状态。
 *
 * 空中首次按下会附加一次快速下坠；松开后立即恢复站立状态。
 *
 * @param[in,out] game 游戏状态对象。
 * @param[in] pressed true 表示按住左半屏，false 表示松开。
 */
void mini_game_dino_set_ducking(mini_game_dino_t *game, bool pressed);
void mini_game_dino_step(mini_game_dino_t *game);
uint32_t mini_game_dino_get_score(const mini_game_dino_t *game);
bool mini_game_dino_is_game_over(const mini_game_dino_t *game);
bool mini_game_dino_is_started(const mini_game_dino_t *game);

#ifdef __cplusplus
}
#endif

#endif /* MINI_GAME_DINO_H */
