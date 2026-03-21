/**
 * @file captive_dns.c
 * @brief Minimal DNS server for AP-mode captive portal
 *
 * Listens on UDP port 53 and responds to ALL DNS queries with
 * 192.168.4.1 (the ESP32 AP gateway IP).  This triggers the
 * "Sign in to network" popup on phones and desktops.
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "captive_dns";

#define DNS_PORT         53
#define DNS_MAX_PACKET   512
#define CAPTIVE_IP       { 192, 168, 4, 1 }

/* DNS header — 12 bytes */
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static TaskHandle_t s_dns_task = NULL;
static volatile bool s_running = false;

/**
 * @brief DNS server task — answers every query with the captive portal IP
 */
static void dns_server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* Set receive timeout so we can check s_running periodically */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "Captive DNS server started on port %d", DNS_PORT);

    uint8_t rx_buf[DNS_MAX_PACKET];
    uint8_t tx_buf[DNS_MAX_PACKET];

    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int rx_len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0,
                              (struct sockaddr *)&client_addr, &client_len);
        if (rx_len < (int)sizeof(dns_header_t)) {
            continue;  /* timeout or short packet */
        }

        dns_header_t *qhdr = (dns_header_t *)rx_buf;

        /* Build response: copy the query, set answer flags, append answer RR */
        memcpy(tx_buf, rx_buf, rx_len);
        dns_header_t *rhdr = (dns_header_t *)tx_buf;

        /* Set response flags: QR=1, AA=1, RD=1, RA=1 */
        rhdr->flags   = htons(0x8580);
        rhdr->ancount = htons(1);
        rhdr->nscount = 0;
        rhdr->arcount = 0;

        /* Append answer resource record after the query section */
        int offset = rx_len;

        /* Name pointer to the question name (offset 12 in DNS packet) */
        tx_buf[offset++] = 0xC0;
        tx_buf[offset++] = 0x0C;

        /* Type A (1) */
        tx_buf[offset++] = 0x00;
        tx_buf[offset++] = 0x01;

        /* Class IN (1) */
        tx_buf[offset++] = 0x00;
        tx_buf[offset++] = 0x01;

        /* TTL = 60 seconds */
        tx_buf[offset++] = 0x00;
        tx_buf[offset++] = 0x00;
        tx_buf[offset++] = 0x00;
        tx_buf[offset++] = 0x3C;

        /* RDLENGTH = 4 (IPv4) */
        tx_buf[offset++] = 0x00;
        tx_buf[offset++] = 0x04;

        /* RDATA = 192.168.4.1 */
        uint8_t ip[] = CAPTIVE_IP;
        memcpy(&tx_buf[offset], ip, 4);
        offset += 4;

        sendto(sock, tx_buf, offset, 0,
               (struct sockaddr *)&client_addr, client_len);
    }

    close(sock);
    ESP_LOGI(TAG, "Captive DNS server stopped");
    vTaskDelete(NULL);
}

/* ===== Public API ===== */

void captive_dns_start(void)
{
    if (s_running) return;
    s_running = true;
    xTaskCreate(dns_server_task, "captive_dns", 4096, NULL, 5, &s_dns_task);
}

void captive_dns_stop(void)
{
    if (!s_running) return;
    s_running = false;
    /* Task will exit after the next recv timeout (~2 s) */
    if (s_dns_task) {
        /* Wait for task to finish cleanly */
        vTaskDelay(pdMS_TO_TICKS(3000));
        s_dns_task = NULL;
    }
}
