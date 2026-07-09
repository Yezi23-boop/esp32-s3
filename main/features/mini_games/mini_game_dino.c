#include "features/mini_games/mini_game_dino.h"
#include <string.h>

static const float kGravity = 0.45f;
static const float kJumpImpulse = -7.2f;
static const float kInitialSpeed = 3.0f;
static const uint32_t kDefaultSeed = 0x87654321U;

static uint32_t mini_game_dino_next_random(mini_game_dino_t *game)
{
    uint32_t x = game->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    game->rng_state = x != 0U ? x : kDefaultSeed;
    return game->rng_state;
}

static void mini_game_dino_randomize_obstacle(mini_game_dino_t *game, dino_obstacle_t *obs, float start_x)
{
    obs->x = start_x;
    obs->passed = false;
    obs->active = true;

    // 如果分数 >= 30，有 30% 概率生成翼龙障碍，否则只生成仙人掌
    if (game->score >= 30 && (mini_game_dino_next_random(game) % 10) < 3) {
        obs->type = DINO_OBSTACLE_PTERODACTYL;
        obs->w = 24;
        obs->h = 20;
    } else {
        obs->type = DINO_OBSTACLE_CACTUS;
        // 随机判定是单棵 (40% 宽12..16)、双连排 (40% 宽20..26) 还是三连排 (20% 宽30..35)
        uint32_t rand_val = mini_game_dino_next_random(game) % 100;
        if (rand_val < 40) {
            obs->w = (int16_t)(12 + (mini_game_dino_next_random(game) % 5)); // 单棵: 12..16
        } else if (rand_val < 80) {
            obs->w = (int16_t)(20 + (mini_game_dino_next_random(game) % 7)); // 双连排: 20..26
        } else {
            obs->w = (int16_t)(30 + (mini_game_dino_next_random(game) % 6)); // 三连排: 30..35
        }
        obs->h = (int16_t)(20 + (mini_game_dino_next_random(game) % 13)); // 20..32
    }
}

void mini_game_dino_init(mini_game_dino_t *game, uint32_t seed)
{
    mini_game_dino_reset(game, seed);
}

void mini_game_dino_reset(mini_game_dino_t *game, uint32_t seed)
{
    if (game == NULL) {
        return;
    }

    memset(game, 0, sizeof(*game));
    game->rng_state = seed != 0U ? seed : kDefaultSeed;
    game->dino_y = (float)(DINO_GROUND_Y - DINO_HEIGHT);
    game->dino_vy = 0.0f;
    game->is_jumping = false;
    game->score = 0;
    game->game_over = false;
    game->frame_count = 0;
    game->speed = kInitialSpeed;

    for (uint8_t i = 0; i < DINO_MAX_OBSTACLES; ++i) {
        mini_game_dino_randomize_obstacle(game, &game->obstacles[i], (float)(DINO_PLAY_AREA_W + i * 220));
    }
}

void mini_game_dino_jump(mini_game_dino_t *game)
{
    if (game == NULL || game->game_over || game->is_jumping) {
        return;
    }
    game->dino_vy = kJumpImpulse;
    game->is_jumping = true;
    game->is_ducking = false;
    game->duck_timer = 0;
}

void mini_game_dino_duck(mini_game_dino_t *game)
{
    if (game == NULL || game->game_over) {
        return;
    }
    if (game->is_jumping) {
        // 空中下蹲：触发快速下坠 (Fast Fall)
        game->dino_vy += 3.5f;
        game->is_ducking = true;
        game->duck_timer = 15; // 落地后保持下蹲一定时间
    } else {
        // 地面下蹲：保持20帧
        game->is_ducking = true;
        game->duck_timer = 20;
    }
}

void mini_game_dino_step(mini_game_dino_t *game)
{
    if (game == NULL || game->game_over) {
        return;
    }

    game->frame_count++;

    // 更新下蹲时间
    if (game->is_ducking) {
        if (game->duck_timer > 0) {
            game->duck_timer--;
        } else {
            game->is_ducking = false;
        }
    }

    // 1. 步进恐龙物理状态
    if (game->is_jumping) {
        game->dino_y += game->dino_vy;
        game->dino_vy += kGravity;

        // 落地检测
        const float ground_limit = (float)(DINO_GROUND_Y - DINO_HEIGHT);
        if (game->dino_y >= ground_limit) {
            game->dino_y = ground_limit;
            game->dino_vy = 0.0f;
            game->is_jumping = false;
            // 若不是由空中划蹲落地，则退出下蹲
            if (game->duck_timer == 0) {
                game->is_ducking = false;
            }
        }
    }

    // 2. 随分数逐渐加速，增加挑战性
    game->speed = kInitialSpeed + (float)(game->score / 50) * 0.2f;
    if (game->speed > 6.0f) {
        game->speed = 6.0f;
    }

    // 3. 移动障碍物并判定碰撞、计分
    for (uint8_t i = 0; i < DINO_MAX_OBSTACLES; ++i) {
        dino_obstacle_t *obs = &game->obstacles[i];
        if (!obs->active) {
            continue;
        }

        obs->x -= game->speed;

        // 滑出屏幕后重置到右侧
        if (obs->x + obs->w < 0.0f) {
            float max_x = 0.0f;
            for (uint8_t j = 0; j < DINO_MAX_OBSTACLES; ++j) {
                if (game->obstacles[j].x > max_x) {
                    max_x = game->obstacles[j].x;
                }
            }
            // 间距加上随机量
            const float gap = 180.0f + (float)(mini_game_dino_next_random(game) % 100);
            mini_game_dino_randomize_obstacle(game, obs, max_x + gap);
            continue;
        }

        // 碰撞判断
        const float dino_x1 = (float)DINO_X;
        const float dino_x2 = (float)(DINO_X + DINO_WIDTH);
        const float dino_y1 = game->is_ducking ? game->dino_y + 12.0f : game->dino_y;
        const float dino_y2 = game->dino_y + (float)DINO_HEIGHT;

        const float obs_x1 = obs->x;
        const float obs_x2 = obs->x + (float)obs->w;
        
        float obs_y1, obs_y2;
        if (obs->type == DINO_OBSTACLE_PTERODACTYL) {
            // 中空翼龙悬空 14 像素，恐龙站立时会撞上，下蹲（高度降为 12）时可安全钻过
            obs_y2 = (float)(DINO_GROUND_Y - 14);
            obs_y1 = (float)(DINO_GROUND_Y - 14 - obs->h);
        } else {
            obs_y2 = (float)DINO_GROUND_Y;
            obs_y1 = (float)(DINO_GROUND_Y - obs->h);
        }

        // X轴重合
        if (dino_x2 > obs_x1 && dino_x1 < obs_x2) {
            // Y轴重合
            if (dino_y2 > obs_y1 && dino_y1 < obs_y2) {
                game->game_over = true;
                return;
            }
        }

        // 计分判定：成功跳过障碍
        if (!obs->passed && obs_x2 < dino_x1) {
            obs->passed = true;
            game->score += 10;
        }
    }
}

uint32_t mini_game_dino_get_score(const mini_game_dino_t *game)
{
    return game != NULL ? game->score : 0;
}

bool mini_game_dino_is_game_over(const mini_game_dino_t *game)
{
    return game != NULL ? game->game_over : true;
}
