/* Pure-C validators: no packet retention, heap allocation or SDK dependency. */
#include "packet.h"
#include <string.h>

bool sn_proxy_arp(uint32_t sender_ip, uint32_t gateway_ip, const uint8_t *sender_mac, const uint8_t *gateway_mac)
{
    return sender_ip != gateway_ip && sender_mac != NULL && gateway_mac != NULL &&
           memcmp(gateway_mac,"\0\0\0\0\0\0",6) != 0 && memcmp(sender_mac,gateway_mac,6) == 0;
}

bool sn_arp_reply(const void *frame, size_t length, uint32_t own_ip,
                  uint32_t *sender_ip, uint8_t sender_mac[6])
{
    if (frame == NULL || length < 42 || sender_ip == NULL || sender_mac == NULL) return false;
    const uint8_t *p = frame;
    if (p[12] != 8 || p[13] != 6 || p[14] != 0 || p[15] != 1 ||
        p[16] != 8 || p[17] != 0 || p[18] != 6 || p[19] != 4 ||
        p[20] != 0 || p[21] != 2 || memcmp(p + 38, &own_ip, 4) != 0) return false;
    if ((p[22] & 1) != 0 || memcmp(p + 22, "\0\0\0\0\0\0", 6) == 0) return false;
    if (memcmp(p + 6, p + 22, 6) != 0) return false;
    memcpy(sender_ip, p + 28, 4);
    memcpy(sender_mac, p + 22, 6);
    return true;
}

bool sn_text_valid(const char *text, size_t maximum)
{
    if (text == NULL) return false;
    size_t length = strlen(text);
    if (length > maximum) return false;
    const unsigned char *p = (const unsigned char *)text;
    for (size_t i = 0; i < length;)
    {
        unsigned char lead = p[i++];
        if (lead < 0x80)
        {
            if (lead < 32 || lead == 127) return false;
            continue;
        }
        unsigned remaining;
        uint32_t value, minimum;
        if (lead >= 0xc2 && lead <= 0xdf) { remaining = 1; value = lead & 31; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { remaining = 2; value = lead & 15; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { remaining = 3; value = lead & 7; minimum = 0x10000; }
        else return false;
        if (i + remaining > length) return false;
        while (remaining--)
        {
            if ((p[i] & 0xc0) != 0x80) return false;
            value = (value << 6) | (p[i++] & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

bool sn_json_safe(const char *json, size_t length)
{
    if (!json || !length) return false;
    char stack[12];
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < length; i++)
    {
        unsigned char c = json[i];
        if (c == 0) return false;
        if (quoted)
        {
            if (c < 32) return false;
            if (escaped) escaped = false;
            else if (c == '\\')
            {
                /* cJSON exposes strings as NUL-terminated C strings. Reject
                 * escaped NULs too, so identifiers cannot silently truncate. */
                if (i+5 < length && memcmp(json+i+1,"u0000",5) == 0) return false;
                escaped = true;
            }
            else if (c == '"') quoted = false;
        }
        else if (c == '"') quoted = true;
        else if (c == '{' || c == '[')
        {
            if (depth == sizeof(stack)) return false;
            stack[depth++] = c;
        }
        else if (c == '}' || c == ']')
        {
            if (!depth || stack[--depth] != (c == '}' ? '{' : '[')) return false;
        }
    }
    return depth == 0 && !quoted;
}
