#ifndef MINI_GAME_FLAPPY_H
#define MINI_GAME_FLAPPY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLAPPY_MAX_PIPES 3
#define FLAPPY_PLAY_AREA_H 340
#define FLAPPY_PLAY_AREA_W 360
#define FLAPPY_PIPE_WIDTH 50
#define FLAPPY_BIRD_X 80
#define FLAPPY_BIRD_SIZE 20

typedef struct {
    float x;
    float top_h;
    float bottom_y;
    bool passed;
    bool active;
} flappy_pipe_t;

typedef struct {
    float bird_y;
    float bird_vy;
    uint32_t score;
    bool game_over;
    uint32_t rng_state;
    flappy_pipe_t pipes[FLAPPY_MAX_PIPES];
    uint32_t frame_count;
} mini_game_flappy_t;

void mini_game_flappy_init(mini_game_flappy_t *game, uint32_t seed);
void mini_game_flappy_reset(mini_game_flappy_t *game, uint32_t seed);
void mini_game_flappy_jump(mini_game_flappy_t *game);
void mini_game_flappy_step(mini_game_flappy_t *game);
uint32_t mini_game_flappy_get_score(const mini_game_flappy_t *game);
bool mini_game_flappy_is_game_over(const mini_game_flappy_t *game);

#ifdef __cplusplus
}
#endif

#endif /* MINI_GAME_FLAPPY_H */
