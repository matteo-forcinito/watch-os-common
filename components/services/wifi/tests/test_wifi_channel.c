#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../wifi_channel.h"

static const uint8_t OWNER_A[4] = {'H', 'I', 'D', '1'};
static const uint8_t OWNER_B[4] = {'S', 'O', 'L', 'C'};
static const uint8_t OWNER_C[4] = {'B', 'L', 'E', '1'};
static const uint8_t OWNER_D[4] = {'W', 'I', 'F', 'I'};
static const uint8_t OWNER_ZERO[4] = {0, 0, 0, 0};

static void test_shared_pin_and_isolation(void)
{
    wifi_channel_policy_t policy = {0};
    assert(wifi_channel_pinned(&policy) == 0);
    assert(wifi_channel_allowed(&policy, 1, false, 0));
    assert(wifi_channel_allowed(&policy, 13, false, 0));
    assert(!wifi_channel_allowed(&policy, 0, false, 0));
    assert(!wifi_channel_allowed(&policy, 14, false, 0));

    assert(wifi_channel_reserve(&policy, OWNER_A, 6, false, 0));
    assert(wifi_channel_reserve(&policy, OWNER_A, 6, false, 0)); /* idempotent */
    assert(wifi_channel_owner_has_reservation(&policy, OWNER_A));
    assert(!wifi_channel_owner_has_reservation(&policy, OWNER_B));
    assert(wifi_channel_reserve(&policy, OWNER_B, 6, false, 0));
    assert(wifi_channel_pinned(&policy) == 6);
    assert(wifi_channel_allowed(&policy, 6, false, 0));
    assert(!wifi_channel_allowed(&policy, 11, false, 0));

    wifi_channel_policy_t snapshot = policy;
    assert(!wifi_channel_reserve(&policy, OWNER_C, 11, false, 0));
    assert(memcmp(&policy, &snapshot, sizeof(policy)) == 0);
    assert(!wifi_channel_reserve(&policy, OWNER_A, 11, false, 0));
    assert(memcmp(&policy, &snapshot, sizeof(policy)) == 0);
}

static void test_network_authority_and_repin(void)
{
    wifi_channel_policy_t policy = {0};
    assert(wifi_channel_allowed(&policy, 6, true, 6));
    assert(!wifi_channel_allowed(&policy, 11, true, 6));
    assert(!wifi_channel_allowed(&policy, 6, true, 0));

    assert(wifi_channel_reserve(&policy, OWNER_A, 6, true, 6));
    assert(wifi_channel_allowed(&policy, 6, false, 0));
    assert(!wifi_channel_allowed(&policy, 11, false, 0));
    assert(!wifi_channel_reserve(&policy, OWNER_B, 6, true, 11));
    assert(wifi_channel_reserve(&policy, OWNER_A, 11, true, 11));
    assert(wifi_channel_pinned(&policy) == 11);
    assert(wifi_channel_reserve(&policy, OWNER_A, 3, false, 0));
    assert(wifi_channel_pinned(&policy) == 3);

    assert(wifi_channel_reserve(&policy, OWNER_B, 3, false, 0));
    assert(!wifi_channel_reserve(&policy, OWNER_A, 8, false, 0));
    assert(wifi_channel_pinned(&policy) == 3);
}

static void test_release_capacity_and_invalid_inputs(void)
{
    wifi_channel_policy_t policy = {0};
    assert(!wifi_channel_reserve(NULL, OWNER_A, 1, false, 0));
    assert(!wifi_channel_reserve(&policy, NULL, 1, false, 0));
    assert(!wifi_channel_reserve(&policy, OWNER_ZERO, 1, false, 0));
    assert(!wifi_channel_reserve(&policy, OWNER_A, 0, false, 0));
    assert(!wifi_channel_reserve(&policy, OWNER_A, 14, false, 0));
    assert(!wifi_channel_reserve(&policy, OWNER_A, 6, true, 11));
    assert(wifi_channel_pinned(&policy) == 0);

    assert(wifi_channel_reserve(&policy, OWNER_A, 9, false, 0));
    assert(wifi_channel_reserve(&policy, OWNER_B, 9, false, 0));
    assert(wifi_channel_reserve(&policy, OWNER_C, 9, false, 0));
    assert(wifi_channel_reserve(&policy, OWNER_D, 9, false, 0));
    wifi_channel_policy_t snapshot = policy;
    const uint8_t owner_e[4] = {'F', 'I', 'V', 'E'};
    assert(!wifi_channel_reserve(&policy, owner_e, 9, false, 0));
    assert(memcmp(&policy, &snapshot, sizeof(policy)) == 0);

    assert(!wifi_channel_release(&policy, NULL));
    assert(!wifi_channel_release(&policy, OWNER_ZERO));
    assert(wifi_channel_release(&policy, OWNER_B));
    assert(wifi_channel_release(&policy, OWNER_B)); /* idempotent */
    assert(!wifi_channel_owner_has_reservation(&policy, OWNER_B));
    assert(wifi_channel_reserve(&policy, owner_e, 9, false, 0));
    assert(wifi_channel_pinned(&policy) == 9);
    assert(wifi_channel_release(&policy, OWNER_A));
    assert(wifi_channel_release(&policy, OWNER_C));
    assert(wifi_channel_release(&policy, OWNER_D));
    assert(wifi_channel_release(&policy, owner_e));
    assert(wifi_channel_pinned(&policy) == 0);
}

int main(void)
{
    test_shared_pin_and_isolation();
    test_network_authority_and_repin();
    test_release_capacity_and_invalid_inputs();
    puts("Wi-Fi channel policy tests passed");
    return 0;
}
