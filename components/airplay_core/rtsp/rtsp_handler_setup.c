#include "rtsp_handler_common.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "audio_receiver.h"
#include "ntp_clock.h"

void rtsp_handle_setup(int socket, rtsp_conn_t *conn, const rtsp_request_t *req,
                       const uint8_t *raw, size_t raw_len) {
  (void)raw_len;

  if (!conn->announce_ready) {
    ESP_LOGW("rtsp_handlers",
             "SETUP rejected before valid ANNOUNCE (cseq=%d)", req->cseq);
    rtsp_send_response(socket, conn, 455, "Method Not Valid in This State",
                       req->cseq, NULL, NULL, 0);
    return;
  }

  ESP_LOGI("rtsp_handlers", "SETUP: AirPlay v1 stream setup");

  int64_t stream_type = 96;
  conn->stream_type = stream_type;

  rtsp_parse_transport((const char *)raw, &conn->client_control_port,
                       &conn->client_timing_port);
  ESP_LOGI("rtsp_handlers", "Client ports: control=%u timing=%u",
           conn->client_control_port, conn->client_timing_port);

  if (conn->client_timing_port > 0 && conn->client_ip != 0) {
    ntp_clock_start_client(conn->client_ip, conn->client_timing_port);
  }

  rtsp_ensure_stream_ports(conn, false);
  ESP_LOGI("rtsp_handlers",
           "SETUP ports: client_control=%u client_timing=%u server_data=%u "
           "server_control=%u server_timing=%u",
           conn->client_control_port, conn->client_timing_port,
           conn->data_port, conn->control_port, conn->timing_port);

  char transport_response[256];
  snprintf(transport_response, sizeof(transport_response),
           "Transport: RTP/AVP/UDP;unicast;mode=record;"
           "server_port=%d;control_port=%d;timing_port=%d\r\n"
           "Session: 1\r\n",
           conn->data_port, conn->control_port, conn->timing_port);
  rtsp_send_response(socket, conn, 200, "OK", req->cseq, transport_response,
                     NULL, 0);

  ESP_LOGI("rtsp_handlers",
           "SETUP: using ANNOUNCE audio format codec=%s sr=%d ch=%d bits=%d",
           conn->codec[0] ? conn->codec : "unknown", conn->sample_rate,
           conn->channels, conn->bits_per_sample);
  audio_receiver_set_stream_type((audio_stream_type_t)stream_type);

  if (conn->client_control_port > 0 && conn->client_ip != 0) {
    audio_receiver_set_client_control(conn->client_ip,
                                      conn->client_control_port);
  }

  conn->stream_active = true;
}
