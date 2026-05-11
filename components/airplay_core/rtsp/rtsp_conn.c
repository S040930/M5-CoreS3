#include "rtsp_conn.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_volume.h"
#include "esp_heap_caps.h"

static int32_t volume_db_to_q15(float volume_db) {
  return audio_volume_db_to_q15(volume_db);
}

rtsp_conn_t *rtsp_conn_create(void) {
  rtsp_conn_t *conn = heap_caps_malloc(sizeof(rtsp_conn_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!conn) conn = heap_caps_malloc(sizeof(rtsp_conn_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (conn) memset(conn, 0, sizeof(rtsp_conn_t));
  if (!conn) {
    return NULL;
  }

  /* Default max (0 dB); NVS overrides when present. load() always fills saved_volume when possible. */
  float saved_volume = 0.0f;
  (void)audio_volume_load(&saved_volume);
  conn->volume_db = saved_volume;
  conn->volume_q15 = volume_db_to_q15(saved_volume);

  conn->data_socket = -1;
  conn->control_socket = -1;
  conn->event_socket = -1;
  conn->announce_ready = false;

  return conn;
}

void rtsp_conn_free(rtsp_conn_t *conn) {
  if (!conn) {
    return;
  }

  audio_volume_persist();

  // Cleanup any resources
  rtsp_conn_cleanup(conn);

  free(conn);
}

void rtsp_conn_reset_stream(rtsp_conn_t *conn) {
  if (!conn) {
    return;
  }

  // Reset stream state but keep session alive
  conn->stream_active = false;
  conn->stream_paused = true; // Paused, not fully torn down
  conn->announce_ready = false;

  // Keep ports allocated for quick resume
  // Don't clear: data_port, control_port, timing_port, event_port
}

void rtsp_conn_cleanup(rtsp_conn_t *conn) {
  if (!conn) {
    return;
  }

  // Note: audio_receiver_stop() is NOT called here — it is a global operation
  // and must be managed by the caller (rtsp_server cleanup / handle_teardown)
  // to avoid killing a new session's audio during client replacement.

  // Close sockets
  if (conn->data_socket >= 0) {
    close(conn->data_socket);
    conn->data_socket = -1;
  }
  if (conn->control_socket >= 0) {
    close(conn->control_socket);
    conn->control_socket = -1;
  }
  if (conn->event_socket >= 0) {
    close(conn->event_socket);
    conn->event_socket = -1;
  }

  // Reset stream state
  conn->stream_active = false;
  conn->stream_paused = false;
  conn->data_port = 0;
  conn->control_port = 0;
  conn->timing_port = 0;
  conn->event_port = 0;
  conn->buffered_port = 0;
  conn->announce_ready = false;

}

void rtsp_conn_set_volume(rtsp_conn_t *conn, float volume_db) {
  if (!conn) {
    return;
  }

  conn->volume_db = volume_db;
  conn->volume_q15 = volume_db_to_q15(volume_db);
  audio_volume_save(volume_db);
}

int32_t rtsp_conn_get_volume_q15(rtsp_conn_t *conn) {
  if (!conn) {
    return 32768; // Default full volume
  }
  return conn->volume_q15;
}
