#include "voice_request.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "voice_dsp.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "voice_request"
#define VOICE_REQUEST_MODEL "qwen3.5-omni-flash"
#define VOICE_REQUEST_TIMEOUT_MS 20000
#define VOICE_REQUEST_MAX_RESPONSE (32 * 1024)

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} http_accum_t;

static uint64_t req_now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000ULL); }

static void put_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint8_t *wav_from_pcm(const int16_t *pcm, size_t frames, uint32_t sample_rate,
                             size_t *out_len) {
  if (pcm == NULL || frames == 0 || out_len == NULL) return NULL;
  size_t pcm_bytes = frames * sizeof(int16_t);
  size_t wav_len = 44 + pcm_bytes;
  uint8_t *wav = (uint8_t *)voice_buf_alloc(wav_len);
  if (wav == NULL) return NULL;
  memcpy(wav, "RIFF", 4);
  put_le32(wav + 4, (uint32_t)(wav_len - 8));
  memcpy(wav + 8, "WAVEfmt ", 8);
  put_le32(wav + 16, 16);
  put_le16(wav + 20, 1);
  put_le16(wav + 22, 1);
  put_le32(wav + 24, sample_rate);
  put_le32(wav + 28, sample_rate * 2);
  put_le16(wav + 32, 2);
  put_le16(wav + 34, 16);
  memcpy(wav + 36, "data", 4);
  put_le32(wav + 40, (uint32_t)pcm_bytes);
  memcpy(wav + 44, pcm, pcm_bytes);
  *out_len = wav_len;
  return wav;
}

static char *base64_encode_bytes(const uint8_t *input, size_t in_len) {
  size_t needed = 0;
  int rc = mbedtls_base64_encode(NULL, 0, &needed, input, in_len);
  if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || needed == 0) return NULL;
  unsigned char *buf = (unsigned char *)voice_buf_alloc(needed + 1);
  if (buf == NULL) return NULL;
  rc = mbedtls_base64_encode(buf, needed + 1, &needed, input, in_len);
  if (rc != 0) {
    voice_buf_free(buf);
    return NULL;
  }
  buf[needed] = '\0';
  return (char *)buf;
}

static void endpoint_from_url(const char *url, char *dst, size_t cap) {
  const char *base = "https://dashscope.aliyuncs.com/compatible-mode/v1";
  if (url != NULL && strstr(url, "dashscope-intl") != NULL) {
    base = "https://dashscope-intl.aliyuncs.com/compatible-mode/v1";
  } else if (url != NULL && strncmp(url, "http", 4) == 0 && strstr(url, "compatible-mode") != NULL) {
    base = url;
  }
  if (strstr(base, "/chat/completions") != NULL) {
    snprintf(dst, cap, "%s", base);
  } else {
    snprintf(dst, cap, "%s/chat/completions", base);
  }
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  if (evt == NULL || evt->user_data == NULL) return ESP_OK;
  http_accum_t *acc = (http_accum_t *)evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data != NULL && evt->data_len > 0) {
    if (acc->len + (size_t)evt->data_len + 1 > acc->cap) {
      ESP_LOGW(TAG, "response too large; truncating");
      return ESP_OK;
    }
    memcpy(acc->data + acc->len, evt->data, (size_t)evt->data_len);
    acc->len += (size_t)evt->data_len;
    acc->data[acc->len] = '\0';
  }
  return ESP_OK;
}

static void extract_text_from_json(const char *json, char *dst, size_t cap) {
  if (dst == NULL || cap == 0) return;
  dst[0] = '\0';
  cJSON *root = cJSON_Parse(json);
  if (root == NULL) return;
  const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
  const cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
  const cJSON *message = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : NULL;
  const cJSON *content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
  if (cJSON_IsString(content) && content->valuestring != NULL) {
    snprintf(dst, cap, "%s", content->valuestring);
  } else if (cJSON_IsArray(content)) {
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, content) {
      const cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
      if (cJSON_IsString(text) && text->valuestring != NULL) {
        snprintf(dst, cap, "%s", text->valuestring);
        break;
      }
    }
  }
  cJSON_Delete(root);
}

static char *build_request_body(const char *audio_b64) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "model", VOICE_REQUEST_MODEL);
  cJSON *modalities = cJSON_AddArrayToObject(root, "modalities");
  cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
  cJSON_AddBoolToObject(root, "stream", false);
  cJSON *messages = cJSON_AddArrayToObject(root, "messages");
  cJSON *sys = cJSON_CreateObject();
  cJSON_AddStringToObject(sys, "role", "system");
  cJSON_AddStringToObject(sys, "content",
                          "You are a concise voice assistant on an ESP32 device. Reply in the "
                          "user's language with one or two short sentences.");
  cJSON_AddItemToArray(messages, sys);
  cJSON *user = cJSON_CreateObject();
  cJSON_AddStringToObject(user, "role", "user");
  cJSON *content = cJSON_AddArrayToObject(user, "content");
  cJSON *audio = cJSON_CreateObject();
  cJSON_AddStringToObject(audio, "type", "input_audio");
  cJSON *input_audio = cJSON_AddObjectToObject(audio, "input_audio");
  cJSON_AddStringToObject(input_audio, "data", audio_b64);
  cJSON_AddStringToObject(input_audio, "format", "wav");
  cJSON_AddItemToArray(content, audio);
  cJSON *text = cJSON_CreateObject();
  cJSON_AddStringToObject(text, "type", "text");
  cJSON_AddStringToObject(text, "text", "Please answer the user's spoken question briefly.");
  cJSON_AddItemToArray(content, text);
  cJSON_AddItemToArray(messages, user);
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return body;
}

esp_err_t voice_request_send_audio(const voice_request_config_t *cfg, const int16_t *pcm,
                                   size_t frames, uint32_t sample_rate,
                                   voice_request_result_t *out) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (cfg == NULL || cfg->api_key == NULL || cfg->api_key[0] == '\0' ||
      pcm == NULL || frames == 0 || out == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  uint64_t started = req_now_ms();
  size_t wav_len = 0;
  uint8_t *wav = wav_from_pcm(pcm, frames, sample_rate, &wav_len);
  if (wav == NULL) return ESP_ERR_NO_MEM;
  char *b64 = base64_encode_bytes(wav, wav_len);
  voice_buf_free(wav);
  if (b64 == NULL) return ESP_ERR_NO_MEM;
  char *body = build_request_body(b64);
  voice_buf_free(b64);
  if (body == NULL) return ESP_ERR_NO_MEM;

  char endpoint[320];
  endpoint_from_url(cfg->url, endpoint, sizeof(endpoint));
  char *response = (char *)voice_buf_alloc(VOICE_REQUEST_MAX_RESPONSE);
  if (response == NULL) {
    free(body);
    return ESP_ERR_NO_MEM;
  }
  response[0] = '\0';
  http_accum_t acc = {.data = response, .len = 0, .cap = VOICE_REQUEST_MAX_RESPONSE};
  esp_http_client_config_t http_cfg = {
      .url = endpoint,
      .method = HTTP_METHOD_POST,
      .timeout_ms = VOICE_REQUEST_TIMEOUT_MS,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .event_handler = http_event_handler,
      .user_data = &acc,
      .buffer_size = 2048,
      .buffer_size_tx = 2048,
  };
  esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
  if (client == NULL) {
    voice_buf_free(response);
    free(body);
    return ESP_FAIL;
  }
  char auth[300];
  snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body, (int)strlen(body));
  ESP_LOGI(TAG, "upload_start frames=%lu wav=%lu endpoint=%s model=%s",
           (unsigned long)frames, (unsigned long)wav_len, endpoint, VOICE_REQUEST_MODEL);
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  out->status_code = status;
  out->latency_ms = req_now_ms() - started;
  out->err = err;
  if (err == ESP_OK && status >= 200 && status < 300) {
    extract_text_from_json(response, out->text, sizeof(out->text));
    if (out->text[0] == '\0') {
      snprintf(out->text, sizeof(out->text), "%s", "[empty response]");
    }
    ESP_LOGI(TAG, "upload_done status=%d latency=%llums text=\"%s\"", status,
             (unsigned long long)out->latency_ms, out->text);
  } else {
    ESP_LOGW(TAG, "upload_failed err=%s status=%d latency=%llums body=%s",
             esp_err_to_name(err), status, (unsigned long long)out->latency_ms,
             response[0] != '\0' ? response : "<empty>");
  }
  esp_http_client_cleanup(client);
  voice_buf_free(response);
  free(body);
  return (err == ESP_OK && status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}
