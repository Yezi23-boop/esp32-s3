#include "features/mini_games/mini_game_flappy.h"
#include <string.h>

static const float kGravity = 0.32f;
static const float kJumpImpulse = -5.0f;
static const float kPipeSpeed = 2.0f;
static const float kPipeGap = 100.0f;
static const int16_t kMinPipeHeight = 40;
static const uint32_t kDefaultSeed = 0x12345678U;

static uint32_t mini_game_flappy_next_random(mini_game_flappy_t *game)
{
    uint32_t x = game->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    game->rng_state = x != 0U ? x : kDefaultSeed;
    return game->rng_state;
}

static void mini_game_flappy_randomize_pipe(mini_game_flappy_t *game, flappy_pipe_t *pipe, float start_x)
{
    pipe->x = start_x;
    pipe->passed = false;
    pipe->active = true;

    // 随机计算上管道的高度
    const int16_t max_top_h = (int16_t)(FLAPPY_PLAY_AREA_H - kPipeGap - kMinPipeHeight);
    const int16_t range = (int16_t)(max_top_h - kMinPipeHeight + 1);
    const int16_t top_h = (int16_t)(kMinPipeHeight + (mini_game_flappy_next_random(game) % range));

    pipe->top_h = (float)top_h;
    pipe->bottom_y = (float)top_h + kPipeGap;
}

void mini_game_flappy_init(mini_game_flappy_t *game, uint32_t seed)
{
    mini_game_flappy_reset(game, seed);
}

void mini_game_flappy_reset(mini_game_flappy_t *game, uint32_t seed)
{
    if (game == NULL) {
        return;
    }

    memset(game, 0, sizeof(*game));
    game->rng_state = seed != 0U ? seed : kDefaultSeed;
    game->bird_y = (float)(FLAPPY_PLAY_AREA_H / 2);
    game->bird_vy = 0.0f;
    game->score = 0;
    game->started = false;
    game->game_over = false;
    game->frame_count = 0;

    // 顺序排列并随机化三对管道的初始位置
    for (uint8_t i = 0; i < FLAPPY_MAX_PIPES; ++i) {
        mini_game_flappy_randomize_pipe(game, &game->pipes[i], (float)(FLAPPY_PLAY_AREA_W + i * 160));
    }
}

void mini_game_flappy_jump(mini_game_flappy_t *game)
{
    if (game == NULL || game->game_over) {
        return;
    }
    game->started = true;
    game->bird_vy = kJumpImpulse;
}

void mini_game_flappy_step(mini_game_flappy_t *game)
{
    if (game == NULL || game->game_over || !game->started) {
        return;
    }

    game->frame_count++;

    // 1. 步进小鸟状态
    game->bird_y += game->bird_vy;
    game->bird_vy += kGravity;

    // 边界检测
    if (game->bird_y <= 0.0f) {
        game->bird_y = 0.0f;
        game->bird_vy = 0.0f;
    }
    if (game->bird_y >= (float)(FLAPPY_PLAY_AREA_H - FLAPPY_BIRD_SIZE)) {
        game->bird_y = (float)(FLAPPY_PLAY_AREA_H - FLAPPY_BIRD_SIZE);
        game->game_over = true;
        return;
    }

    // 2. 移动管道并判断碰撞、计分
    for (uint8_t i = 0; i < FLAPPY_MAX_PIPES; ++i) {
        flappy_pipe_t *pipe = &game->pipes[i];
        if (!pipe->active) {
            continue;
        }

        pipe->x -= kPipeSpeed;

        // 如果管道移出屏幕，循环并生成到最右侧
        if (pipe->x + FLAPPY_PIPE_WIDTH < 0.0f) {
            // 找出当前最右侧管道的 x 坐标
            float max_x = 0.0f;
            for (uint8_t j = 0; j < FLAPPY_MAX_PIPES; ++j) {
                if (game->pipes[j].x > max_x) {
                    max_x = game->pipes[j].x;
                }
            }
            mini_game_flappy_randomize_pipe(game, pipe, max_x + 160.0f);
            continue;
        }

        // 碰撞判断（将小鸟看作边长为 FLAPPY_BIRD_SIZE 的正方形，管道是矩形）
        const float bird_x1 = (float)FLAPPY_BIRD_X;
        const float bird_x2 = (float)(FLAPPY_BIRD_X + FLAPPY_BIRD_SIZE);
        const float bird_y1 = game->bird_y;
        const float bird_y2 = game->bird_y + (float)FLAPPY_BIRD_SIZE;

        const float pipe_x1 = pipe->x;
        const float pipe_x2 = pipe->x + (float)FLAPPY_PIPE_WIDTH;

        // X轴重合判断
        if (bird_x2 > pipe_x1 && bird_x1 < pipe_x2) {
            // Y轴重合判断（碰上管或碰下管）
            if (bird_y1 < pipe->top_h || bird_y2 > pipe->bottom_y) {
                game->game_over = true;
                return;
            }
        }

        // 计分判定：通过管道的 x
        if (!pipe->passed && pipe_x2 < bird_x1) {
            pipe->passed = true;
            game->score++;
        }
    }
}

uint32_t mini_game_flappy_get_score(const mini_game_flappy_t *game)
{
    return game != NULL ? game->score : 0;
}

bool mini_game_flappy_is_game_over(const mini_game_flappy_t *game)
{
    return game != NULL ? game->game_over : true;
}

bool mini_game_flappy_is_started(const mini_game_flappy_t *game)
{
    return game != NULL && game->started;
}
