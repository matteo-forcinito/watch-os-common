#include "../ecos_stream_slot.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    ecos_stream_slot_t slot = {0};
    uint8_t mac[6] = {2, 1, 2, 3, 4, 5}, other[6] = {4}, zero[6] = {0}, multicast[6] = {1};
    uint8_t data[ECOS_STREAM_DATA_MAX + 1], out[ECOS_STREAM_DATA_MAX], to[6];
    memset(data, 0x23, sizeof(data));
    assert(!ecos_stream_open(&slot, zero) && !ecos_stream_open(&slot, multicast));
    assert(!ecos_stream_put(&slot, mac, data, 1));
    assert(ecos_stream_open(&slot, mac) && !ecos_stream_open(&slot, other));
    assert(!ecos_stream_put(&slot, other, data, 1));
    assert(!ecos_stream_put(&slot, mac, data, sizeof(data)));
    assert(!ecos_stream_put(&slot, mac, data, 0));
    assert(ecos_stream_put(&slot, mac, data, ECOS_STREAM_DATA_MAX));
    assert(!ecos_stream_take(&slot, to, out, 1) && slot.length == ECOS_STREAM_DATA_MAX);
    data[0] = 0x42;
    assert(ecos_stream_put(&slot, mac, data, 1));
    assert(ecos_stream_take(&slot, to, out, sizeof(out)) == 1 && out[0] == 0x42);
    assert(memcmp(to, mac, 6) == 0 && ecos_stream_take(&slot, to, out, sizeof(out)) == 0);
    ecos_stream_close(&slot, other);
    assert(slot.active);
    ecos_stream_close(&slot, mac);
    assert(!slot.active && slot.length == 0 && slot.data[0] == 0 && slot.mac[0] == 0);
    assert(ecos_stream_open(&slot, other));
    puts("Orbit lossy stream mailbox tests passed");
    return 0;
}
