#include "receiver_state.h"

#include "esp_timer.h"
#include <string.h>

static receiver_state_snapshot_t s_state;

static receiver_state_t choose_state(void) {
  if (s_state.faulted) {
    return RECEIVER_STATE_FAULT;
  }
  if (s_state.recovering) {
    return RECEIVER_STATE_RECOVERING;
  }
  if (s_state.streaming) {
    return RECEIVER_STATE_STREAMING;
  }
  if (s_state.session_establishing) {
    return RECEIVER_STATE_SESSION_ESTABLISHING;
  }
  if (s_state.discoverable) {
    return RECEIVER_STATE_DISCOVERABLE;
  }
  if (s_state.network_ready) {
    return RECEIVER_STATE_NETWORK_READY;
  }
  if (s_state.setup_ap_enabled) {
    return RECEIVER_STATE_SETUP_AP;
  }
  return RECEIVER_STATE_BOOT;
}

static void refresh_state(void) {
  receiver_state_t next = choose_state();
  if (next != s_state.state) {
    s_state.last_change_us = (uint64_t)esp_timer_get_time();
  }
  s_state.state = next;
}

void receiver_state_init(void) {
  memset(&s_state, 0, sizeof(s_state));
  s_state.last_change_us = (uint64_t)esp_timer_get_time();
  refresh_state();
}

void receiver_state_dispatch(receiver_event_t event) {
  switch (event) {
  case RECEIVER_EVENT_BOOT:
    memset(&s_state, 0, sizeof(s_state));
    s_state.last_change_us = (uint64_t)esp_timer_get_time();
    break;
  case RECEIVER_EVENT_NETWORK_READY:
    s_state.network_ready = true;
    break;
  case RECEIVER_EVENT_DISCOVERABLE:
    s_state.discoverable = true;
    break;
  case RECEIVER_EVENT_SESSION_ESTABLISHING:
    s_state.session_establishing = true;
    break;
  case RECEIVER_EVENT_STREAMING:
    s_state.streaming = true;
    break;
  case RECEIVER_EVENT_RECOVERING:
    s_state.recovering = true;
    break;
  case RECEIVER_EVENT_FAULT:
    s_state.faulted = true;
    break;
  }
  refresh_state();
}

void receiver_state_set_network_ready(bool ready) {
  s_state.network_ready = ready;
  if (!ready) {
    s_state.discoverable = false;
    s_state.session_establishing = false;
    s_state.streaming = false;
  }
  refresh_state();
}

void receiver_state_set_setup_ap_enabled(bool enabled) {
  s_state.setup_ap_enabled = enabled;
  refresh_state();
}

void receiver_state_set_discoverable(bool discoverable) {
  s_state.discoverable = discoverable;
  if (!discoverable) {
    s_state.session_establishing = false;
    s_state.streaming = false;
  }
  refresh_state();
}

void receiver_state_set_session_establishing(bool active) {
  s_state.session_establishing = active;
  if (active) {
    s_state.discoverable = false;
    s_state.streaming = false;
  }
  refresh_state();
}

void receiver_state_set_streaming(bool streaming) {
  s_state.streaming = streaming;
  if (streaming) {
    s_state.discoverable = false;
    s_state.session_establishing = false;
    s_state.recovering = false;
  }
  refresh_state();
}

void receiver_state_set_recovering(bool recovering) {
  s_state.recovering = recovering;
  if (recovering) {
    s_state.discoverable = false;
    s_state.session_establishing = false;
    s_state.streaming = false;
  }
  refresh_state();
}

void receiver_state_set_faulted(bool faulted) {
  s_state.faulted = faulted;
  refresh_state();
}

void receiver_state_set_ota_in_progress(bool in_progress) {
  s_state.ota_in_progress = in_progress;
  refresh_state();
}

receiver_state_t receiver_state_get(void) { return s_state.state; }

void receiver_state_get_snapshot(receiver_state_snapshot_t *snapshot) {
  if (!snapshot) {
    return;
  }
  *snapshot = s_state;
}

const char *receiver_state_to_str(receiver_state_t state) {
  switch (state) {
  case RECEIVER_STATE_BOOT:
    return "boot";
  case RECEIVER_STATE_SETUP_AP:
    return "setup_ap";
  case RECEIVER_STATE_NETWORK_READY:
    return "network_ready";
  case RECEIVER_STATE_DISCOVERABLE:
    return "discoverable";
  case RECEIVER_STATE_SESSION_ESTABLISHING:
    return "session_establishing";
  case RECEIVER_STATE_STREAMING:
    return "streaming";
  case RECEIVER_STATE_RECOVERING:
    return "recovering";
  case RECEIVER_STATE_FAULT:
    return "fault";
  default:
    return "unknown";
  }
}
