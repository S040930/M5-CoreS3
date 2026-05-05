#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rtsp_handler_common.h"

int rtsp_dispatch(int socket, rtsp_conn_t *conn, const uint8_t *raw_request,
                  size_t raw_len);
