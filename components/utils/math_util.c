#include "math_util.h"

int32_t math_map(int32_t x, int32_t min_in, int32_t max_in,
                 int32_t min_out, int32_t max_out)
{
    if (max_in >= min_in && x >= max_in) return max_out;
    if (max_in >= min_in && x <= min_in) return min_out;

    if (max_in <= min_in && x <= max_in) return max_out;
    if (max_in <= min_in && x >= min_in) return min_out;

    return ((x - min_in) * (max_out - min_out)) / (max_in - min_in) + min_out;
}
