/* Exercise the production bus with a deterministic fake radio. */
#include <assert.h>
#include <stdio.h>
#include "../ecos_bus.c"
static void receive(const esp_now_recv_info_t *i, const uint8_t *d, int n)
{ (void)i; (void)d; (void)n; recv_count++; }
static void sent(const esp_now_send_info_t *i, esp_now_send_status_t s)
{ (void)i; (void)s; sent_count++; }
static int drain_slot=-1, drains;
static const uint8_t mac_a[6]={2,0,0,0,0,1};
static const uint8_t mac_b[6]={2,0,0,0,0,2};
static const uint8_t broadcast_mac[6]={255,255,255,255,255,255};
static const uint8_t frame_ec[4]={'E','C','0','1'};
static const uint8_t frame_el[4]={'E','L','0','2'};
static esp_now_send_info_t tx_info;
static void try_close_during_sdk_send(void)
{
    assert(lock_depth == 1);
    ecos_bus_close("EL02");
    assert(s_open > 0 && s_tx_pending);
}
static void vTaskDelay(int ticks)
{
    (void)ticks;
    assert(drain_slot >= 0 && !s_services[drain_slot].used);
    s_services[drain_slot].active=0;
    drains++;
}
int main(void)
{
    esp_now_recv_info_t info={0};
    uint8_t mac[6]={0};
    assert(ecos_bus_open(NULL, receive, NULL, 0)==ESP_ERR_INVALID_ARG);
    fail_init=7;
    assert(ecos_bus_open("EC01", receive, NULL, 0)==7 && leases==0);
    fail_init=0; fail_recv=8;
    assert(ecos_bus_open("EC01", receive, NULL, 0)==8 && leases==0 && radio_recv==NULL);
    fail_recv=0; fail_sent=9;
    assert(ecos_bus_open("EC01", receive, NULL, 0)==9 && leases==0 && radio_sent==NULL);
    fail_sent=0;
    assert(ecos_bus_open("EC01", receive, NULL, 32)==ESP_OK && leases==1 && power==32);
    int starts=inits;
    assert(ecos_bus_open("EL02", receive, sent, 44)==ESP_OK && inits==starts && leases==1 && power==32);
    assert(ecos_bus_open("EL02", receive, sent, 0)==ESP_ERR_INVALID_STATE);
    radio_recv(&info, (const uint8_t *)"EL02payload", 11);
    radio_recv(&info, (const uint8_t *)"BAD!", 4);
    radio_recv(&info, (const uint8_t *)"EC", 2);
    assert(recv_count==1);
    tx_info.des_addr=mac_a;
    assert(ecos_bus_send(mac_a, frame_el, sizeof(frame_el))==ESP_OK && send_count==1);
    assert(ecos_bus_send(mac_a, frame_ec, sizeof(frame_ec))==ESP_ERR_INVALID_STATE);
    tx_info.des_addr=mac_b; radio_sent(&tx_info,0); assert(sent_count==0 && s_tx_pending);
    tx_info.des_addr=mac_a; radio_sent(&tx_info,0); assert(sent_count==1 && !s_tx_pending);
    /* Both owners can target the same MAC; the frame tag still selects one owner. */
    assert(ecos_bus_send(mac_a, frame_ec, sizeof(frame_ec))==ESP_OK);
    radio_sent(&tx_info,0); assert(sent_count==1); /* EC01 has no sent callback */
    assert(ecos_bus_send_tag("EL02", mac_a, frame_el, sizeof(frame_el))==ESP_OK);
    radio_sent(&tx_info,0); assert(sent_count==2);
    fail_send=17;
    assert(ecos_bus_send_tag("EL02", mac_a, frame_el, sizeof(frame_el))==17 && !s_tx_pending);
    fail_send=0;
    /* A closing owner is detached but its in-flight slot survives for the late callback. */
    assert(ecos_bus_send_tag("EL02", mac_a, frame_el, sizeof(frame_el))==ESP_OK);
    ecos_bus_close("EL02");
    assert(s_tx_pending && !s_tx_callback_enabled);
    assert(ecos_bus_open("EL02", receive, sent, 44)==ESP_OK);
    tx_info.des_addr=mac_a; radio_sent(&tx_info,0);
    assert(sent_count==2 && !s_tx_pending);
    assert(ecos_bus_send_tag("EL02", broadcast_mac, frame_el, sizeof(frame_el))==ESP_OK);
    tx_info.des_addr=broadcast_mac; radio_sent(&tx_info,0);
    assert(sent_count==2 && !s_tx_pending);
    assert(ecos_bus_peer(NULL)==ESP_ERR_INVALID_ARG);
    assert(ecos_bus_peer(mac)==ESP_OK);
    mac[0]=1; assert(ecos_bus_peer(mac)==ESP_OK);
    ecos_bus_close("EC01"); assert(leases==1 && power==44);
    fail_power=12;
    assert(ecos_bus_power("EL02", 0)==12 && power==44);
    fail_power=0;
    assert(ecos_bus_power("EL02", 0)==ESP_OK && power==80);
    assert(ecos_bus_power("EL02", 100)==ESP_OK && power==80);
    assert(ecos_bus_power("NONE", 0)==ESP_ERR_NOT_FOUND);
    ecos_bus_close("NONE"); assert(leases==1);
    assert(ecos_bus_open("AAA1", receive, NULL, 0)==ESP_OK);
    assert(ecos_bus_open("AAA2", receive, NULL, 0)==ESP_OK);
    assert(ecos_bus_open("AAA3", receive, NULL, 0)==ESP_OK);
    assert(ecos_bus_open("FULL", receive, NULL, 0)==ESP_ERR_NO_MEM);
    ecos_bus_close("AAA1"); ecos_bus_close("AAA2"); ecos_bus_close("AAA3");
    for (int i=0;i<BUS_SERVICES;i++) if (s_services[i].used) drain_slot=i;
    s_services[drain_slot].active=1;
    ecos_bus_close("EL02");
    assert(drains==1 && leases==0 && s_open==0 && !radio_recv && !radio_sent);
    power=20;
    assert(ecos_bus_open("EL02", receive, sent, 44)==ESP_OK && power==20);
    assert(ecos_bus_send_tag("EL02", mac_a, frame_el, sizeof(frame_el))==ESP_OK);
    ecos_bus_close("EL02");
    assert(power==20 && leases==0 && !s_tx_pending && s_tx_owner==-1);
    /* Last deinit clears an abandoned in-flight slot so a fresh radio lease can send. */
    assert(ecos_bus_open("EL02", receive, sent, 44)==ESP_OK);
    const uint8_t hid_pattern[4] = {'R', 0x40, 0, 0};
    const uint8_t hid_mask[4] = {0xff, 0xe7, 0, 0};
    assert(ecos_bus_open_mask("HID?", NULL, hid_mask, receive, NULL, 0)==ESP_ERR_INVALID_ARG);
    assert(ecos_bus_open_mask("HID?", hid_pattern, hid_mask, receive, NULL, 0)==ESP_OK);
    assert(ecos_bus_open("RHU1", receive, NULL, 0)==ESP_ERR_INVALID_STATE);
    const uint8_t disjoint_pattern[4] = {'Z', 0, 0, 0};
    const uint8_t disjoint_mask[4] = {0xff, 0, 0, 0};
    assert(ecos_bus_open_mask("HID?", disjoint_pattern, disjoint_mask, receive, NULL, 0)==ESP_ERR_INVALID_STATE);
    int before_masked_rx = recv_count;
    radio_recv(&info, (const uint8_t *)"RHUBdata", 8);
    radio_recv(&info, (const uint8_t *)"RPxxdata", 8);
    radio_recv(&info, (const uint8_t *)"RQxxdata", 8);
    assert(recv_count == before_masked_rx+2);
    ecos_bus_close("HID?");
    assert(ecos_bus_send_tag("EL02", mac_a, frame_el, sizeof(frame_el))==ESP_OK);
    tx_info.des_addr=mac_a; radio_sent(&tx_info,0);
    assert(sent_count==3 && !s_tx_pending);
    uint8_t v2_frame[ESP_NOW_MAX_DATA_LEN_V2] = {'E','L','0','2'};
    assert(ecos_bus_send_tag("EL02", mac_a, v2_frame, sizeof(v2_frame))==ESP_OK);
    radio_sent(&tx_info,0);
    assert(ecos_bus_send_tag("EL02", mac_a, v2_frame, sizeof(v2_frame)+1)==ESP_ERR_INVALID_ARG);
    int before_hook = send_count;
    send_hook = try_close_during_sdk_send;
    assert(ecos_bus_send_tag("EL02", mac_a, frame_el, sizeof(frame_el))==ESP_OK);
    send_hook = NULL;
    assert(send_count == before_hook+1 && s_open == 1);
    radio_sent(&tx_info,0);
    ecos_bus_close("EL02"); assert(leases==0);
    puts("PASS: bus leases, dispatch, limits, startup rollback, capacity and callback drain");
    return 0;
}
