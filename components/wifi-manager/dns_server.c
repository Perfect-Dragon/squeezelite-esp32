/*
Copyright (c) 2019 Tony Pottier

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

@file dns_server.c
@author Tony Pottier
@brief Defines an extremely basic DNS server for captive portal functionality.
It's basically a DNS hijack that replies to the esp's address no matter which
request is sent to it.

Contains the freeRTOS task for the DNS server that processes the requests.

@see https://idyl.io
@see https://github.com/tonyp7/esp32-wifi-manager
*/

#include "dns_server.h"

#include <lwip/sockets.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_err.h>
#include <nvs_flash.h>

#include <lwip/err.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/netdb.h>
#include <lwip/dns.h>

#include <byteswap.h>
#include "squeezelite-ota.h"
#include "network_manager.h"

static const char TAG[] = "dns_server";
static TaskHandle_t task_dns_server = NULL;
static volatile bool dns_stop_requested = false;
static int socket_fd = -1;

void  dns_server_start(esp_netif_t * netif) {
    if (task_dns_server) { ESP_LOGW(TAG, "DNS server already running"); return; }
    dns_stop_requested = false;
    if (xTaskCreate(&dns_server, "dns_server", 3072, (void *)netif, WIFI_MANAGER_TASK_PRIORITY-1, &task_dns_server) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create DNS server task");
        task_dns_server = NULL;
    }
}

/* Never vTaskDelete() the server from outside: it is blocked in recvfrom(), and lwIP
 * only frees a socket slot once every call using it has returned. Killing the task
 * mid-call leaks the slot for good, even after close(). Ask it to stop instead; its
 * recvfrom() times out every 500 ms, then it closes its own socket and exits. */
void  dns_server_stop(){
    if (!task_dns_server) return;
    dns_stop_requested = true;
    for (int i = 0; i < 40 && task_dns_server; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (task_dns_server) ESP_LOGE(TAG, "DNS server task did not stop");
}

static void dns_server_exit(void) {
    if (socket_fd >= 0) { close(socket_fd); socket_fd = -1; }
    task_dns_server = NULL;
    vTaskDelete(NULL);
}


void  dns_server(void *pvParameters) {
    struct sockaddr_in sa, ra;
    esp_err_t esp_err = ESP_OK;
    esp_netif_t * netif = (esp_netif_t * )pvParameters;
    /* Set redirection DNS hijack to the access point IP */
    ip4_addr_t ip_resolved;
    inet_pton(AF_INET, DEFAULT_AP_IP, &ip_resolved);


    /* Create UDP socket */
    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0){
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        dns_server_exit();
    }
    memset(&sa, 0, sizeof(struct sockaddr_in));

    /* Bind to port 53 (typical DNS Server port) */
    esp_netif_ip_info_t ip_info;
    esp_err = esp_netif_get_ip_info(netif,&ip_info);
    if(esp_err!=ESP_OK)    {
        ESP_LOGE(TAG, "Failed to get adapter info for udp: %s", esp_err_to_name(esp_err));
        dns_server_exit();
    }
    ra.sin_family = AF_INET;
    ra.sin_addr.s_addr = ip_info.ip.addr;
    ra.sin_port = htons(53);
    if (bind(socket_fd, (struct sockaddr *)&ra, sizeof(struct sockaddr_in)) == -1) {
        ESP_LOGE(TAG, "Failed to bind to 53/udp: errno %d", errno);
        dns_server_exit();
    }

    /* wake up regularly so dns_server_stop() is noticed */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in client;
    socklen_t client_len;
    client_len = sizeof(client);
    int length;
    uint8_t data[DNS_QUERY_MAX_SIZE];	/* dns query buffer */
    uint8_t response[DNS_ANSWER_MAX_SIZE]; /* dns response buffer */
    char ip_address[INET_ADDRSTRLEN]; /* buffer to store IPs as text. This is only used for debug and serves no other purpose */
    char *domain; /* This is only used for debug and serves no other purpose */
    int err;

    ESP_LOGI(TAG, "DNS Server listening on 53/udp");

    /* Start loop to process DNS requests */
    while (!dns_stop_requested) {

    	memset(data, 0x00,  sizeof(data)); /* reset buffer */
        client_len = sizeof(client);
        length = recvfrom(socket_fd, data, sizeof(data), 0, (struct sockaddr *)&client, &client_len); /* read udp request */

        /*if the query is bigger than the buffer size we simply ignore it. This case should only happen in case of multiple
         * queries within the same DNS packet and is not supported by this simple DNS hijack. */
        if ( length > 0   &&  ((length + sizeof(dns_answer_t)-1) < DNS_ANSWER_MAX_SIZE)   ) {

        	data[length] = '\0'; /*in case there's a bogus domain name that isn't null terminated */

            /* Generate header message */
            memcpy(response, data, sizeof(dns_header_t));
            dns_header_t *dns_header = (dns_header_t*)response;
            dns_header->QR = 1; /*response bit */
            dns_header->OPCode  = DNS_OPCODE_QUERY; /* no support for other type of response */
            dns_header->AA = 1; /*authoritative answer */
            dns_header->RCode = DNS_REPLY_CODE_NO_ERROR; /* no error */
            dns_header->TC = 0; /*no truncation */
            dns_header->RD = 0; /*no recursion */
            dns_header->ANCount = dns_header->QDCount; /* set answer count = question count -- duhh! */
            dns_header->NSCount = 0x0000; /* name server resource records = 0 */
            dns_header->ARCount = 0x0000; /* resource records = 0 */


            /* copy the rest of the query in the response */
            memcpy(response + sizeof(dns_header_t), data + sizeof(dns_header_t), length - sizeof(dns_header_t));


            /* extract domain name and request IP for debug */
            inet_ntop(AF_INET, &(client.sin_addr), ip_address, INET_ADDRSTRLEN);
            domain = (char*) &data[sizeof(dns_header_t) + 1];
            for(char* c=domain; *c != '\0'; c++){
            	if(*c < ' ' || *c > 'z') *c = '.'; /* technically we should test if the first two bits are 00 (e.g. if( (*c & 0xC0) == 0x00) *c = '.') but this makes the code a lot more readable */
            }
            ESP_LOGD(TAG, "Replying to DNS request for %s from %s", domain, ip_address);


            /* create DNS answer at the end of the query*/
            dns_answer_t *dns_answer = (dns_answer_t*)&response[length];
            dns_answer->NAME = __bswap_16(0xC00C); /* This is a pointer to the beginning of the question. As per DNS standard, first two bits must be set to 11 for some odd reason hence 0xC0 */
            dns_answer->TYPE = __bswap_16(DNS_ANSWER_TYPE_A);
            dns_answer->CLASS = __bswap_16(DNS_ANSWER_CLASS_IN);
            dns_answer->TTL = (uint32_t)0x00000000; /* no caching. Avoids DNS poisoning since this is a DNS hijack */
            dns_answer->RDLENGTH = __bswap_16(0x0004); /* 4 byte => size of an ipv4 address */
            dns_answer->RDATA = ip_resolved.addr;

            err = sendto(socket_fd, response, length+sizeof(dns_answer_t), 0, (struct sockaddr *)&client, client_len);
            if (err < 0) {
            	ESP_LOGE(TAG, "UDP sendto failed: %d", err);
            }
        }

        taskYIELD(); /* allows the freeRTOS scheduler to take over if needed. DNS daemon should not be taxing on the system */

    }

    ESP_LOGI(TAG, "DNS Server stopped");
    dns_server_exit();
}




