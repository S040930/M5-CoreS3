#include "rtsp_handlers.h"
#include "rtsp_handler_common.h"

#include <string.h>
#include <strings.h>

#include "esp_log.h"

extern void rtsp_handle_options(int socket, rtsp_conn_t *conn,
                                const rtsp_request_t *req,
                                const uint8_t *raw, size_t raw_len);
extern void rtsp_handle_get(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req,
                            const uint8_t *raw, size_t raw_len);
extern void rtsp_handle_post(int socket, rtsp_conn_t *conn,
                             const rtsp_request_t *req,
                             const uint8_t *raw, size_t raw_len);
extern void rtsp_handle_announce(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req,
                                 const uint8_t *raw, size_t raw_len);
extern void rtsp_handle_setup(int socket, rtsp_conn_t *conn,
                              const rtsp_request_t *req,
                              const uint8_t *raw, size_t raw_len);
extern void rtsp_handle_record(int socket, rtsp_conn_t *conn,
                               const rtsp_request_t *req,
                               const uint8_t *raw, size_t raw_len);
extern void rtsp_handle_set_parameter(int socket, rtsp_conn_t *conn,
                                      const rtsp_request_t *req,
                                      const uint8_t *raw, size_t raw_len);
extern void rtsp_handle_get_parameter(int socket, rtsp_conn_t *conn,
                                      const rtsp_request_t *req,
                                      const uint8_t *raw, size_t raw_len);
extern void rtsp_handle_pause(int socket, rtsp_conn_t *conn,
                              const rtsp_request_t *req,
                              const uint8_t *raw, size_t raw_len);
extern void rtsp_handle_flush(int socket, rtsp_conn_t *conn,
                              const rtsp_request_t *req,
                              const uint8_t *raw, size_t raw_len);
extern void rtsp_handle_teardown(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req,
                                 const uint8_t *raw, size_t raw_len);

static const rtsp_method_handler_t method_handlers[] = {
    {"OPTIONS", rtsp_handle_options},
    {"GET", rtsp_handle_get},
    {"POST", rtsp_handle_post},
    {"ANNOUNCE", rtsp_handle_announce},
    {"SETUP", rtsp_handle_setup},
    {"RECORD", rtsp_handle_record},
    {"SET_PARAMETER", rtsp_handle_set_parameter},
    {"GET_PARAMETER", rtsp_handle_get_parameter},
    {"PAUSE", rtsp_handle_pause},
    {"FLUSH", rtsp_handle_flush},
    {"TEARDOWN", rtsp_handle_teardown},
    {NULL, NULL}};

int rtsp_dispatch(int socket, rtsp_conn_t *conn, const uint8_t *raw_request,
                  size_t raw_len) {
  rtsp_request_t req;
  if (rtsp_request_parse(raw_request, raw_len, &req) < 0) {
    ESP_LOGW("rtsp_handlers", "Failed to parse RTSP request");
    return -1;
  }

  ESP_LOGI("rtsp_handlers",
           "RTSP request: method=%s path=%s cseq=%d body_len=%zu",
           req.method, req.path, req.cseq, req.body_len);

  for (const rtsp_method_handler_t *h = method_handlers; h->method; h++) {
    if (strcasecmp(req.method, h->method) == 0) {
      h->handler(socket, conn, &req, raw_request, raw_len);
      return 0;
    }
  }

  ESP_LOGW("rtsp_handlers", "Unknown method: %s", req.method);
  rtsp_send_http_response(socket, conn, 501, "Not Implemented", "text/plain",
                          "Not Implemented", 15);
  return 0;
}
