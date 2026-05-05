#include "rtsp_handler_common.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "rtsp_rsa.h"

void rtsp_handle_options(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len) {
  const char *public_methods =
      "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, FLUSHBUFFERED, TEARDOWN, "
      "OPTIONS, POST, GET, SET_PARAMETER, GET_PARAMETER, SETPEERS, "
      "SETRATEANCHORTIME\r\n";

  const char *challenge = rtsp_parse_raw_header(raw, raw_len, "Apple-Challenge:");
  if (challenge) {
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
      uint8_t mac[6];
      esp_read_mac(mac, ESP_MAC_WIFI_STA);
      char response_b64[512];
      if (rsa_apple_challenge_response(challenge, ip_info.ip.addr, mac,
                                       response_b64,
                                       sizeof(response_b64)) == 0) {
        char headers[768];
        snprintf(headers, sizeof(headers), "%sApple-Response: %s\r\n",
                 public_methods, response_b64);
        rtsp_send_response(socket, conn, 200, "OK", req->cseq, headers, NULL,
                           0);
        return;
      }
    }
    ESP_LOGW("rtsp_handlers", "Failed to build Apple-Challenge response");
  }

  rtsp_send_response(socket, conn, 200, "OK", req->cseq, public_methods, NULL,
                     0);
}
