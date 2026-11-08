/* Ordered chunks share the existing authenticated family RPC and its lease.
 * No task/queue/heap allocation here. The application caller owns its buffers. */
#include "services/ecos_orbit.h"
#include "ecos_orbit_fragment.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <stdatomic.h>
#include <string.h>

_Static_assert(ECOS_ORBIT_REQUEST_MAX == ECOS_ORBIT_LARGE_MAX, "Orbit request bounds must agree");
static atomic_bool s_large_calling;

int ecos_family_orbit_large(const char *id, const uint8_t *request, size_t length,
                            uint8_t *reply, size_t max, int timeout_ms)
{
    if (id == NULL || request == NULL || reply == NULL || length == 0 || length > ECOS_ORBIT_REQUEST_MAX ||
        max == 0 || timeout_ms <= 0 || timeout_ms > 30000)
    {
        return -1;
    }
    if (length <= ECOS_ORBIT_MAX)
    {
        return ecos_family_orbit(id, request, length, reply, max, timeout_ms);
    }
    if (atomic_exchange(&s_large_calling, true))
    {
        return -1;
    }
    uint64_t transfer;
    do
    {
        esp_fill_random(&transfer, sizeof(transfer));
    } while (transfer == 0);
    int result = -1;
    int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint8_t packet[ECOS_ORBIT_MAX], answer[ECOS_ORBIT_MAX];
    for (size_t offset = 0; offset < length;)
    {
        int64_t left = (until - esp_timer_get_time()) / 1000;
        if (left <= 0) break;
        size_t n = ecos_orbit_fragment_pack(packet, sizeof(packet), transfer, request, length, offset);
        if (n == 0) break;
        int got = ecos_family_orbit(id, packet, n, answer, sizeof(answer), (int)left);
        if (got <= 0) break;
        size_t next = offset + n - ECOS_ORBIT_FRAGMENT_HEADER;
        if (next == length)
        {
            if ((size_t)got <= max)
            {
                memcpy(reply, answer, (size_t)got);
                result = got;
            }
            break;
        }
        if (!ecos_orbit_fragment_ack_matches(answer, (size_t)got, next)) break;
        offset = next;
    }
    atomic_store(&s_large_calling, false);
    return result;
}
