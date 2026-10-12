#include "swipe.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RESAMPLE 24 /* points per path compared by DTW */
#define BAND (RESAMPLE / 3)
#define NEAR_KEYS 4       /* start / end key candidates */
#define NEAR_RADIUS 1.15f /* in key widths */
#define REJECT 0.9f       /* DTW distance above this (key widths): not this word */
#define FREQ_WEIGHT 0.25f /* tuned in the simulator */
#define MAX_SEQ 20        /* keys in a word path */

struct swipe_dict
{
    char *text;         /* all words, NUL separated */
    const char **words; /* by rank (0 = most frequent) */
    uint8_t *seq;       /* folded key sequences, back to back */
    uint32_t *seq_offset;
    uint8_t *seq_length;
    int count;
    int *by_first[SWIPE_LETTERS]; /* word indexes, by first key, by rank */
    int by_first_count[SWIPE_LETTERS];
    float log_norm;
};

/* ------------------------------------------------------------ helpers */

/* Next letter of a word as a key index (accents folded), -1 = not a letter, -2 = end. */
static int next_key(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;

    if (*s == 0)
    {
        return -2;
    }

    if (*s >= 'a' && *s <= 'z')
    {
        *p += 1;
        return *s - 'a';
    }

    if (*s == 0xC3 && s[1] != 0)
    {
        *p += 2;

        switch (s[1])
        {
        case 0xA0:
            return 'a' - 'a'; /* à */
        case 0xA8:            /* è */
        case 0xA9:
            return 'e' - 'a'; /* é */
        case 0xAC:
            return 'i' - 'a'; /* ì */
        case 0xB2:
            return 'o' - 'a'; /* ò */
        case 0xB9:
            return 'u' - 'a'; /* ù */
        default:
            return -1;
        }
    }

    *p += 1;
    return -1;
}

static float dist(swipe_point_t a, swipe_point_t b)
{
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return sqrtf(dx * dx + dy * dy);
}

/* `n` points evenly spaced along the polyline `in` (count points). */
static void resample(const swipe_point_t *in, int count, swipe_point_t *out, int n)
{
    float total = 0.0f;

    for (int i = 1; i < count; i++)
    {
        total += dist(in[i - 1], in[i]);
    }

    if (count < 2 || total <= 0.0f)
    {
        for (int k = 0; k < n; k++)
        {
            out[k] = in[0];
        }

        return;
    }

    float step = total / (float)(n - 1);
    float walked = 0.0f; /* length up to in[j] */
    int j = 0;

    for (int k = 0; k < n; k++)
    {
        float target = step * (float)k;

        while (j < count - 2 && walked + dist(in[j], in[j + 1]) < target)
        {
            walked += dist(in[j], in[j + 1]);
            j++;
        }

        float segment = dist(in[j], in[j + 1]);
        float t = segment > 0.0f ? (target - walked) / segment : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        out[k].x = in[j].x + (in[j + 1].x - in[j].x) * t;
        out[k].y = in[j].y + (in[j + 1].y - in[j].y) * t;
    }
}

/* Banded DTW between two RESAMPLE-point paths, as an average distance per step. */
static float dtw(const swipe_point_t *a, const swipe_point_t *b)
{
    float prev[RESAMPLE + 1];
    float cur[RESAMPLE + 1];
    const float inf = 1e30f;

    for (int j = 0; j <= RESAMPLE; j++)
    {
        prev[j] = inf;
    }

    prev[0] = 0.0f;

    for (int i = 1; i <= RESAMPLE; i++)
    {
        for (int j = 0; j <= RESAMPLE; j++)
        {
            cur[j] = inf;
        }

        int lo = i - BAND < 1 ? 1 : i - BAND;
        int hi = i + BAND > RESAMPLE ? RESAMPLE : i + BAND;

        for (int j = lo; j <= hi; j++)
        {
            float best = prev[j];
            best = prev[j - 1] < best ? prev[j - 1] : best;
            best = cur[j - 1] < best ? cur[j - 1] : best;
            cur[j] = best + dist(a[i - 1], b[j - 1]);
        }

        memcpy(prev, cur, sizeof(prev));
    }

    return prev[RESAMPLE] / (float)(2 * RESAMPLE);
}

/* Up to NEAR_KEYS keys within `radius` of p, nearest first. */
static int near_keys(const swipe_layout_t *layout, swipe_point_t p, float radius, int *out)
{
    float d[NEAR_KEYS];
    int n = 0;

    for (int k = 0; k < SWIPE_LETTERS; k++)
    {
        float dk = dist(p, layout->center[k]);

        if (dk >= radius)
        {
            continue;
        }

        /* insertion into the small sorted list */
        int pos = n < NEAR_KEYS ? n : NEAR_KEYS;

        while (pos > 0 && d[pos - 1] > dk)
        {
            if (pos < NEAR_KEYS)
            {
                d[pos] = d[pos - 1];
                out[pos] = out[pos - 1];
            }

            pos--;
        }

        if (pos < NEAR_KEYS)
        {
            d[pos] = dk;
            out[pos] = k;

            if (n < NEAR_KEYS)
            {
                n++;
            }
        }
    }

    return n;
}

/* ------------------------------------------------------------- public */

swipe_dict_t *swipe_dict_create(const char *text, size_t length)
{
    swipe_dict_t *d = calloc(1, sizeof(swipe_dict_t));

    if (d == NULL)
    {
        return NULL;
    }

    d->text = malloc(length + 1);

    if (d->text == NULL)
    {
        free(d);
        return NULL;
    }

    memcpy(d->text, text, length);
    d->text[length] = '\0';

    int lines = 1;

    for (size_t i = 0; i < length; i++)
    {
        lines += d->text[i] == '\n';
    }

    d->words = malloc(sizeof(char *) * (size_t)lines);
    d->seq_offset = malloc(sizeof(uint32_t) * (size_t)lines);
    d->seq_length = malloc((size_t)lines);
    d->seq = malloc(length + 1); /* never longer than the text */

    if (d->words == NULL || d->seq_offset == NULL || d->seq_length == NULL || d->seq == NULL)
    {
        swipe_dict_free(d);
        return NULL;
    }

    uint32_t seq_used = 0;
    char *p = d->text;

    while (*p != '\0')
    {
        char *end = strchr(p, '\n');

        if (end != NULL)
        {
            *end = '\0';
        }

        if (end != NULL && end > p && end[-1] == '\r')
        {
            end[-1] = '\0';
        }

        /* Key sequence with doubles collapsed ("palla" -> p a l a). */
        const char *q = p;
        uint32_t start = seq_used;
        int key;
        bool valid = *p != '\0';

        while (valid && (key = next_key(&q)) != -2)
        {
            if (key < 0 || seq_used - start >= MAX_SEQ)
            {
                valid = false;
            }
            else if (seq_used == start || d->seq[seq_used - 1] != key)
            {
                d->seq[seq_used++] = (uint8_t)key;
            }
        }

        if (valid && strlen(p) < SWIPE_MAX_WORD)
        {
            d->words[d->count] = p;
            d->seq_offset[d->count] = start;
            d->seq_length[d->count] = (uint8_t)(seq_used - start);
            d->count++;
        }
        else
        {
            seq_used = start;
        }

        if (end == NULL)
        {
            break;
        }

        p = end + 1;
    }

    /* Index by first key (ranks stay in order inside each bucket). */
    for (int i = 0; i < d->count; i++)
    {
        d->by_first_count[d->seq[d->seq_offset[i]]]++;
    }

    for (int k = 0; k < SWIPE_LETTERS; k++)
    {
        d->by_first[k] = malloc(sizeof(int) * (size_t)(d->by_first_count[k] + 1));

        if (d->by_first[k] == NULL)
        {
            swipe_dict_free(d);
            return NULL;
        }

        d->by_first_count[k] = 0;
    }

    for (int i = 0; i < d->count; i++)
    {
        int k = d->seq[d->seq_offset[i]];
        d->by_first[k][d->by_first_count[k]++] = i;
    }

    d->log_norm = logf((float)d->count + 2.0f);

    return d;
}

void swipe_dict_free(swipe_dict_t *d)
{
    if (d == NULL)
    {
        return;
    }

    for (int k = 0; k < SWIPE_LETTERS; k++)
    {
        free(d->by_first[k]);
    }

    free(d->text);
    free(d->words);
    free(d->seq);
    free(d->seq_offset);
    free(d->seq_length);
    free(d);
}

int swipe_dict_size(const swipe_dict_t *dict)
{
    return dict != NULL ? dict->count : 0;
}

int swipe_decode(const swipe_dict_t *d, const swipe_layout_t *layout, const swipe_point_t *path, int count,
                 swipe_candidate_t *out, int max)
{
    if (d == NULL || count < 2 || max <= 0)
    {
        return 0;
    }

    swipe_point_t user[RESAMPLE];
    resample(path, count, user, RESAMPLE);

    float radius = layout->key_w * NEAR_RADIUS;
    int starts[NEAR_KEYS];
    int ends[NEAR_KEYS];
    int start_count = near_keys(layout, path[0], radius, starts);
    int end_count = near_keys(layout, path[count - 1], radius, ends);
    bool is_end[SWIPE_LETTERS] = {false};

    for (int i = 0; i < end_count; i++)
    {
        is_end[ends[i]] = true;
    }

    int found = 0;
    swipe_point_t keys[MAX_SEQ];
    swipe_point_t ideal[RESAMPLE];

    for (int s = 0; s < start_count; s++)
    {
        const int *bucket = d->by_first[starts[s]];
        int bucket_count = d->by_first_count[starts[s]];

        for (int b = 0; b < bucket_count; b++)
        {
            int w = bucket[b];
            int length = d->seq_length[w];
            const uint8_t *seq = d->seq + d->seq_offset[w];

            if (length < 2 || !is_end[seq[length - 1]])
            {
                continue;
            }

            for (int i = 0; i < length; i++)
            {
                keys[i] = layout->center[seq[i]];
            }

            resample(keys, length, ideal, RESAMPLE);
            float distance = dtw(user, ideal) / layout->key_w;

            if (distance > REJECT)
            {
                continue;
            }

            float score = distance + FREQ_WEIGHT * logf((float)w + 2.0f) / d->log_norm;

            /* keep the `max` best, sorted */
            int pos = found < max ? found : max;

            while (pos > 0 && out[pos - 1].score > score)
            {
                if (pos < max)
                {
                    out[pos] = out[pos - 1];
                }

                pos--;
            }

            if (pos < max)
            {
                out[pos].word = d->words[w];
                out[pos].score = score;

                if (found < max)
                {
                    found++;
                }
            }
        }
    }

    return found;
}
