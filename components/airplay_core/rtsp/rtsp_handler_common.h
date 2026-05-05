#pragma once

#include "rtsp_message.h"
#include "rtsp_conn.h"
#include "audio_receiver.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#define METADATA_STRING_MAX 128
#define RTSP_MAX_ARTWORK_BYTES ((size_t)2U * 1024U * 1024U)

typedef enum {
  SDP_PARSE_OK = 0,
  SDP_PARSE_INVALID_ENCRYPTION,
  SDP_PARSE_INVALID_BODY,
} sdp_parse_result_t;

typedef enum {
  RTSP_EVENT_CLIENT_CONNECTED,
  RTSP_EVENT_PLAYING,
  RTSP_EVENT_PAUSED,
  RTSP_EVENT_DISCONNECTED,
  RTSP_EVENT_METADATA,
} rtsp_event_t;

typedef struct {
  char title[METADATA_STRING_MAX];
  char artist[METADATA_STRING_MAX];
  char album[METADATA_STRING_MAX];
  char genre[METADATA_STRING_MAX];
  uint32_t position_secs;
  uint32_t duration_secs;
  void *artwork_data;
  size_t artwork_len;
  bool has_artwork;
} rtsp_metadata_t;

typedef struct {
  const char *name;
  int64_t type_id;
} rtsp_codec_t;

typedef union {
  rtsp_metadata_t metadata;
} rtsp_event_data_t;

typedef void (*rtsp_handler_fn)(int socket, struct rtsp_conn *conn,
                                const rtsp_request_t *req,
                                const uint8_t *raw, size_t raw_len);

typedef struct {
  const char *method;
  rtsp_handler_fn handler;
} rtsp_method_handler_t;

bool rtsp_codec_configure(int64_t type_id, audio_format_t *fmt,
                          int64_t sample_rate, int64_t samples_per_frame);
void rtsp_get_device_id(char *device_id, size_t len);
void rtsp_format_time_mmss(uint32_t seconds, char *out, size_t out_size);

int rtsp_dispatch(int socket, rtsp_conn_t *conn, const uint8_t *raw_request,
                  size_t raw_len);

sdp_parse_result_t rtsp_parse_sdp(rtsp_conn_t *conn, const char *sdp,
                                  size_t len);
void rtsp_parse_dmap_metadata(const uint8_t *data, size_t len,
                              rtsp_metadata_t *meta);
void rtsp_parse_progress(const char *progress_str, uint32_t sample_rate,
                         rtsp_metadata_t *meta);
bool rtsp_parse_rate_channels(const char *text, int *sample_rate, int *channels);
bool rtsp_parse_fmtp_config(const char *fmtp, audio_format_t *format);
bool rtsp_parse_int_token(const char *text, int *value);
bool rtsp_parse_uint32_token(const char *text, uint32_t *value);
bool rtsp_parse_double_token(const char *text, double *value);
bool rtsp_parse_uint64_triplet(const char *text, uint64_t *first,
                               uint64_t *second, uint64_t *third);
size_t rtsp_split_whitespace_tokens(char *text, char *tokens[],
                                    size_t max_tokens);
const char *rtsp_parse_raw_header(const uint8_t *raw, size_t raw_len,
                                  const char *name);
void rtsp_ensure_stream_ports(rtsp_conn_t *conn, bool buffered);
void rtsp_flush_output_if_active(void);
