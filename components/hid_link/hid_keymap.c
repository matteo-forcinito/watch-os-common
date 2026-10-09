#include "hid_private.h"

/*
 * Character -> key on the HOST keyboard layout.
 *
 * HID sends key positions; the computer turns them into characters with its
 * own layout setting. So "@" is AltGr+ò on an Italian PC and Shift+2 on a US
 * one. Characters the layout cannot type are reported as not typeable.
 */

#define S  HIDL_MOD_SHIFT
#define G  HIDL_MOD_ALTGR

typedef struct
{
    uint16_t codepoint;
    uint8_t modifiers;
    uint8_t keycode;
} entry_t;

/* Same on both layouts. */
static const entry_t s_common[] = {
    {' ', 0, 0x2C}, {'\n', 0, 0x28}, {'\t', 0, 0x2B},
    {'1', 0, 0x1E}, {'2', 0, 0x1F}, {'3', 0, 0x20}, {'4', 0, 0x21}, {'5', 0, 0x22},
    {'6', 0, 0x23}, {'7', 0, 0x24}, {'8', 0, 0x25}, {'9', 0, 0x26}, {'0', 0, 0x27},
};

/* Italian (Italy) PC layout. */
static const entry_t s_it[] = {
    {'!', S, 0x1E}, {'"', S, 0x1F}, {0x00A3 /* £ */, S, 0x20}, {'$', S, 0x21}, {'%', S, 0x22},
    {'&', S, 0x23}, {'/', S, 0x24}, {'(', S, 0x25}, {')', S, 0x26}, {'=', S, 0x27},
    {'\'', 0, 0x2D}, {'?', S, 0x2D}, {0x00EC /* ì */, 0, 0x2E}, {'^', S, 0x2E},
    {0x00E8 /* è */, 0, 0x2F}, {0x00E9 /* é */, S, 0x2F}, {'[', G, 0x2F}, {'{', G | S, 0x2F},
    {'+', 0, 0x30}, {'*', S, 0x30}, {']', G, 0x30}, {'}', G | S, 0x30},
    {0x00F2 /* ò */, 0, 0x33}, {0x00E7 /* ç */, S, 0x33}, {'@', G, 0x33},
    {0x00E0 /* à */, 0, 0x34}, {0x00B0 /* ° */, S, 0x34}, {'#', G, 0x34},
    {0x00F9 /* ù */, 0, 0x31}, {0x00A7 /* § */, S, 0x31},
    {'\\', 0, 0x35}, {'|', S, 0x35},
    {',', 0, 0x36}, {';', S, 0x36}, {'.', 0, 0x37}, {':', S, 0x37}, {'-', 0, 0x38}, {'_', S, 0x38},
    {'<', 0, 0x64}, {'>', S, 0x64}, {0x20AC /* € */, G, 0x08},
};

/* US (English) layout. */
static const entry_t s_us[] = {
    {'!', S, 0x1E}, {'@', S, 0x1F}, {'#', S, 0x20}, {'$', S, 0x21}, {'%', S, 0x22},
    {'^', S, 0x23}, {'&', S, 0x24}, {'*', S, 0x25}, {'(', S, 0x26}, {')', S, 0x27},
    {'-', 0, 0x2D}, {'_', S, 0x2D}, {'=', 0, 0x2E}, {'+', S, 0x2E},
    {'[', 0, 0x2F}, {'{', S, 0x2F}, {']', 0, 0x30}, {'}', S, 0x30},
    {'\\', 0, 0x31}, {'|', S, 0x31}, {';', 0, 0x33}, {':', S, 0x33},
    {'\'', 0, 0x34}, {'"', S, 0x34}, {'`', 0, 0x35}, {'~', S, 0x35},
    {',', 0, 0x36}, {'<', S, 0x36}, {'.', 0, 0x37}, {'>', S, 0x37}, {'/', 0, 0x38}, {'?', S, 0x38},
};

static bool find(const entry_t *table, int count, uint32_t cp, uint8_t *modifiers, uint8_t *keycode)
{
    for (int i = 0; i < count; i++)
    {
        if (table[i].codepoint == cp)
        {
            *modifiers = table[i].modifiers;
            *keycode = table[i].keycode;
            return true;
        }
    }

    return false;
}

/* Uppercase accented letters have no key: use the lowercase one. */
static uint32_t simplify(uint32_t cp)
{
    switch (cp)
    {
        case 0x00C0: return 0x00E0;   /* À */
        case 0x00C8: return 0x00E8;   /* È */
        case 0x00C9: return 0x00E9;   /* É */
        case 0x00CC: return 0x00EC;   /* Ì */
        case 0x00D2: return 0x00F2;   /* Ò */
        case 0x00D9: return 0x00F9;   /* Ù */
        case 0x2019: return '\'';     /* typographic apostrophe */
        default:     return cp;
    }
}

bool hid_keymap_lookup(hid_host_layout_t layout, uint32_t cp, uint8_t *modifiers, uint8_t *keycode)
{
    cp = simplify(cp);

    if (cp >= 'a' && cp <= 'z')
    {
        *modifiers = 0;
        *keycode = (uint8_t)(0x04 + (cp - 'a'));
        return true;
    }

    if (cp >= 'A' && cp <= 'Z')
    {
        *modifiers = S;
        *keycode = (uint8_t)(0x04 + (cp - 'A'));
        return true;
    }

    if (find(s_common, sizeof(s_common) / sizeof(s_common[0]), cp, modifiers, keycode))
    {
        return true;
    }

    if (layout == HID_HOST_IT)
    {
        return find(s_it, sizeof(s_it) / sizeof(s_it[0]), cp, modifiers, keycode);
    }

    return find(s_us, sizeof(s_us) / sizeof(s_us[0]), cp, modifiers, keycode);
}
