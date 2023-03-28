#include <inttypes.h>
#include "esp_mac.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include <sys/socket.h>

#include "esp_bridge.h"
#include "esp_mesh_lite.h"
#include "esp_log.h"

#define PAYLOAD_LEN 1456 /**< Max payload size(in bytes) */

static const char* TAG = "vip_video_meshlite";

static int gs_sock = -1; // Ground station socket connection
static int drone_sock = -1; // Drone socket connection

// TODO: Implement a singly linked queue aka ring buffer to store incoming data (uint8_t)

static int 	socket_udp_client_create(const char *ip, uint16_t port)
{
	ESP_LOGI(TAG, "Creating a UDP client, IP: %s, Port: %d", ip, port);
	int sockfd = -1; // local socket connection
	
	struct ifreq iface;
	memset(&iface, 0x0, sizeof(iface));
	
	struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = inet_addr(ip),
    };
	
	/*
	ipv4: AF_INET, ipv6: AF_INET6
	TCP: SOCK_STREAM, UDP: SOCK_DGRAM
	*/
	sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	
	// Get net interface name from network stack implementation.
	esp_netif_get_netif_impl_name(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), iface.ifr_name);
	connect(sockfd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in));
	
	return sockfd;
}

static esp_err_t esp_storage_init(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    return ret;
}

/**
 * @brief Prints system info on a timed loop
 */
static void print_system_info_timercb(TimerHandle_t timer)
{
    uint8_t primary                 = 0;
    uint8_t sta_mac[6]              = {0};
    wifi_ap_record_t ap_info        = {0};
    wifi_second_chan_t second       = 0;
    wifi_sta_list_t wifi_sta_list   = {0x0};

    esp_wifi_sta_get_ap_info(&ap_info);
    esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);
    esp_wifi_ap_get_sta_list(&wifi_sta_list);
    esp_wifi_get_channel(&primary, &second);

    ESP_LOGI(TAG, "System information, channel: %d, layer: %d, self mac: " MACSTR ", parent bssid: " MACSTR
             ", parent rssi: %d, free heap: %"PRIu32"", primary,
             esp_mesh_lite_get_level(), MAC2STR(sta_mac), MAC2STR(ap_info.bssid),
             (ap_info.rssi != 0 ? ap_info.rssi : -120), esp_get_free_heap_size());

    for (int i = 0; i < wifi_sta_list.num; i++) {
        ESP_LOGI(TAG, "Child mac: " MACSTR, MAC2STR(wifi_sta_list.sta[i].mac));
    }
}

static void udp_forward_task()
{
	ssize_t receive_size;
	uint8_t rx_buf[128];
	
	while(1){
		receive_size = recv(drone_sock, rx_buf, sizeof(rx_buf) - 1, 0);
		send(gs_sock, rx_buf, receive_size, 0);
		ESP_LOGI(TAG, "Forwarded data, Data: %s", rx_buf);
		// TODO: receive and send in different threads
	}
	
	close(drone_sock);
	close(gs_sock);

	free(rx_buf);
}

// This function is called when the station gets assigned an IP address
static void ip_event_sta_got_ip_handler(esp_event_base_t event_base,int32_t event_id, void *event_data)
{
	gs_sock = socket_udp_client_create(CONFIG_GS_IP, CONFIG_GS_PORT);
	drone_sock = socket_udp_client_create(CONFIG_DRONE_IP, CONFIG_DRONE_PORT);
	
	xTaskCreate(udp_forward_task, "udp_forward_task", 4*1024, NULL, 5, NULL);
	/*
	TODO: Instead of rx/tx to servers from all nodes, only root nodes on either side should.
	All other nodes should only connect to mesh-lite network
	*/
}

static esp_err_t wifi_init(void)
{
    // Station
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ROUTER_SSID,
            .password = CONFIG_ROUTER_PASSWORD,
        },
    };
    esp_bridge_wifi_set(WIFI_MODE_STA, (char *)wifi_config.sta.ssid, (char *)wifi_config.sta.password, NULL);

    // Softap
    memset(&wifi_config, 0x0, sizeof(wifi_config_t));
    size_t softap_ssid_len = sizeof(wifi_config.ap.ssid);
    if (esp_mesh_lite_get_softap_ssid_from_nvs((char *)wifi_config.ap.ssid, &softap_ssid_len) != ESP_OK) {
        snprintf((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), "%s", CONFIG_BRIDGE_SOFTAP_SSID);
    }
    size_t softap_psw_len = sizeof(wifi_config.ap.password);
    if (esp_mesh_lite_get_softap_psw_from_nvs((char *)wifi_config.ap.password, &softap_psw_len) != ESP_OK) {
        strlcpy((char *)wifi_config.ap.password, CONFIG_BRIDGE_SOFTAP_PASSWORD, sizeof(wifi_config.ap.password));
    }
    esp_bridge_wifi_set(WIFI_MODE_AP, (char *)wifi_config.ap.ssid, (char *)wifi_config.ap.password, NULL);

    return ESP_OK;
}

void app_main()
{
    esp_log_level_set("*", ESP_LOG_INFO);

    esp_storage_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_bridge_create_all_netif();

    wifi_init();

    esp_mesh_lite_config_t mesh_lite_config = ESP_MESH_LITE_DEFAULT_INIT();
    esp_mesh_lite_init(&mesh_lite_config);

    /**
     * @breif Create handler
     */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_sta_got_ip_handler, NULL, NULL));

    TimerHandle_t timer = xTimerCreate("print_system_info", 10000 / portTICK_PERIOD_MS,
                                       true, NULL, print_system_info_timercb);
    xTimerStart(timer, 0);
}
