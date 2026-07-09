#include "features/mini_games/mini_game_2048.h"

#include <string.h>

static const uint32_t kDefaultSeed = 0x20482048U;
static const uint8_t kNewTileFourChancePercent = 10U;

static uint32_t mini_game_2048_normalize_seed(uint32_t seed)
{
    return seed != 0U ? seed : kDefaultSeed;
}

static uint32_t mini_game_2048_next_random(mini_game_2048_t *game)
{
    uint32_t x = game->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    game->rng_state = x != 0U ? x : kDefaultSeed;
    return game->rng_state;
}

static uint8_t mini_game_2048_index(uint8_t row, uint8_t col)
{
    return (uint8_t)(row * MINI_GAME_2048_SIZE + col);
}

static bool mini_game_2048_has_empty_cell(const mini_game_2048_t *game)
{
    for (uint8_t i = 0; i < MINI_GAME_2048_CELL_COUNT; ++i) {
        if (game->cells[i] == 0U) {
            return true;
        }
    }
    return false;
}

static bool mini_game_2048_can_merge_neighbor(const mini_game_2048_t *game)
{
    for (uint8_t row = 0; row < MINI_GAME_2048_SIZE; ++row) {
        for (uint8_t col = 0; col < MINI_GAME_2048_SIZE; ++col) {
            const uint16_t value =
                game->cells[mini_game_2048_index(row, col)];
            if (value == 0U) {
                continue;
            }
            if (col + 1U < MINI_GAME_2048_SIZE &&
                value == game->cells[mini_game_2048_index(row, col + 1U)]) {
                return true;
            }
            if (row + 1U < MINI_GAME_2048_SIZE &&
                value == game->cells[mini_game_2048_index(row + 1U, col)]) {
                return true;
            }
        }
    }
    return false;
}

static bool mini_game_2048_calculate_game_over(const mini_game_2048_t *game)
{
    return !mini_game_2048_has_empty_cell(game) &&
           !mini_game_2048_can_merge_neighbor(game);
}

static bool mini_game_2048_add_random_tile(mini_game_2048_t *game)
{
    uint8_t empty_count = 0;

    for (uint8_t i = 0; i < MINI_GAME_2048_CELL_COUNT; ++i) {
        if (game->cells[i] == 0U) {
            ++empty_count;
        }
    }
    if (empty_count == 0U) {
        return false;
    }

    const uint8_t selected =
        (uint8_t)(mini_game_2048_next_random(game) % empty_count);
    const uint16_t value =
        (mini_game_2048_next_random(game) % 100U) <
                kNewTileFourChancePercent
            ? 4U
            : 2U;

    uint8_t seen = 0;
    for (uint8_t i = 0; i < MINI_GAME_2048_CELL_COUNT; ++i) {
        if (game->cells[i] != 0U) {
            continue;
        }
        if (seen == selected) {
            game->cells[i] = value;
            return true;
        }
        ++seen;
    }

    return false;
}

static bool mini_game_2048_merge_line(const uint16_t in_line[MINI_GAME_2048_SIZE],
                                      uint16_t out_line[MINI_GAME_2048_SIZE],
                                      uint32_t *gained_score)
{
    uint16_t packed[MINI_GAME_2048_SIZE] = {0};
    uint8_t packed_count = 0;
    uint8_t out_count = 0;
    bool changed = false;

    for (uint8_t i = 0; i < MINI_GAME_2048_SIZE; ++i) {
        if (in_line[i] != 0U) {
            packed[packed_count++] = in_line[i];
        }
    }

    memset(out_line, 0, sizeof(uint16_t) * MINI_GAME_2048_SIZE);
    for (uint8_t i = 0; i < packed_count; ++i) {
        if (i + 1U < packed_count && packed[i] == packed[i + 1U]) {
            const uint16_t merged = (uint16_t)(packed[i] * 2U);
            out_line[out_count++] = merged;
            if (gained_score != NULL) {
                *gained_score += merged;
            }
            ++i;
        } else {
            out_line[out_count++] = packed[i];
        }
    }

    for (uint8_t i = 0; i < MINI_GAME_2048_SIZE; ++i) {
        if (in_line[i] != out_line[i]) {
            changed = true;
            break;
        }
    }

    return changed;
}

static void mini_game_2048_read_line(
    const mini_game_2048_t *game, mini_game_2048_direction_t direction,
    uint8_t line_index, uint16_t line[MINI_GAME_2048_SIZE])
{
    for (uint8_t i = 0; i < MINI_GAME_2048_SIZE; ++i) {
        switch (direction) {
            case MINI_GAME_2048_DIRECTION_LEFT:
                line[i] = game->cells[mini_game_2048_index(line_index, i)];
                break;
            case MINI_GAME_2048_DIRECTION_RIGHT:
                line[i] = game->cells[mini_game_2048_index(
                    line_index, MINI_GAME_2048_SIZE - 1U - i)];
                break;
            case MINI_GAME_2048_DIRECTION_UP:
                line[i] = game->cells[mini_game_2048_index(i, line_index)];
                break;
            case MINI_GAME_2048_DIRECTION_DOWN:
            default:
                line[i] = game->cells[mini_game_2048_index(
                    MINI_GAME_2048_SIZE - 1U - i, line_index)];
                break;
        }
    }
}

static void mini_game_2048_write_line(
    mini_game_2048_t *game, mini_game_2048_direction_t direction,
    uint8_t line_index, const uint16_t line[MINI_GAME_2048_SIZE])
{
    for (uint8_t i = 0; i < MINI_GAME_2048_SIZE; ++i) {
        switch (direction) {
            case MINI_GAME_2048_DIRECTION_LEFT:
                game->cells[mini_game_2048_index(line_index, i)] = line[i];
                break;
            case MINI_GAME_2048_DIRECTION_RIGHT:
                game->cells[mini_game_2048_index(
                    line_index, MINI_GAME_2048_SIZE - 1U - i)] = line[i];
                break;
            case MINI_GAME_2048_DIRECTION_UP:
                game->cells[mini_game_2048_index(i, line_index)] = line[i];
                break;
            case MINI_GAME_2048_DIRECTION_DOWN:
            default:
                game->cells[mini_game_2048_index(
                    MINI_GAME_2048_SIZE - 1U - i, line_index)] = line[i];
                break;
        }
    }
}

void mini_game_2048_init(mini_game_2048_t *game, uint32_t seed)
{
    mini_game_2048_reset(game, seed);
}

void mini_game_2048_reset(mini_game_2048_t *game, uint32_t seed)
{
    if (game == NULL) {
        return;
    }

    memset(game, 0, sizeof(*game));
    game->rng_state = mini_game_2048_normalize_seed(seed);
    (void)mini_game_2048_add_random_tile(game);
    (void)mini_game_2048_add_random_tile(game);
    game->game_over = mini_game_2048_calculate_game_over(game);
}

mini_game_2048_move_result_t mini_game_2048_move(
    mini_game_2048_t *game, mini_game_2048_direction_t direction)
{
    mini_game_2048_move_result_t result = {0};
    uint32_t gained_score = 0;

    if (game == NULL || game->game_over) {
        result.game_over = game != NULL ? game->game_over : true;
        return result;
    }

    for (uint8_t line_index = 0; line_index < MINI_GAME_2048_SIZE;
         ++line_index) {
        uint16_t in_line[MINI_GAME_2048_SIZE] = {0};
        uint16_t out_line[MINI_GAME_2048_SIZE] = {0};

        mini_game_2048_read_line(game, direction, line_index, in_line);
        if (mini_game_2048_merge_line(in_line, out_line, &gained_score)) {
            result.moved = true;
        }
        mini_game_2048_write_line(game, direction, line_index, out_line);
    }

    if (result.moved) {
        game->score += gained_score;
        result.gained_score = gained_score;
        (void)mini_game_2048_add_random_tile(game);
    }

    game->game_over = mini_game_2048_calculate_game_over(game);
    result.game_over = game->game_over;
    return result;
}

uint16_t mini_game_2048_get_cell(const mini_game_2048_t *game, uint8_t row,
                                 uint8_t col)
{
    if (game == NULL || row >= MINI_GAME_2048_SIZE ||
        col >= MINI_GAME_2048_SIZE) {
        return 0U;
    }

    return game->cells[mini_game_2048_index(row, col)];
}

uint32_t mini_game_2048_get_score(const mini_game_2048_t *game)
{
    return game != NULL ? game->score : 0U;
}

bool mini_game_2048_is_game_over(const mini_game_2048_t *game)
{
    return game != NULL ? game->game_over : true;
}

void mini_game_2048_load_board(mini_game_2048_t *game,
                               const uint16_t cells[MINI_GAME_2048_CELL_COUNT],
                               uint32_t score, uint32_t seed)
{
    if (game == NULL || cells == NULL) {
        return;
    }

    memset(game, 0, sizeof(*game));
    memcpy(game->cells, cells, sizeof(game->cells));
    game->score = score;
    game->rng_state = mini_game_2048_normalize_seed(seed);
    game->game_over = mini_game_2048_calculate_game_over(game);
}
