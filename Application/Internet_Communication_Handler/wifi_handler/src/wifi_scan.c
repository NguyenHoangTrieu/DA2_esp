/* 
* WiFi Scan for esp32s3
*/
#include "wifi_scan.h"

static const char *TAG = "scan";
/* Only scan channels 1, 6, and 11 (if bitmap enabled) */
#if USE_CHANNEL_BITMAP
uint8_t channel_list[CHANNEL_LIST_SIZE] = {1, 6, 11};
#endif

void print_auth_mode(int authmode) {
  switch (authmode) {
  case WIFI_AUTH_OPEN:
    ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_OPEN");
    break;
  case WIFI_AUTH_OWE:
    ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_OWE");
    break;
  case WIFI_AUTH_WEP:
    ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WEP");
    break;
  case WIFI_AUTH_WPA_PSK:
    ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA_PSK");
    break;
  case WIFI_AUTH_WPA2_PSK:
    ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA2_PSK");
    break;
  case WIFI_AUTH_WPA_WPA2_PSK:
    ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA_WPA2_PSK");
    break;
  case WIFI_AUTH_ENTERPRISE:
    ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_ENTERPRISE");
    break;
  case WIFI_AUTH_WPA3_PSK:
    ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA3_PSK");
    break;
  case WIFI_AUTH_WPA2_WPA3_PSK:
    ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA2_WPA3_PSK");
    break;
  case WIFI_AUTH_WPA3_ENTERPRISE:
    ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA3_ENTERPRISE");
    break;
  case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
    ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA2_WPA3_ENTERPRISE");
    break;
  case WIFI_AUTH_WPA3_ENT_192:
    ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA3_ENT_192");
    break;
  default:
    ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_UNKNOWN");
    break;
  }
}

void print_cipher_type(int pairwise_cipher, int group_cipher) {
  switch (pairwise_cipher) {
  case WIFI_CIPHER_TYPE_NONE:
    ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_NONE");
    break;
  case WIFI_CIPHER_TYPE_WEP40:
    ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_WEP40");
    break;
  case WIFI_CIPHER_TYPE_WEP104:
    ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_WEP104");
    break;
  case WIFI_CIPHER_TYPE_TKIP:
    ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_TKIP");
    break;
  case WIFI_CIPHER_TYPE_CCMP:
    ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_CCMP");
    break;
  case WIFI_CIPHER_TYPE_TKIP_CCMP:
    ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_TKIP_CCMP");
    break;
  case WIFI_CIPHER_TYPE_AES_CMAC128:
    ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_AES_CMAC128");
    break;
  case WIFI_CIPHER_TYPE_SMS4:
    ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_SMS4");
    break;
  case WIFI_CIPHER_TYPE_GCMP:
    ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_GCMP");
    break;
  case WIFI_CIPHER_TYPE_GCMP256:
    ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_GCMP256");
    break;
  default:
    ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_UNKNOWN");
    break;
  }

  switch (group_cipher) {
  case WIFI_CIPHER_TYPE_NONE:
    ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_NONE");
    break;
  case WIFI_CIPHER_TYPE_WEP40:
    ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_WEP40");
    break;
  case WIFI_CIPHER_TYPE_WEP104:
    ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_WEP104");
    break;
  case WIFI_CIPHER_TYPE_TKIP:
    ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_TKIP");
    break;
  case WIFI_CIPHER_TYPE_CCMP:
    ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_CCMP");
    break;
  case WIFI_CIPHER_TYPE_TKIP_CCMP:
    ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_TKIP_CCMP");
    break;
  case WIFI_CIPHER_TYPE_SMS4:
    ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_SMS4");
    break;
  case WIFI_CIPHER_TYPE_GCMP:
    ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_GCMP");
    break;
  case WIFI_CIPHER_TYPE_GCMP256:
    ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_GCMP256");
    break;
  default:
    ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_UNKNOWN");
    break;
  }
}

#if USE_CHANNEL_BITMAP
void array_2_channel_bitmap(const uint8_t channel_list[],
                            const uint8_t channel_list_size,
                            wifi_scan_config_t *scan_config) {
  for (uint8_t i = 0; i < channel_list_size; i++) {
    uint8_t channel = channel_list[i];
    scan_config->channel_bitmap.ghz_2_channels |= (1 << channel);
  }
}
#endif /*USE_CHANNEL_BITMAP*/

void perform_scan(void) {
  uint16_t number = DEFAULT_SCAN_LIST_SIZE;
  wifi_ap_record_t ap_info[DEFAULT_SCAN_LIST_SIZE];
  uint16_t ap_count = 0;
  memset(ap_info, 0, sizeof(ap_info));

#if USE_CHANNEL_BITMAP
  wifi_scan_config_t *scan_config =
      (wifi_scan_config_t *)calloc(1, sizeof(wifi_scan_config_t));
  if (!scan_config) {
    ESP_LOGE(TAG, "Memory Allocation for scan config failed!");
    return;
  }
  array_2_channel_bitmap(channel_list, CHANNEL_LIST_SIZE, scan_config);
  esp_wifi_scan_start(scan_config, true);
  free(scan_config);
#else
  esp_wifi_scan_start(NULL, true);
#endif /*USE_CHANNEL_BITMAP*/

  ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
  ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_info));

  ESP_LOGI(TAG, "Total APs scanned = %u, actual AP number ap_info holds = %u",
           ap_count, number);

  for (int i = 0; i < number; i++) {
    ESP_LOGI(TAG, "SSID \t\t%s", ap_info[i].ssid);
    ESP_LOGI(TAG, "RSSI \t\t%d", ap_info[i].rssi);
    print_auth_mode(ap_info[i].authmode);
    if (ap_info[i].authmode != WIFI_AUTH_WEP) {
      print_cipher_type(ap_info[i].pairwise_cipher, ap_info[i].group_cipher);
    }
    ESP_LOGI(TAG, "Channel \t\t%d", ap_info[i].primary);
    ESP_LOGI(TAG, "--------------------------------");
  }
}