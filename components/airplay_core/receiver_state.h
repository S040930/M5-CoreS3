#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  RECEIVER_STATE_BOOT = 0,
  RECEIVER_STATE_CONFIG_REQUIRED,
  RECEIVER_STATE_NETWORK_READY,
  RECEIVER_STATE_DISCOVERABLE,
  RECEIVER_STATE_SESSION_ESTABLISHING,
  RECEIVER_STATE_STREAMING,
  RECEIVER_STATE_RECOVERING,
  RECEIVER_STATE_FAULT,
} receiver_state_t;

typedef enum {
  RECEIVER_EVENT_BOOT = 0,
  RECEIVER_EVENT_CONFIG_REQUIRED,
  RECEIVER_EVENT_NETWORK_READY,
  RECEIVER_EVENT_DISCOVERABLE,
  RECEIVER_EVENT_SESSION_ESTABLISHING,
  RECEIVER_EVENT_STREAMING,
  RECEIVER_EVENT_RECOVERING,
  RECEIVER_EVENT_FAULT,
} receiver_event_t;

typedef struct {
  receiver_state_t state;
  uint64_t last_change_us;
  bool config_required;
  bool network_ready;
  bool discoverable;
  bool session_establishing;
  bool streaming;
  bool recovering;
  bool faulted;
} receiver_state_snapshot_t;

void receiver_state_init(void);
void receiver_state_dispatch(receiver_event_t event);
void receiver_state_set_config_required(bool required);
void receiver_state_set_network_ready(bool ready);
void receiver_state_set_discoverable(bool discoverable);
void receiver_state_set_session_establishing(bool active);
void receiver_state_set_streaming(bool streaming);
void receiver_state_set_recovering(bool recovering);
void receiver_state_set_faulted(bool faulted);
receiver_state_t receiver_state_get(void);
void receiver_state_get_snapshot(receiver_state_snapshot_t *snapshot);
const char *receiver_state_to_str(receiver_state_t state);
