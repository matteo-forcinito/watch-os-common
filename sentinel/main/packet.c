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

/* ---- Identification: DNS/mDNS PTR, NetBIOS node status, UPnP and HTTP text. */

static void put16(uint8_t *p, uint16_t value) { p[0] = value >> 8; p[1] = value & 255; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static int decimal(int value, char *out)
{
    int length = 0;
    if (value >= 100) out[length++] = '0' + value / 100;
    if (value >= 10) out[length++] = '0' + value / 10 % 10;
    out[length++] = '0' + value % 10;
    return length;
}

size_t sn_dns_ptr_query(uint8_t *out, size_t capacity, uint16_t id, uint32_t ip)
{
    /* ip in network order: a.b.c.d -> "d.c.b.a.in-addr.arpa", type PTR, class IN. */
    static const uint8_t suffix[] = {7,'i','n','-','a','d','d','r',4,'a','r','p','a',0,0,12,0,1};
    if (out == NULL || capacity < 12 + 16 + sizeof(suffix)) return 0;
    const uint8_t *octet = (const uint8_t *)&ip;
    memset(out, 0, 12);
    put16(out, id);
    out[2] = 1; /* recursion desired */
    put16(out + 4, 1);
    size_t used = 12;
    for (int i = 3; i >= 0; i--)
    {
        int length = decimal(octet[i], (char *)out + used + 1);
        out[used] = (uint8_t)length;
        used += 1 + length;
    }
    memcpy(out + used, suffix, sizeof(suffix));
    return used + sizeof(suffix);
}

/* Skip a (possibly compressed) name starting at *offset; false if malformed. */
static bool skip_name(const uint8_t *p, size_t length, size_t *offset)
{
    for (int labels = 0; labels < 128 && *offset < length; labels++)
    {
        uint8_t size = p[*offset];
        if (size == 0) { (*offset)++; return true; }
        if ((size & 0xc0) == 0xc0)
        {
            if (*offset + 2 > length) return false;
            *offset += 2;
            return true;
        }
        if (size & 0xc0) return false;
        *offset += 1 + size;
    }
    return false;
}

/* Decode a compressed name into dotted text: bounded jumps, printable ASCII only. */
static bool read_name(const uint8_t *p, size_t length, size_t offset, char *out, size_t capacity)
{
    size_t used = 0;
    for (int steps = 0; steps < 64; steps++)
    {
        if (offset >= length) return false;
        uint8_t size = p[offset];
        if (size == 0)
        {
            if (used == 0) return false;
            out[used] = 0;
            return true;
        }
        if ((size & 0xc0) == 0xc0)
        {
            if (offset + 2 > length) return false;
            offset = get16(p + offset) & 0x3fff;
            continue;
        }
        if ((size & 0xc0) || offset + 1 + size > length || used + size + 2 > capacity) return false;
        if (used) out[used++] = '.';
        for (uint8_t i = 0; i < size; i++)
        {
            uint8_t c = p[offset + 1 + i];
            if (c <= 32 || c >= 127) return false;
            out[used++] = (char)c;
        }
        offset += 1 + size;
    }
    return false;
}

bool sn_dns_ptr_answer(const uint8_t *p, size_t length, uint16_t *id, char *name, size_t capacity)
{
    if (p == NULL || id == NULL || name == NULL || capacity == 0 || length < 12) return false;
    name[0] = 0;
    if ((p[2] & 0x80) == 0 || (p[3] & 15) != 0) return false; /* not a response, or an error code */
    unsigned questions = get16(p + 4), answers = get16(p + 6);
    if (questions > 4 || answers == 0 || answers > 32) return false;
    size_t offset = 12;
    for (unsigned i = 0; i < questions; i++)
    {
        if (!skip_name(p, length, &offset) || offset + 4 > length) return false;
        offset += 4;
    }
    for (unsigned i = 0; i < answers; i++)
    {
        if (!skip_name(p, length, &offset) || offset + 10 > length) return false;
        uint16_t type = get16(p + offset), class = get16(p + offset + 2) & 0x7fff; /* mDNS cache-flush bit */
        uint16_t data = get16(p + offset + 8);
        offset += 10;
        if (offset + data > length) return false;
        if (type == 12 && class == 1 && read_name(p, length, offset, name, capacity))
        {
            *id = get16(p);
            return true;
        }
        name[0] = 0;
        offset += data;
    }
    return false;
}

size_t sn_nbstat_query(uint8_t *out, size_t capacity, uint16_t id)
{
    /* Node status request for the wildcard name "*", first-level encoded (RFC 1002). */
    if (out == NULL || capacity < 50) return 0;
    memset(out, 0, 50);
    put16(out, id);
    put16(out + 4, 1);
    out[12] = 32;
    out[13] = 'C';
    out[14] = 'K';
    for (int i = 15; i < 45; i++) out[i] = 'A';
    put16(out + 46, 0x21);
    put16(out + 48, 1);
    return 50;
}

bool sn_nbstat_name(const uint8_t *p, size_t length, uint16_t id, char *name, size_t capacity)
{
    if (p == NULL || name == NULL || capacity < 16 || length < 12) return false;
    name[0] = 0;
    if (get16(p) != id || (p[2] & 0x80) == 0 || get16(p + 6) == 0) return false;
    size_t offset = 12;
    if (!skip_name(p, length, &offset) || offset + 11 > length || get16(p + offset) != 0x21) return false;
    uint16_t data = get16(p + offset + 8);
    offset += 10;
    if (data < 1 || offset + data > length) return false;
    unsigned count = p[offset++];
    for (unsigned i = 0; i < count && offset + 18 <= length; i++, offset += 18)
    {
        const uint8_t *entry = p + offset;
        if (entry[15] != 0x00 || (get16(entry + 16) & 0x8000)) continue; /* unique workstation name */
        int end = 15;
        while (end > 0 && entry[end-1] == ' ') end--;
        if (end == 0) continue;
        for (int j = 0; j < end; j++) if (entry[j] < 33 || entry[j] > 126) return false;
        memcpy(name, entry, end);
        name[end] = 0;
        return true;
    }
    return false;
}

static bool same_text(const char *a, const char *b, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        char x = a[i], y = b[i];
        if (x == 0 || y == 0) return x == y;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

bool sn_http_header(const char *response, const char *header, char *out, size_t capacity)
{
    if (response == NULL || header == NULL || out == NULL || capacity == 0) return false;
    out[0] = 0;
    size_t name = strlen(header);
    const char *line = strchr(response, '\n'); /* skip the status line */
    while (line != NULL)
    {
        line++;
        if (*line == '\r' || *line == '\n' || *line == 0) return false; /* end of headers */
        if (same_text(line, header, name) && line[name] == ':')
        {
            const char *value = line + name + 1;
            while (*value == ' ' || *value == '\t') value++;
            size_t used = 0;
            while (value[used] && value[used] != '\r' && value[used] != '\n')
            {
                if (used + 1 >= capacity) return false;
                out[used] = value[used];
                used++;
            }
            while (used && out[used-1] == ' ') used--;
            out[used] = 0;
            if (used == 0 || !sn_text_valid(out, capacity - 1)) { out[0] = 0; return false; }
            return true;
        }
        line = strchr(line, '\n');
    }
    return false;
}

bool sn_location(const char *response, uint32_t source_ip, uint16_t *port, char *path, size_t capacity)
{
    char url[160], expected[16];
    if (port == NULL || path == NULL || capacity < 2 || !sn_http_header(response, "LOCATION", url, sizeof(url)) ||
        strncmp(url, "http://", 7) != 0) return false;
    /* Only follow a description served by the device that answered, never another host. */
    const uint8_t *ip = (const uint8_t *)&source_ip;
    int length = 0;
    for (int i = 0; i < 4; i++)
    {
        length += decimal(ip[i], expected + length);
        if (i < 3) expected[length++] = '.';
    }
    const char *rest = url + 7;
    if (strncmp(rest, expected, length) != 0) return false;
    rest += length;
    unsigned value = 80;
    if (*rest == ':')
    {
        value = 0;
        rest++;
        if (*rest < '0' || *rest > '9') return false;
        while (*rest >= '0' && *rest <= '9')
        {
            value = value * 10 + (unsigned)(*rest++ - '0');
            if (value > 65535) return false;
        }
        if (value == 0) return false;
    }
    if (*rest != '/') return false;
    size_t used = strlen(rest);
    if (used >= capacity) return false;
    for (size_t i = 0; i < used; i++) if (rest[i] <= 32 || rest[i] >= 127) return false;
    memcpy(path, rest, used + 1);
    *port = (uint16_t)value;
    return true;
}

bool sn_xml_value(const char *xml, const char *tag, char *out, size_t capacity)
{
    static const struct { const char *entity; char value; } entities[] =
        {{"&amp;",'&'},{"&lt;",'<'},{"&gt;",'>'},{"&quot;",'"'},{"&apos;",'\''},{"&#39;",'\''}};
    if (xml == NULL || tag == NULL || out == NULL || capacity == 0) return false;
    out[0] = 0;
    size_t name = strlen(tag);
    for (const char *p = strchr(xml, '<'); p != NULL; p = strchr(p + 1, '<'))
    {
        if (!same_text(p + 1, tag, name) || (p[1+name] != '>' && p[1+name] != ' ')) continue;
        const char *c = strchr(p, '>');
        if (c == NULL) return false;
        c++;
        while (*c == ' ' || *c == '\r' || *c == '\n' || *c == '\t') c++;
        size_t used = 0;
        while (*c && *c != '<' && used + 1 < capacity) /* long values are cut, never overflow */
        {
            char next = *c;
            size_t skip = 1;
            if (next == '&')
                for (unsigned e = 0; e < sizeof(entities)/sizeof(entities[0]); e++)
                    if (strncmp(c, entities[e].entity, strlen(entities[e].entity)) == 0)
                    {
                        next = entities[e].value;
                        skip = strlen(entities[e].entity);
                        break;
                    }
            if (next == '\r' || next == '\n' || next == '\t') next = ' ';
            out[used++] = next;
            c += skip;
        }
        while (used && out[used-1] == ' ') used--;
        /* A cut may split a UTF-8 sequence: drop the incomplete tail. */
        size_t lead = used;
        while (lead && ((unsigned char)out[lead-1] & 0xc0) == 0x80) lead--;
        if (lead && ((unsigned char)out[lead-1] & 0x80))
        {
            unsigned char first = (unsigned char)out[lead-1];
            size_t need = first >= 0xf0 ? 4 : first >= 0xe0 ? 3 : 2;
            if (used - (lead - 1) < need) used = lead - 1;
        }
        out[used] = 0;
        if (used == 0 || !sn_text_valid(out, capacity - 1)) { out[0] = 0; return false; }
        return true;
    }
    return false;
}
