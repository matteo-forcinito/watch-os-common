#include "captive_dns.h"

#include "core/sys.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <stdatomic.h>
#include <string.h>

static const char *TAG = "captive_dns";

/*
 * "Captive portal" DNS for the watch's own Wi-Fi network: every name a phone
 * asks for resolves to the watch. The phone's connectivity check then finds
 * our page instead of the internet, shows "sign in to network" and - most
 * important - keeps the connection instead of dropping a network "without
 * internet". Only A (IPv4) questions get an answer; others get an empty reply.
 */
#define DNS_PORT       53
#define PACKET_MAX     512
#define TASK_STACK     3072
#define HEADER_SIZE    12
#define TYPE_A         1

static atomic_bool s_running = false;
static atomic_bool s_task_alive = false;
static uint32_t s_ip = 0;   /* network byte order */

/* Length of the question section starting at `q`, 0 if malformed. */
static size_t question_length(const uint8_t *q, size_t avail)
{
    size_t i = 0;

    while (i < avail && q[i] != 0)
    {
        if (q[i] & 0xC0)
        {
            return 0;   /* compression in a question: not expected */
        }

        i += q[i] + 1;
    }

    i += 1 + 4;   /* final zero + QTYPE + QCLASS */

    return i <= avail ? i : 0;
}

static void dns_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (sock < 0)
    {
        ESP_LOGE(TAG, "No socket");
        atomic_store(&s_task_alive, false);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        ESP_LOGE(TAG, "Port 53 busy");
        close(sock);
        atomic_store(&s_task_alive, false);
        vTaskDelete(NULL);
        return;
    }

    static uint8_t packet[PACKET_MAX + 16];

    while (atomic_load(&s_running))
    {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, packet, PACKET_MAX, 0, (struct sockaddr *)&from, &from_len);

        if (len < HEADER_SIZE)
        {
            continue;   /* timeout (check s_running) or junk */
        }

        uint16_t questions = (uint16_t)((packet[4] << 8) | packet[5]);
        size_t qlen = question_length(packet + HEADER_SIZE, (size_t)len - HEADER_SIZE);

        if ((packet[2] & 0x80) || questions != 1 || qlen == 0)
        {
            continue;   /* not a single plain query */
        }

        uint16_t qtype = (uint16_t)((packet[HEADER_SIZE + qlen - 4] << 8) | packet[HEADER_SIZE + qlen - 3]);
        size_t out = HEADER_SIZE + qlen;
        bool answer = qtype == TYPE_A;

        packet[2] = 0x81;   /* response, recursion desired copied */
        packet[3] = 0x80;   /* recursion available, no error */
        packet[6] = 0;
        packet[7] = answer ? 1 : 0;
        memset(packet + 8, 0, 4);   /* no authority / additional records */

        if (answer)
        {
            static const uint8_t record[] = {
                0xC0, 0x0C,             /* name: pointer to the question */
                0x00, 0x01, 0x00, 0x01, /* type A, class IN */
                0x00, 0x00, 0x00, 0x3C, /* TTL 60 s */
                0x00, 0x04,             /* 4 bytes of address */
            };

            memcpy(packet + out, record, sizeof(record));
            out += sizeof(record);
            memcpy(packet + out, &s_ip, 4);
            out += 4;
        }

        sendto(sock, packet, out, 0, (struct sockaddr *)&from, from_len);
    }

    close(sock);
    atomic_store(&s_task_alive, false);
    ESP_LOGI(TAG, "Stopped");
    vTaskDelete(NULL);
}

esp_err_t captive_dns_start(uint32_t ip)
{
    /* Stopped a moment ago: let the old task close its socket first. */
    for (int i = 0; i < 30 && atomic_load(&s_task_alive) && !atomic_load(&s_running); i++)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (atomic_load(&s_task_alive))
    {
        s_ip = ip;
        return ESP_OK;
    }

    s_ip = ip;
    atomic_store(&s_running, true);
    atomic_store(&s_task_alive, true);

    if (!sys_task_create(dns_task, "captive_dns", TASK_STACK, NULL, 4, SYS_CORE_ANY))
    {
        atomic_store(&s_running, false);
        atomic_store(&s_task_alive, false);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Answering every name with the watch address");

    return ESP_OK;
}

void captive_dns_stop(void)
{
    /* The task notices within its 1 s receive timeout and closes the socket. */
    atomic_store(&s_running, false);
}
