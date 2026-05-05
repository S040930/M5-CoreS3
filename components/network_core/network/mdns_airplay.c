#include "esp_log.h"
#include "esp_mac.h"
#include "esp_err.h"
#include "mdns.h"
#include <stdio.h>
#include <string.h>

#include "mdns_airplay.h"
#include "wifi.h"

static const char *TAG = "mdns_airplay";
static bool s_mdns_inited = false;
static bool s_raop_added = false;

#define AIRPLAY_PROTOCOL_VERSION "1"
#define AIRPLAY_SOURCE_VERSION "377.40.00"

// Flags: 0x4 = audio receiver
#define AIRPLAY_FLAGS "0x4"

// Model identifier - AudioAccessory for speaker appearance
// AppleTV3,2 = Apple TV, AudioAccessory5,1 = HomePod mini (speaker)
#define AIRPLAY_MODEL "AudioAccessory5,1"

static void build_mdns_hostname(const char *device_name, char *hostname,
                                size_t hostname_len) {
  if (!hostname || hostname_len == 0) {
    return;
  }

  size_t out = 0;
  bool last_was_dash = false;

  for (size_t i = 0; device_name && device_name[i] != '\0' &&
                     out + 1 < hostname_len;
       i++) {
    unsigned char ch = (unsigned char)device_name[i];
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
      hostname[out++] = (char)ch;
      last_was_dash = false;
    } else if (ch >= 'A' && ch <= 'Z') {
      hostname[out++] = (char)(ch - 'A' + 'a');
      last_was_dash = false;
    } else if (!last_was_dash && out > 0) {
      hostname[out++] = '-';
      last_was_dash = true;
    }
  }

  while (out > 0 && hostname[out - 1] == '-') {
    out--;
  }

  if (out == 0) {
    strncpy(hostname, "esp32-airplay", hostname_len - 1);
    hostname[hostname_len - 1] = '\0';
    return;
  }

  hostname[out] = '\0';
}

esp_err_t mdns_airplay_init(const char *device_name) {
  char mac_str[18];
  char device_id[18];
  char service_name[80];
  char hostname[65];

  if (!device_name) return ESP_ERR_INVALID_ARG;

  build_mdns_hostname(device_name, hostname, sizeof(hostname));
  wifi_get_mac_str(mac_str, sizeof(mac_str));
  strncpy(device_id, mac_str, sizeof(device_id));

  // Service name format: <MAC>@<DeviceName>
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(service_name, sizeof(service_name), "%02X%02X%02X%02X%02X%02X@%s",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], device_name);

  // Initialize mDNS
  esp_err_t err = mdns_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
    return err;
  }
  s_mdns_inited = true;

  ESP_LOGI(TAG, "mDNS hostname: %s (device name: %s)", hostname, device_name ? device_name : "(null)");
  err = mdns_hostname_set(hostname);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set mDNS hostname: %s", esp_err_to_name(err));
    return err;
  }

  // Avoid duplicate _raop registration after reconnect/restart cycles.
  (void)mdns_service_remove("_raop", "_tcp");

  // ========================================
  // _raop._tcp service (port 7000)
  // Classic AirPlay 1 (RAOP) advertisement
  // ========================================
  mdns_txt_item_t raop_txt[] = {
      {"am", AIRPLAY_MODEL},
      {"tp", "UDP"},      // Transport protocol
      {"sm", "false"},    // Sharing mode
      {"sv", "false"},    // Server version (unused)
      {"ek", "1"},        // Encryption key available
      {"et", "0,1"},      // Encryption types: none, RSA
      {"md", "0,1,2"},    // Metadata types
      {"cn", "0,1"},      // Audio codecs: PCM, ALAC
      {"ch", "2"},        // Channels
      {"ss", "16"},       // Sample size (bits)
      {"sr", "44100"},    // Sample rate
      {"vn", "3"},        // Version number
      {"txtvers", "1"},   // TXT record version
  };

  esp_err_t err_raop =
      mdns_service_add(service_name, "_raop", "_tcp", 7000, raop_txt,
                       sizeof(raop_txt) / sizeof(raop_txt[0]));
  if (err_raop != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add _raop._tcp service: %s",
             esp_err_to_name(err_raop));
  } else {
    s_raop_added = true;
    ESP_LOGI(TAG, "_raop._tcp published on port 7000 as %s", service_name);
  }
  return err_raop;
}

void mdns_airplay_deinit(void) {
  if (s_raop_added) {
    esp_err_t remove_err = mdns_service_remove("_raop", "_tcp");
    if (remove_err != ESP_OK) {
      ESP_LOGW(TAG, "Failed to remove _raop._tcp: %s",
               esp_err_to_name(remove_err));
    } else {
      ESP_LOGI(TAG, "_raop._tcp removed");
    }
    s_raop_added = false;
  }

  if (s_mdns_inited) {
    mdns_free();
    s_mdns_inited = false;
    ESP_LOGI(TAG, "mDNS deinitialized");
  }
}
