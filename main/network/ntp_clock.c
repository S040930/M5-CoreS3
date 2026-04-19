#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ntp_clock.h"

static const char *TAG = "ntp_clock";

#define NTP_EPOCH_OFFSET_SECONDS 2208988800ULL
#define BOOTSTRAP_MIN_MEASUREMENTS 4
#define MAX_TRACKING_DELTA_NS     250000000LL
#define MAX_ACCEPTED_SAMPLE_AGE_US 20000000LL
#define NTP_SANE_HOLDOVER_US     15000000LL
#define NTP_REJECT_STREAK_MAX    6

// Timing packet types (from shairport-sync)
#define TIMING_REQUEST  0xD2
#define TIMING_RESPONSE 0xD3

// Timing request packet structure (32 bytes)
typedef struct __attribute__((packed)) {
  uint8_t leader;    // 0x80
  uint8_t type;      // 0xD2 for request
  uint16_t seqno;    // sequence number (network byte order)
  uint32_t filler;   // padding
  uint64_t origin;   // our transmit time (will be echoed back)
  uint64_t receive;  // zeroed in request
  uint64_t transmit; // zeroed in request
} timing_packet_t;

// Number of measurements to keep for stability
#define TIMING_HISTORY_SIZE 8
#define TIMING_INTERVAL_MS  3000 // Send request every 3 seconds

#define NTP_STACK_SIZE 3072

static StaticTask_t s_ntp_tcb;
static StackType_t s_ntp_stack[NTP_STACK_SIZE / sizeof(StackType_t)];

// Timing state
static struct {
  bool running;
  TaskHandle_t task_handle;
  int socket;
  struct sockaddr_in remote_addr;

  // Clock offset tracking
  bool locked;
  bool tracking;
  bool sane_offset;
  int64_t offset_ns; // Current best offset
  int64_t last_good_offset_ns;
  int64_t measurements[TIMING_HISTORY_SIZE];
  int64_t dispersions[TIMING_HISTORY_SIZE];
  int measurement_count;
  int measurement_index;
  int accepted_count;
  int reject_streak;
  int64_t last_accept_us;
} ntp = {0};

static void accept_measurement(int64_t offset_ns, int64_t dispersion_ns) {
  int idx = ntp.measurement_index;
  ntp.measurements[idx] = offset_ns;
  ntp.dispersions[idx] = dispersion_ns;
  ntp.measurement_index = (idx + 1) % TIMING_HISTORY_SIZE;
  if (ntp.measurement_count < TIMING_HISTORY_SIZE) {
    ntp.measurement_count++;
  }

  int64_t best_offset = offset_ns;
  int64_t best_dispersion = dispersion_ns;
  for (int i = 0; i < ntp.measurement_count; i++) {
    if (ntp.dispersions[i] < best_dispersion) {
      best_dispersion = ntp.dispersions[i];
      best_offset = ntp.measurements[i];
    }
  }

  ntp.offset_ns = best_offset;
  ntp.last_good_offset_ns = best_offset;
  ntp.sane_offset = true;
  ntp.accepted_count++;
  ntp.reject_streak = 0;
  ntp.last_accept_us = esp_timer_get_time();

  if (!ntp.tracking && ntp.accepted_count >= BOOTSTRAP_MIN_MEASUREMENTS) {
    ntp.tracking = true;
    ntp.locked = true;
    ESP_LOGI(TAG,
             "NTP bootstrap complete: offset=%lld ms dispersion=%lld us "
             "samples=%d",
             (long long)(ntp.offset_ns / 1000000LL),
             (long long)(best_dispersion / 1000LL), ntp.accepted_count);
  }
}

// Convert NTP timestamp from packet to nanoseconds
static int64_t ntp_to_ns(uint32_t secs, uint32_t frac) {
  int64_t ns = (int64_t)secs * 1000000000LL;
  ns += ((int64_t)frac * 1000000000LL) >> 32;
  return ns;
}

// Convert local time (microseconds) to NTP timestamp format (for packet)
static uint64_t local_us_to_ntp(int64_t local_us) {
  local_us += (int64_t)NTP_EPOCH_OFFSET_SECONDS * 1000000LL;
  // NTP timestamp: upper 32 bits = seconds, lower 32 bits = fraction
  uint32_t secs = (uint32_t)(local_us / 1000000);
  uint64_t frac_us = local_us % 1000000;
  uint32_t frac = (uint32_t)((frac_us << 32) / 1000000);
  return ((uint64_t)secs << 32) | frac;
}

// Same NTP timescale as departure (origin) from local_us_to_ntp — required
// for offset/RTT; mixing esp_timer ns + raw epoch caused ~41146 s bogus offset.
static int64_t local_us_to_absolute_ntp_ns(int64_t local_us) {
  uint64_t ntp = local_us_to_ntp(local_us);
  uint32_t secs = (uint32_t)(ntp >> 32);
  uint32_t frac = (uint32_t)(ntp & 0xFFFFFFFF);
  return ntp_to_ns(secs, frac);
}

// Process timing response and calculate offset
static void process_timing_response(const uint8_t *packet, size_t len) {
  if (len < sizeof(timing_packet_t)) {
    return;
  }

  int64_t arrival_ns = local_us_to_absolute_ntp_ns(esp_timer_get_time());

  // Extract timestamps from response
  // Origin: our original transmit time (echoed back)
  // Receive: when remote received our request
  // Transmit: when remote sent response

  // Offsets in packet (after 8-byte header)
  // Bytes 8-15: origin timestamp (our transmit time, echoed)
  // Bytes 16-23: receive timestamp (remote's receive time)
  // Bytes 24-31: transmit timestamp (remote's transmit time)

  uint32_t origin_secs = ntohl(*(uint32_t *)(packet + 8));
  uint32_t origin_frac = ntohl(*(uint32_t *)(packet + 12));
  uint32_t receive_secs = ntohl(*(uint32_t *)(packet + 16));
  uint32_t receive_frac = ntohl(*(uint32_t *)(packet + 20));
  uint32_t transmit_secs = ntohl(*(uint32_t *)(packet + 24));
  uint32_t transmit_frac = ntohl(*(uint32_t *)(packet + 28));

  int64_t departure_ns = ntp_to_ns(origin_secs, origin_frac);
  int64_t remote_receive_ns = ntp_to_ns(receive_secs, receive_frac);
  int64_t remote_transmit_ns = ntp_to_ns(transmit_secs, transmit_frac);

  // Calculate offset using NTP formula:
  // Round-trip time = (arrival - departure) - (transmit - receive)
  // Offset = ((receive - departure) + (transmit - arrival)) / 2
  //        = remote_transmit + (return_time - remote_processing) / 2 - arrival

  int64_t round_trip_ns = arrival_ns - departure_ns;
  int64_t remote_processing_ns = remote_transmit_ns - remote_receive_ns;
  int64_t network_delay_ns = round_trip_ns - remote_processing_ns;

  // Offset = remote_transmit + network_delay/2 - arrival
  // This gives: remote_time = local_time + offset
  int64_t offset_ns = remote_transmit_ns + (network_delay_ns / 2) - arrival_ns;

  // Dispersion is the uncertainty (half round-trip time)
  int64_t dispersion_ns = network_delay_ns / 2;

  // Sanity check: reject if round-trip is negative or too large (>1 second)
  if (round_trip_ns < 0 || round_trip_ns > 1000000000LL) {
    ESP_LOGW(TAG, "Rejecting measurement: RTT=%lld ms",
             (long long)(round_trip_ns / 1000000));
    return;
  }

  bool reject = false;
  const char *reject_reason = NULL;
  int64_t delta_ns = 0;
  if (ntp.tracking) {
    delta_ns = llabs(offset_ns - ntp.last_good_offset_ns);
    if (delta_ns > MAX_TRACKING_DELTA_NS) {
      reject = true;
      reject_reason = "delta-too-large";
    }
  }

  if (reject) {
    int64_t now_us = esp_timer_get_time();
    ntp.reject_streak++;
    bool holdover_active =
        ntp.last_accept_us > 0 &&
        (now_us - ntp.last_accept_us) <= NTP_SANE_HOLDOVER_US &&
        ntp.reject_streak <= NTP_REJECT_STREAK_MAX;
    if (holdover_active) {
      ntp.sane_offset = true;
      ESP_LOGW(TAG,
               "Rejecting timing sample: reason=%s delta=%lld ms "
               "(holdover active, streak=%d)",
               reject_reason ? reject_reason : "unknown",
               (long long)(delta_ns / 1000000LL), ntp.reject_streak);
    } else {
      ntp.sane_offset = false;
      if (now_us - ntp.last_accept_us > MAX_ACCEPTED_SAMPLE_AGE_US) {
        ntp.locked = false;
      }
      ESP_LOGW(TAG,
               "Rejecting timing sample: reason=%s delta=%lld ms "
               "(marking unsane, streak=%d)",
               reject_reason ? reject_reason : "unknown",
               (long long)(delta_ns / 1000000LL), ntp.reject_streak);
    }
    return;
  }

  accept_measurement(offset_ns, dispersion_ns);

  ESP_LOGD(TAG,
           "Timing: RTT=%lld us offset=%lld ms tracking=%d accepted=%d",
           (long long)(round_trip_ns / 1000),
           (long long)(offset_ns / 1000000), ntp.tracking ? 1 : 0,
           ntp.accepted_count);
}

// Send timing request
static void send_timing_request(void) {
  timing_packet_t req;
  memset(&req, 0, sizeof(req));

  req.leader = 0x80;
  req.type = TIMING_REQUEST;
  req.seqno = htons(7); // Fixed sequence number (like shairport-sync)

  // Set transmit field to our current time — the AirPlay source echoes
  // request bytes 24-31 (transmit) into response bytes 8-15 (origin).
  int64_t now_us = esp_timer_get_time();
  uint64_t ntp_time = local_us_to_ntp(now_us);

  // Split into network byte order (avoid unaligned access)
  uint8_t *tx_bytes = (uint8_t *)&req.transmit;
  uint32_t secs = htonl((uint32_t)(ntp_time >> 32));
  uint32_t frac = htonl((uint32_t)(ntp_time & 0xFFFFFFFF));
  memcpy(tx_bytes, &secs, 4);
  memcpy(tx_bytes + 4, &frac, 4);

  sendto(ntp.socket, &req, sizeof(req), 0, (struct sockaddr *)&ntp.remote_addr,
         sizeof(ntp.remote_addr));
}

// Timing task: sends requests and processes responses
static void ntp_task(void *pvParameters) {
  uint8_t packet[64];
  struct sockaddr_in src_addr;
  socklen_t addr_len;

  // Initial delay before first request
  vTaskDelay(pdMS_TO_TICKS(300));

  TickType_t last_request = 0;

  while (ntp.running) {
    // Send timing request periodically
    TickType_t now = xTaskGetTickCount();
    if (now - last_request >= pdMS_TO_TICKS(TIMING_INTERVAL_MS)) {
      send_timing_request();
      last_request = now;
    }

    // Check for response (with short timeout)
    addr_len = sizeof(src_addr);
    int len = recvfrom(ntp.socket, packet, sizeof(packet), 0,
                       (struct sockaddr *)&src_addr, &addr_len);

    if (len < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      if (ntp.running) {
        ESP_LOGE(TAG, "recvfrom error: %d", errno);
      }
      break;
    }

    if (len >= 2) {
      uint8_t pkt_type = packet[1];

      if (pkt_type == TIMING_RESPONSE) {
        process_timing_response(packet, len);
      }
    }
  }

  ntp.task_handle = NULL;
  vTaskDelete(NULL);
}

esp_err_t ntp_clock_start_client(uint32_t remote_ip, uint16_t remote_port) {
  if (ntp.running) {
    // If already running to same target, keep going
    if (ntp.remote_addr.sin_addr.s_addr == remote_ip &&
        ntohs(ntp.remote_addr.sin_port) == remote_port) {
      return ESP_OK;
    }
    // Stop existing and restart with new target
    ntp_clock_stop();
  }

  ntp.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (ntp.socket < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %d", errno);
    return ESP_FAIL;
  }

  // Set receive timeout (100ms for responsive checking)
  struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
  setsockopt(ntp.socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Setup remote address
  memset(&ntp.remote_addr, 0, sizeof(ntp.remote_addr));
  ntp.remote_addr.sin_family = AF_INET;
  ntp.remote_addr.sin_addr.s_addr = remote_ip;
  ntp.remote_addr.sin_port = htons(remote_port);

  // Reset state
  ntp.locked = false;
  ntp.tracking = false;
  ntp.sane_offset = false;
  ntp.offset_ns = 0;
  ntp.last_good_offset_ns = 0;
  ntp.measurement_count = 0;
  ntp.measurement_index = 0;
  ntp.accepted_count = 0;
  ntp.reject_streak = 0;
  ntp.last_accept_us = 0;
  memset(ntp.measurements, 0, sizeof(ntp.measurements));
  memset(ntp.dispersions, 0, sizeof(ntp.dispersions));
  ntp.running = true;

  ntp.task_handle = xTaskCreateStatic(ntp_task, "ntp_clock",
                                      NTP_STACK_SIZE / sizeof(StackType_t),
                                      NULL, 5, s_ntp_stack, &s_ntp_tcb);
  if (ntp.task_handle == NULL) {
    ESP_LOGE(TAG, "Failed to create NTP task");
    close(ntp.socket);
    ntp.socket = -1;
    ntp.running = false;
    return ESP_FAIL;
  }

  uint8_t *ip = (uint8_t *)&remote_ip;
  ESP_LOGI(TAG, "NTP timing client started -> %d.%d.%d.%d:%d", ip[0], ip[1],
           ip[2], ip[3], remote_port);
  return ESP_OK;
}

void ntp_clock_stop(void) {
  if (!ntp.running) {
    return;
  }

  ntp.running = false;

  if (ntp.socket >= 0) {
    close(ntp.socket);
    ntp.socket = -1;
  }

  if (ntp.task_handle) {
    // Wait for task to finish
    for (int i = 0; i < 20 && ntp.task_handle != NULL; i++) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }

  ntp.locked = false;
  ntp.tracking = false;
  ntp.sane_offset = false;
  ntp.measurement_count = 0;
  ntp.accepted_count = 0;
  ntp.reject_streak = 0;
  ntp.last_accept_us = 0;
  ESP_LOGI(TAG, "NTP timing stopped");
}

bool ntp_clock_is_locked(void) {
  return ntp.locked;
}

bool ntp_clock_has_sane_offset(void) {
  return ntp.sane_offset;
}

bool ntp_clock_is_tracking(void) {
  return ntp.tracking && ntp.sane_offset;
}

int64_t ntp_clock_get_offset_ns(void) {
  return ntp.sane_offset ? ntp.offset_ns : ntp.last_good_offset_ns;
}

int ntp_clock_get_reject_streak(void) {
  return ntp.reject_streak;
}

int64_t ntp_clock_get_last_accept_age_ms(void) {
  if (ntp.last_accept_us <= 0) {
    return -1;
  }
  int64_t age_us = esp_timer_get_time() - ntp.last_accept_us;
  if (age_us < 0) {
    age_us = 0;
  }
  return age_us / 1000LL;
}
