#include "rtsp_handler_common.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "audio_output.h"
#include "audio_receiver.h"
#include "audio_timing.h"
#include "ntp_clock.h"
#include "rtsp_events.h"

void rtsp_handle_record(int socket, rtsp_conn_t *conn, const rtsp_request_t *req,
                        const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;

  if (!conn->announce_ready) {
    ESP_LOGW("rtsp_handlers",
             "RECORD rejected before valid ANNOUNCE (cseq=%d)", req->cseq);
    rtsp_send_response(socket, conn, 455, "Method Not Valid in This State",
                       req->cseq, NULL, NULL, 0);
    return;
  }

  ESP_LOGI("rtsp_handlers", "RECORD received - starting playback, stream_paused was %d",
           conn->stream_paused);

  if (conn->stream_paused) {
    ESP_LOGI("rtsp_handlers", "RECORD: resuming from pause, skipping stream restart");
    audio_receiver_set_playing(true);
  } else {
    audio_receiver_start_stream(conn->data_port, conn->control_port,
                                conn->buffered_port);
    if (conn->client_control_port > 0 && conn->client_ip != 0) {
      audio_receiver_set_client_control(conn->client_ip,
                                        conn->client_control_port);
    }
    audio_receiver_set_playing(true);
  }
  conn->stream_paused = false;
  rtsp_events_emit(RTSP_EVENT_PLAYING, NULL);

  ESP_LOGI("rtsp_handlers",
           "RECORD: audio_output_active=%d, buffer_frames=%u, anchor_valid=%d, "
           "ntp_locked=%d ntp_tracking=%d reject_streak=%d age_ms=%lld",
           audio_output_is_active(),
           audio_receiver_get_buffered_frames(),
           audio_receiver_anchor_valid(),
           ntp_clock_is_locked(),
           ntp_clock_is_tracking(),
           ntp_clock_get_reject_streak(),
           (long long)ntp_clock_get_last_accept_age_ms());

  audio_timing_force_start();

  char headers[128];
  uint32_t latency_samples = 0;
  snprintf(headers, sizeof(headers),
           "Audio-Latency: %" PRIu32 "\r\n"
           "Audio-Jack-Status: connected\r\n",
           latency_samples);

  rtsp_send_response(socket, conn, 200, "OK", req->cseq, headers, NULL, 0);
}

void rtsp_handle_pause(int socket, rtsp_conn_t *conn, const rtsp_request_t *req,
                       const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;

  ESP_LOGI("rtsp_handlers", "PAUSE received");

  audio_receiver_pause();
  rtsp_flush_output_if_active();
  conn->stream_paused = true;

  rtsp_events_emit(RTSP_EVENT_PAUSED, NULL);

  rtsp_send_ok(socket, conn, req->cseq);
}

void rtsp_handle_flush(int socket, rtsp_conn_t *conn, const rtsp_request_t *req,
                       const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;

  ESP_LOGI("rtsp_handlers", "FLUSH received");
  audio_receiver_seek_flush();
  rtsp_flush_output_if_active();
  rtsp_send_ok(socket, conn, req->cseq);
}

void rtsp_handle_teardown(int socket, rtsp_conn_t *conn,
                          const rtsp_request_t *req, const uint8_t *raw,
                          size_t raw_len) {
  (void)raw;
  (void)raw_len;

  ESP_LOGI("rtsp_handlers", "TEARDOWN received");
  audio_receiver_stop();
  rtsp_flush_output_if_active();
  conn->stream_active = false;
  conn->stream_paused = false;
  ntp_clock_stop();
  conn->timing_port = 0;

  rtsp_send_ok(socket, conn, req->cseq);
}
