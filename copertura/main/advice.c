/* Copertura's judgement (see advice.h). Freestanding: no libc calls. */
#include "advice.h"

#define REPEATER_TARGET (-62)

cp_grade_t cp_grade(int rssi, int ping_ms, int loss)
{
    int g = rssi >= -55 ? CP_A : rssi >= -63 ? CP_B : rssi >= -70 ? CP_C : rssi >= -77 ? CP_D : rssi >= -84 ? CP_E : CP_F;
    /* A strong signal is not enough if packets get lost or arrive late. */
    if (loss >= 20)
    {
        g += 2;
    }
    else if (loss >= 5)
    {
        g += 1;
    }
    if (ping_ms >= 150)
    {
        g += 2;
    }
    else if (ping_ms >= 60)
    {
        g += 1;
    }
    return (cp_grade_t)(g > CP_F ? CP_F : g);
}

const char *cp_grade_letter(cp_grade_t grade)
{
    static const char *const letters[] = {"A", "B", "C", "D", "E", "F"};
    return letters[grade <= CP_F ? grade : CP_F];
}

const char *cp_grade_text(cp_grade_t grade)
{
    static const char *const text[] = {
        "Ottimo: videochiamate e 4K",
        "Buono: streaming HD senza pensieri",
        "Discreto: navigazione e musica",
        "Debole: video a scatti, chiamate incerte",
        "Scarso: solo messaggi",
        "Zona morta",
    };
    return text[grade <= CP_F ? grade : CP_F];
}

int cp_signal_percent(int rssi)
{
    int p = (rssi + 90) * 2;
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

/* Weight of a neighbour by strength: a -45 dBm router weighs ~10x a -85 dBm one. */
static int weight(int rssi)
{
    int w = rssi + 95;   /* -95 -> 0, -45 -> 50 */
    if (w < 0)
    {
        return 0;
    }
    return w * w / 25;   /* 0..100, quadratic: strong neighbours dominate */
}

/* Overlap of two 20 MHz channels (5 MHz apart per step): in percent. */
static int overlap(int a, int b)
{
    int d = a > b ? a - b : b - a;
    static const int8_t table[] = {100, 75, 50, 25, 5};
    return d < 5 ? table[d] : 0;
}

void cp_channel_advice(const cp_neighbour_t *neighbours, int count, uint8_t current, cp_channels_t *out)
{
    for (int c = 0; c <= CP_CHANNELS; c++)
    {
        out->score[c] = 0;
    }
    for (int i = 0; i < count; i++)
    {
        int ch = neighbours[i].channel;
        if (ch < 1 || ch > CP_CHANNELS)
        {
            continue;
        }
        int w = weight(neighbours[i].rssi);
        for (int c = 1; c <= CP_CHANNELS; c++)
        {
            out->score[c] += w * overlap(c, ch) / 100;
        }
    }
    /* Europe allows 13 channels: 1/5/9/13 do not overlap, the classic 1/6/11 neither. */
    static const uint8_t candidates[] = {1, 5, 6, 9, 11, 13};
    out->best = 1;
    for (unsigned i = 0; i < sizeof(candidates); i++)
    {
        if (out->score[candidates[i]] < out->score[out->best])
        {
            out->best = candidates[i];
        }
    }
    out->current = current;
    out->current_score = current >= 1 && current <= CP_CHANNELS ? out->score[current] : 0;
    out->best_score = out->score[out->best];
    /* Changing is worth it only for a clear gain (the air changes all the time). */
    out->worth_changing = current >= 1 && current <= CP_CHANNELS && out->best != current &&
                          out->current_score >= 20 && out->best_score * 2 < out->current_score;
}

int cp_repeater_spot(const cp_spot_t *spots, int count)
{
    bool any_weak = false;
    for (int i = 0; i < count; i++)
    {
        any_weak |= spots[i].weak;
    }
    if (!any_weak)
    {
        return -1;   /* everything is covered: no repeater needed */
    }
    int best = -1, best_distance = 0;
    for (int i = 0; i < count; i++)
    {
        int r = spots[i].rssi;
        if (spots[i].weak || r < -70 || r > -50)
        {
            continue;   /* too weak to relay well, or too close to the router to extend anything */
        }
        int distance = r > REPEATER_TARGET ? r - REPEATER_TARGET : REPEATER_TARGET - r;
        if (best < 0 || distance < best_distance)
        {
            best = i;
            best_distance = distance;
        }
    }
    return best;
}

int cp_finder_heat(int mode, int rssi, int best_seen)
{
    int heat;
    if (mode == 1)
    {
        int d = rssi > REPEATER_TARGET ? rssi - REPEATER_TARGET : REPEATER_TARGET - rssi;
        heat = 100 - d * 5;   /* on target = 100, 20 dB away = 0 */
    }
    else
    {
        /* Strongest: 100 at the best seen so far (or above), 15 dB below = 0. */
        int gap = best_seen - rssi;
        heat = gap <= 0 ? 100 : 100 - gap * 100 / 15;
    }
    return heat < 0 ? 0 : heat > 100 ? 100 : heat;
}
