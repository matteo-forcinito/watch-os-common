#include "hub_state.h"

static void clear_bytes(void *target, size_t size)
{
    uint8_t *bytes = target;
    for (size_t i = 0; i < size; i++)
    {
        bytes[i] = 0;
    }
}

static uint64_t read_le(const uint8_t *data, unsigned length)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < length; i++)
    {
        value |= (uint64_t)data[i] << (8 * i);
    }
    return value;
}

static void changed(hub_state_t *state, uint8_t id)
{
    state->generation[id - 1]++;
    state->dirty |= (uint8_t)(1u << (id - 1));
}

static void clear_node(hub_state_t *state, unsigned node)
{
    clear_bytes(&state->nodes[node], sizeof(state->nodes[node]));
    for (uint8_t id = 1; id <= 4; id++)
    {
        changed(state, id);
    }
}

void hub_state_init(hub_state_t *state)
{
    clear_bytes(state, sizeof(*state));
    state->dirty = 0x0f;
}

bool hub_state_authorize(hub_state_t *state, unsigned node, uint64_t session)
{
    if (node >= HUB_NODES || session == 0)
    {
        return false;
    }
    clear_node(state, node);
    state->nodes[node].session = session;
    state->nodes[node].authorized = true;
    return true;
}

void hub_state_revoke(hub_state_t *state, unsigned node)
{
    if (node < HUB_NODES)
    {
        clear_node(state, node);
    }
}

static bool valid_payload(uint8_t id, const uint8_t *data)
{
    if (id == 1)
    {
        if (data[1] != 0)
        {
            return false;
        }
        for (unsigned i = 2; i < 8; i++)
        {
            if ((data[i] > 0 && data[i] < 4) || data[i] > 0x65)
            {
                return false;
            }
        }
    }
    else
    {
        unsigned length = id == 2 ? 2 : id == 3 ? 5 : 4;
        if ((id == 2 && read_le(data, 2) > 0x3ff) || (id == 3 && data[0] > 31))
        {
            return false;
        }
        if (id == 3 || id == 4)
        {
            unsigned start = id == 3 ? 1 : 0;
            unsigned end = id == 3 ? 5 : 2;
            for (unsigned i = start; i < end; i++)
            {
                if (data[i] == 0x80) /* Descriptor range is -127..127. */
                {
                    return false;
                }
            }
        }
        for (unsigned i = length; i < 8; i++)
        {
            if (data[i] != 0)
            {
                return false;
            }
        }
    }
    return true;
}

hub_result_t hub_state_receive(hub_state_t *state, unsigned node,
                               const uint8_t *packet, size_t length, uint32_t now_ms)
{
    if (node >= HUB_NODES || packet == NULL || length != HUB_PACKET_SIZE ||
        packet[0] != 'R' || packet[1] != 'H' || packet[2] != 1 ||
        packet[3] < 1 || packet[3] > 4 || !valid_payload(packet[3], packet + 16))
    {
        return HUB_INVALID;
    }
    hub_node_t *peer = &state->nodes[node];
    if (!peer->authorized || read_le(packet + 8, 8) != peer->session)
    {
        return HUB_UNAUTHORIZED;
    }
    uint32_t sequence = (uint32_t)read_le(packet + 4, 4);
    uint32_t distance = sequence - peer->sequence;
    if (peer->seen && (distance == 0 || distance >= UINT32_C(0x80000000)))
    {
        return HUB_DUPLICATE;
    }
    peer->sequence = sequence;
    peer->seen = true;
    peer->last_ms = now_ms;
    const uint8_t *data = packet + 16;
    uint8_t id = packet[3];
    if (id == 1)
    {
        for (unsigned i = 0; i < 8; i++)
        {
            peer->keyboard[i] = data[i];
        }
    }
    else if (id == 2)
    {
        peer->media = (uint16_t)read_le(data, 2);
    }
    else if (id == 3)
    {
        peer->buttons = data[0];
        for (unsigned i = 0; i < 4; i++)
        {
            int32_t sum = peer->motion[i] + (int8_t)data[i + 1];
            if (sum > 32767 || sum < -32767)
            {
                sum = sum > 0 ? 32767 : -32767;
                state->motion_overflows++;
            }
            peer->motion[i] = sum;
        }
    }
    else
    {
        for (unsigned i = 0; i < 4; i++)
        {
            peer->gamepad[i] = data[i];
        }
    }
    changed(state, id);
    return HUB_ACCEPTED;
}

void hub_state_expire(hub_state_t *state, uint32_t now_ms)
{
    for (unsigned node = 0; node < HUB_NODES; node++)
    {
        hub_node_t *peer = &state->nodes[node];
        if (peer->seen && (uint32_t)(now_ms - peer->last_ms) >= HUB_TIMEOUT_MS)
        {
            /* Retire the session. Old queued packets cannot revive held keys. */
            clear_node(state, node);
        }
    }
}

bool hub_state_snapshot(const hub_state_t *state, uint8_t id, hub_report_t *report)
{
    if (id < 1 || id > 4 || report == NULL || !(state->dirty & (1u << (id - 1))))
    {
        return false;
    }
    clear_bytes(report, sizeof(*report));
    report->id = id;
    report->length = id == 1 ? 8 : id == 2 ? 2 : id == 3 ? 5 : 4;
    report->generation = state->generation[id - 1];
    bool gamepad_selected = false;
    bool rollover = false;
    unsigned keys = 0;
    for (unsigned node = 0; node < HUB_NODES; node++)
    {
        const hub_node_t *peer = &state->nodes[node];
        if (!peer->authorized || !peer->seen)
        {
            continue;
        }
        if (id == 1)
        {
            report->data[0] |= peer->keyboard[0];
            for (unsigned k = 2; k < 8; k++)
            {
                uint8_t key = peer->keyboard[k];
                bool present = key == 0;
                for (unsigned j = 0; j < keys; j++)
                {
                    present |= report->data[j + 2] == key;
                }
                if (!present)
                {
                    if (keys == 6)
                    {
                        rollover = true;
                    }
                    else
                    {
                        report->data[keys++ + 2] = key;
                    }
                }
            }
        }
        else if (id == 2 && report->data[0] == 0 && report->data[1] == 0)
        {
            /* One consumer usage in the descriptor: lowest active slot wins. */
            report->data[0] = (uint8_t)peer->media;
            report->data[1] = (uint8_t)(peer->media >> 8);
        }
        else if (id == 3)
        {
            report->data[0] |= peer->buttons;
        }
        else if (id == 4 && !gamepad_selected)
        {
            bool active = false;
            for (unsigned i = 0; i < 4; i++)
            {
                active |= peer->gamepad[i] != 0;
            }
            if (active)
            {
                for (unsigned i = 0; i < 4; i++)
                {
                    report->data[i] = peer->gamepad[i];
                }
                gamepad_selected = true;
            }
        }
    }
    if (rollover)
    {
        for (unsigned i = 2; i < 8; i++)
        {
            report->data[i] = 1; /* HID ErrorRollOver; never silently drop keys. */
        }
    }
    if (id == 3)
    {
        for (unsigned i = 0; i < 4; i++)
        {
            int32_t value = 0;
            for (unsigned node = 0; node < HUB_NODES; node++)
            {
                value += state->nodes[node].motion[i];
            }
            value = value > 127 ? 127 : value < -127 ? -127 : value;
            report->data[i + 1] = (uint8_t)(int8_t)value;
        }
    }
    return true;
}

bool hub_state_commit(hub_state_t *state, const hub_report_t *report)
{
    if (report == NULL || report->id < 1 || report->id > 4 ||
        report->generation != state->generation[report->id - 1])
    {
        return false;
    }
    hub_report_t expected;
    if (!hub_state_snapshot(state, report->id, &expected) || report->length != expected.length)
    {
        return false;
    }
    for (unsigned i = 0; i < 8; i++)
    {
        if (expected.data[i] != report->data[i])
        {
            return false;
        }
    }
    state->dirty &= (uint8_t)~(1u << (report->id - 1));
    if (report->id == 3)
    {
        for (unsigned i = 0; i < 4; i++)
        {
            /* Cancel opposing deltas, then consume the accepted chunk. Each
             * peer retains its own remainder so revocation drops only its work. */
            for (unsigned a = 0; a < HUB_NODES; a++)
            {
                for (unsigned b = 0; b < HUB_NODES; b++)
                {
                    int32_t *positive = &state->nodes[a].motion[i];
                    int32_t *negative = &state->nodes[b].motion[i];
                    if (*positive > 0 && *negative < 0)
                    {
                        int32_t cancel = *positive < -*negative ? *positive : -*negative;
                        *positive -= cancel;
                        *negative += cancel;
                    }
                }
            }
            int32_t remaining = (int8_t)report->data[i + 1];
            for (unsigned node = 0; node < HUB_NODES; node++)
            {
                int32_t *pending = &state->nodes[node].motion[i];
                if (remaining > 0 && *pending > 0)
                {
                    int32_t consume = remaining < *pending ? remaining : *pending;
                    *pending -= consume;
                    remaining -= consume;
                }
                else if (remaining < 0 && *pending < 0)
                {
                    int32_t consume = remaining > *pending ? remaining : *pending;
                    *pending -= consume;
                    remaining -= consume;
                }
                if (*pending != 0)
                {
                    state->dirty |= 4;
                }
            }
            if (remaining != 0)
            {
                return false; /* Defensive: only a snapshot can be committed. */
            }
        }
        /* Prevent committing the same relative motion snapshot twice. */
        state->generation[2]++;
    }
    return true;
}

void hub_state_disconnect(hub_state_t *state)
{
    hub_state_init(state);
}
