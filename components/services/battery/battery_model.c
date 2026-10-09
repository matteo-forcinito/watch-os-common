#include "services/battery_health.h"

/*
 * The health model: rules of thumb for LiPo cells, kept simple on purpose.
 * Pure C, no ESP-IDF: tested on the PC.
 */

#define CYCLES_TO_80   400   /* full cycles to lose 20 % */
#define AGE_LOSS_YEAR  3     /* % per year of calendar ageing */

static int clamp(int value)
{
    return value < 0 ? 0 : (value > 100 ? 100 : value);
}

int battery_health_from_cycles(uint32_t cycles_x10)
{
    /* 100 % new, -20 % every CYCLES_TO_80 cycles */
    return clamp(100 - (int)((cycles_x10 * 20) / (CYCLES_TO_80 * 10)));
}

int battery_health_from_age(int days)
{
    if (days < 0)
    {
        return BATTERY_UNKNOWN;
    }

    return clamp(100 - days * AGE_LOSS_YEAR / 365);
}

/*
 * The internal resistance roughly doubles by the time a cell is down to
 * ~80 % capacity: a drop 1.5x the first one reads as ~90 %, 2x as ~80 %.
 */
int battery_health_from_resistance(int first_drop_mv, int last_drop_mv)
{
    if (first_drop_mv <= 0 || last_drop_mv <= 0)
    {
        return BATTERY_UNKNOWN;
    }

    int ratio_x100 = last_drop_mv * 100 / first_drop_mv;

    if (ratio_x100 <= 100)
    {
        return 100;
    }

    return clamp(100 - (ratio_x100 - 100) / 5);
}
