#include "rtsp_handler_common.h"

#include <string.h>

#include "esp_log.h"
#include "audio_receiver.h"

void rtsp_handle_announce(int socket, rtsp_conn_t *conn,
                          const rtsp_request_t *req, const uint8_t *raw,
                          size_t raw_len) {
  (void)raw;
  (void)raw_len;

  conn->announce_ready = false;
  audio_receiver_clear_encryption();

  if (req->body && req->body_len > 0) {
    sdp_parse_result_t parse_result =
        rtsp_parse_sdp(conn, (const char *)req->body, req->body_len);
    if (parse_result != SDP_PARSE_OK) {
      ESP_LOGW("rtsp_handlers",
               "ANNOUNCE rejected: invalid SDP/encryption parameters "
               "(result=%d cseq=%d)",
               (int)parse_result, req->cseq);
      rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq, NULL,
                         NULL, 0);
      return;
    }
    conn->announce_ready = true;
  } else {
    ESP_LOGW("rtsp_handlers", "ANNOUNCE rejected: missing SDP body (cseq=%d)",
             req->cseq);
    rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq, NULL,
                       NULL, 0);
    return;
  }

  rtsp_send_ok(socket, conn, req->cseq);
}
