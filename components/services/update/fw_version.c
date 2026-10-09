#include "services/fw_version.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* "1.3.0-dev.2" -> numbers 1, 3, 0 and the pre-release part "dev.2" (or NULL). */
static bool parse(const char *text, long numbers[3], const char **pre)
{
    const char *p = text;

    for (int i = 0; i < 3; i++)
    {
        if (!isdigit((unsigned char)*p))
        {
            return false;
        }

        char *end = NULL;
        numbers[i] = strtol(p, &end, 10);
        p = end;

        if (i < 2)
        {
            if (*p != '.')
            {
                return false;
            }

            p++;
        }
    }

    if (*p == '\0')
    {
        *pre = NULL;
        return true;
    }

    if (*p == '-' && p[1] != '\0')
    {
        *pre = p + 1;
        return true;
    }

    return false;
}

/* One dot-separated piece of a pre-release ("dev", "10"): numbers compare as numbers. */
static int compare_identifier(const char *a, size_t a_len, const char *b, size_t b_len)
{
    bool a_num = a_len > 0;
    bool b_num = b_len > 0;

    for (size_t i = 0; i < a_len; i++)
    {
        a_num &= isdigit((unsigned char)a[i]) != 0;
    }

    for (size_t i = 0; i < b_len; i++)
    {
        b_num &= isdigit((unsigned char)b[i]) != 0;
    }

    if (a_num && b_num)
    {
        long x = strtol(a, NULL, 10);
        long y = strtol(b, NULL, 10);
        return (x > y) - (x < y);
    }

    if (a_num != b_num)
    {
        return a_num ? -1 : 1;   /* numbers come before words */
    }

    size_t n = a_len < b_len ? a_len : b_len;
    int c = strncmp(a, b, n);

    return c != 0 ? c : (int)(a_len > b_len) - (int)(a_len < b_len);
}

static int compare_prerelease(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        size_t a_len = strcspn(a, ".");
        size_t b_len = strcspn(b, ".");
        int c = compare_identifier(a, a_len, b, b_len);

        if (c != 0)
        {
            return c;
        }

        a += a_len + (a[a_len] == '.');
        b += b_len + (b[b_len] == '.');
    }

    return (*a != '\0') - (*b != '\0');   /* more pieces = newer */
}

int fw_version_compare(const char *a, const char *b)
{
    long x[3];
    long y[3];
    const char *x_pre = NULL;
    const char *y_pre = NULL;

    if (!parse(a, x, &x_pre) || !parse(b, y, &y_pre))
    {
        int c = strcmp(a, b);
        return (c > 0) - (c < 0);
    }

    for (int i = 0; i < 3; i++)
    {
        if (x[i] != y[i])
        {
            return x[i] > y[i] ? 1 : -1;
        }
    }

    if (x_pre == NULL || y_pre == NULL)
    {
        /* 1.3.0 is newer than any 1.3.0-something */
        return (x_pre == NULL) - (y_pre == NULL);
    }

    int c = compare_prerelease(x_pre, y_pre);
    return (c > 0) - (c < 0);
}

bool fw_version_is_prerelease(const char *version)
{
    long numbers[3];
    const char *pre = NULL;

    return parse(version, numbers, &pre) && pre != NULL;
}
