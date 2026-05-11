
#include "omni_client.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "env_monitor.h"
#include "sdkconfig.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef CONFIG_VOICE_MODEL
#define CONFIG_VOICE_MODEL "qwen3.5-omni-flash"
#endif

#ifndef CONFIG_VOICE_OMNI_VOICE
#define CONFIG_VOICE_OMNI_VOICE "Tina"
#endif

#define TAG "omni_client"

#define OMNI_REQUEST_TIMEOUT_MS 60000
#define SSE_LINE_BUF_CAP 4096
#define SSE_LINE_BUF_MAX 262144
#define AUDIO_DECODE_BUF_CAP 8192
#define WAV_HEADER_SIZE 44
#define OSS_BOUNDARY "----ESP32OssBoundary7x9z"

static omni_client_config_t s_config = {0};
static omni_client_callbacks_t s_callbacks = {0};
static bool s_initialized = false;

// Persistent HTTP client for getPolicy requests (no event handler, safe to reuse)
static esp_http_client_handle_t s_dashscope_client = NULL;

typedef struct {
    char *sse_line_buf;
    size_t sse_line_len;
    size_t sse_line_buf_cap;
    uint8_t *audio_decode_buf;
    size_t audio_decode_buf_cap;
    bool has_error;
    bool response_done;
    bool wav_header_stripped;
    int sse_data_count;
    int audio_chunk_count;
    char error_msg[256];
    char http_error_body[512];
    size_t http_error_len;
    int http_status;
} sse_ctx_t;

static void make_wav_header(uint8_t *dst, size_t pcm_frames, uint32_t sample_rate) {
    size_t data_size = pcm_frames * sizeof(int16_t);
    size_t file_size = 36 + data_size;

    memcpy(dst, "RIFF", 4);
    dst[4] = (uint8_t)(file_size);
    dst[5] = (uint8_t)(file_size >> 8);
    dst[6] = (uint8_t)(file_size >> 16);
    dst[7] = (uint8_t)(file_size >> 24);
    memcpy(dst + 8, "WAVE", 4);
    memcpy(dst + 12, "fmt ", 4);
    dst[16] = 16; dst[17] = 0; dst[18] = 0; dst[19] = 0;
    dst[20] = 1; dst[21] = 0;
    dst[22] = 1; dst[23] = 0;
    dst[24] = (uint8_t)(sample_rate);
    dst[25] = (uint8_t)(sample_rate >> 8);
    dst[26] = (uint8_t)(sample_rate >> 16);
    dst[27] = (uint8_t)(sample_rate >> 24);
    uint32_t byte_rate = sample_rate * 2;
    dst[28] = (uint8_t)(byte_rate);
    dst[29] = (uint8_t)(byte_rate >> 8);
    dst[30] = (uint8_t)(byte_rate >> 16);
    dst[31] = (uint8_t)(byte_rate >> 24);
    dst[32] = 2; dst[33] = 0;
    dst[34] = 16; dst[35] = 0;
    memcpy(dst + 36, "data", 4);
    dst[40] = (uint8_t)(data_size);
    dst[41] = (uint8_t)(data_size >> 8);
    dst[42] = (uint8_t)(data_size >> 16);
    dst[43] = (uint8_t)(data_size >> 24);
}

static void process_sse_line(const char *line, size_t line_len, sse_ctx_t *ctx) {
    if (line_len == 0) return;

    if (strncmp(line, "data: ", 6) != 0) return;

    ctx->sse_data_count++;

    const char *json_str = line + 6;
    size_t json_len = line_len - 6;

    if (json_len == 0) return;

    if (strncmp(json_str, "[DONE]", 6) == 0) {
        ctx->response_done = true;
        return;
    }

    cJSON *root = cJSON_ParseWithLength(json_str, json_len);
    if (root == NULL) return;

    cJSON *error_obj = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(error_obj)) {
        cJSON *message = cJSON_GetObjectItemCaseSensitive(error_obj, "message");
        if (cJSON_IsString(message) && message->valuestring) {
            strncpy(ctx->error_msg, message->valuestring, sizeof(ctx->error_msg) - 1);
        }
        ctx->has_error = true;
        cJSON_Delete(root);
        return;
    }

    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *first = cJSON_GetArrayItem(choices, 0);
    if (first == NULL) {
        cJSON_Delete(root);
        return;
    }

    cJSON *finish_reason = cJSON_GetObjectItemCaseSensitive(first, "finish_reason");
    if (cJSON_IsString(finish_reason) && strcmp(finish_reason->valuestring, "stop") == 0) {
        ctx->response_done = true;
    }

    cJSON *delta = cJSON_GetObjectItemCaseSensitive(first, "delta");
    if (cJSON_IsObject(delta)) {
        cJSON *content = cJSON_GetObjectItemCaseSensitive(delta, "content");
        if (s_callbacks.on_text_delta && content != NULL) {
            if (cJSON_IsString(content) && content->valuestring && content->valuestring[0] != '\0') {
                s_callbacks.on_text_delta(content->valuestring, strlen(content->valuestring),
                                          s_callbacks.user_data);
            } else if (cJSON_IsArray(content)) {
                cJSON *part = NULL;
                cJSON_ArrayForEach(part, content) {
                    if (!cJSON_IsObject(part)) {
                        continue;
                    }
                    cJSON *typ = cJSON_GetObjectItemCaseSensitive(part, "type");
                    cJSON *txt = cJSON_GetObjectItemCaseSensitive(part, "text");
                    if (cJSON_IsString(typ) && typ->valuestring && strcmp(typ->valuestring, "text") == 0 &&
                        cJSON_IsString(txt) && txt->valuestring && txt->valuestring[0] != '\0') {
                        s_callbacks.on_text_delta(txt->valuestring, strlen(txt->valuestring),
                                                  s_callbacks.user_data);
                    }
                }
            }
        }

        cJSON *audio = cJSON_GetObjectItemCaseSensitive(delta, "audio");
        if (cJSON_IsObject(audio)) {
            cJSON *audio_data = cJSON_GetObjectItemCaseSensitive(audio, "data");
            if (cJSON_IsString(audio_data) && audio_data->valuestring) {
                const char *b64 = audio_data->valuestring;
                size_t b64_len = strlen(b64);
                ESP_LOGI(TAG, "SSE audio delta: b64_len=%d", (int)b64_len);
                if (b64_len > 0) {
                    size_t needed = 0;
                    mbedtls_base64_decode(NULL, 0, &needed, (const unsigned char *)b64, b64_len);
                    if (needed > 0) {
                        if (needed > ctx->audio_decode_buf_cap) {
                            uint8_t *new_buf = heap_caps_realloc(ctx->audio_decode_buf, needed, MALLOC_CAP_SPIRAM);
                            if (new_buf == NULL) {
                                ESP_LOGE(TAG, "audio decode buf realloc failed (%d)", needed);
                            } else {
                                ctx->audio_decode_buf = new_buf;
                                ctx->audio_decode_buf_cap = needed;
                            }
                        }
                        if (ctx->audio_decode_buf && needed <= ctx->audio_decode_buf_cap) {
                            size_t decoded = 0;
                            int ret = mbedtls_base64_decode(ctx->audio_decode_buf, ctx->audio_decode_buf_cap,
                                                            &decoded, (const unsigned char *)b64, b64_len);
                            if (ret == 0 && decoded > 0) {
                                const uint8_t *pcm_bytes = ctx->audio_decode_buf;
                                size_t pcm_byte_len = decoded;

                                if (!ctx->wav_header_stripped && decoded >= 4 &&
                                    memcmp(pcm_bytes, "RIFF", 4) == 0) {
                                    if (decoded > WAV_HEADER_SIZE) {
                                        pcm_bytes += WAV_HEADER_SIZE;
                                        pcm_byte_len -= WAV_HEADER_SIZE;
                                    }
                                    ctx->wav_header_stripped = true;
                                }

                                const int16_t *pcm = (const int16_t *)pcm_bytes;
                                size_t frames = pcm_byte_len / sizeof(int16_t);
                                if (frames > 0 && s_callbacks.on_audio_delta) {
                                    ctx->audio_chunk_count++;
                                    if (ctx->audio_chunk_count == 1) {
                                        ESP_LOGI(TAG, "first audio chunk: %d frames, %d Hz", (int)frames, 24000);
                                    }
                                    s_callbacks.on_audio_delta(pcm, frames, 24000, s_callbacks.user_data);
                                }
                            } else if (ret != 0) {
                                ESP_LOGE(TAG, "base64 decode failed: %d", ret);
                            }
                        }
                    }
                }
            }
        }
    }

    cJSON_Delete(root);
}

static void sse_ctx_init(sse_ctx_t *ctx) {
    ctx->sse_line_buf = (char *)heap_caps_malloc(SSE_LINE_BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx->sse_line_buf == NULL) {
        ctx->sse_line_buf = (char *)malloc(SSE_LINE_BUF_CAP);
    }
    ctx->sse_line_buf_cap = SSE_LINE_BUF_CAP;
    ctx->sse_line_len = 0;
    ctx->audio_decode_buf = (uint8_t *)heap_caps_malloc(AUDIO_DECODE_BUF_CAP, MALLOC_CAP_SPIRAM);
    ctx->audio_decode_buf_cap = AUDIO_DECODE_BUF_CAP;
    ctx->has_error = false;
    ctx->response_done = false;
    ctx->wav_header_stripped = false;
    ctx->sse_data_count = 0;
    ctx->audio_chunk_count = 0;
    ctx->error_msg[0] = '\0';
    ctx->http_error_body[0] = '\0';
    ctx->http_error_len = 0;
    ctx->http_status = 0;
}

static void sse_ctx_cleanup(sse_ctx_t *ctx) {
    if (ctx->sse_line_buf) {
        heap_caps_free(ctx->sse_line_buf);
        ctx->sse_line_buf = NULL;
    }
    if (ctx->audio_decode_buf) {
        heap_caps_free(ctx->audio_decode_buf);
        ctx->audio_decode_buf = NULL;
    }
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt == NULL || evt->user_data == NULL) return ESP_OK;

    sse_ctx_t *ctx = (sse_ctx_t *)evt->user_data;

    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len == 0) {
        return ESP_OK;
    }

    int status = esp_http_client_get_status_code(evt->client);
    if (status > 0) {
        ctx->http_status = status;
    }

    if (ctx->http_status >= 400) {
        const char *data = (const char *)evt->data;
        size_t remaining = evt->data_len;
        if (ctx->http_error_len < sizeof(ctx->http_error_body) - 1) {
            size_t copy_len = remaining;
            if (copy_len > sizeof(ctx->http_error_body) - 1 - ctx->http_error_len) {
                copy_len = sizeof(ctx->http_error_body) - 1 - ctx->http_error_len;
            }
            memcpy(ctx->http_error_body + ctx->http_error_len, data, copy_len);
            ctx->http_error_len += copy_len;
            ctx->http_error_body[ctx->http_error_len] = '\0';
        }
        return ESP_OK;
    }

    const char *data = (const char *)evt->data;
    size_t remaining = evt->data_len;

    size_t pos = 0;
    while (pos < remaining) {
        const char *nl = memchr(data + pos, '\n', remaining - pos);
        if (nl == NULL) {
            size_t avail = remaining - pos;
            size_t needed = ctx->sse_line_len + avail + 1;
            if (needed > ctx->sse_line_buf_cap) {
                size_t new_cap = ctx->sse_line_buf_cap * 2;
                if (new_cap < needed) new_cap = needed;
                if (new_cap > SSE_LINE_BUF_MAX) {
                    ESP_LOGW(TAG, "SSE line too long (%d > %d), discarding", (int)needed, SSE_LINE_BUF_MAX);
                    ctx->sse_line_len = 0;
                    pos = remaining;
                    continue;
                }
                char *new_buf = heap_caps_realloc(ctx->sse_line_buf, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (new_buf == NULL) {
                    ESP_LOGW(TAG, "SSE line realloc failed (%d), discarding", (int)new_cap);
                    new_buf = realloc(ctx->sse_line_buf, new_cap);
                }
                if (new_buf == NULL) {
                    ctx->sse_line_len = 0;
                    pos = remaining;
                    continue;
                }
                ctx->sse_line_buf = new_buf;
                ctx->sse_line_buf_cap = new_cap;
            }
            memcpy(ctx->sse_line_buf + ctx->sse_line_len, data + pos, avail);
            ctx->sse_line_len += avail;
            break;
        }

        size_t line_len = (nl - (data + pos));
        size_t needed = ctx->sse_line_len + line_len + 1;
        if (needed > ctx->sse_line_buf_cap) {
            size_t new_cap = ctx->sse_line_buf_cap * 2;
            if (new_cap < needed) new_cap = needed;
            if (new_cap > SSE_LINE_BUF_MAX) {
                ESP_LOGW(TAG, "SSE line too long (%d > %d), discarding", (int)needed, SSE_LINE_BUF_MAX);
                ctx->sse_line_len = 0;
                pos = (nl - data) + 1;
                continue;
            }
            char *new_buf = heap_caps_realloc(ctx->sse_line_buf, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (new_buf == NULL) {
                new_buf = realloc(ctx->sse_line_buf, new_cap);
            }
            if (new_buf == NULL) {
                ESP_LOGW(TAG, "SSE line realloc failed (%d), discarding", (int)new_cap);
                ctx->sse_line_len = 0;
                pos = (nl - data) + 1;
                continue;
            }
            ctx->sse_line_buf = new_buf;
            ctx->sse_line_buf_cap = new_cap;
        }

        if (ctx->sse_line_len > 0) {
            memcpy(ctx->sse_line_buf + ctx->sse_line_len, data + pos, line_len);
            ctx->sse_line_buf[ctx->sse_line_len + line_len] = '\0';
            process_sse_line(ctx->sse_line_buf, ctx->sse_line_len + line_len, ctx);
            ctx->sse_line_len = 0;
        } else {
            memcpy(ctx->sse_line_buf, data + pos, line_len);
            ctx->sse_line_buf[line_len] = '\0';
            process_sse_line(ctx->sse_line_buf, line_len, ctx);
        }

        pos = (nl - data) + 1;
    }

    return ESP_OK;
}

static void sanitize_model_name(char *model) {
    if (!model || model[0] == '\0') return;
    char *p = model;
    while ((p = strchr(p, '-')) != NULL) {
        if (strncmp(p, "-20", 3) == 0 && strlen(p) >= 10) {
            *p = '\0';
            break;
        }
        p++;
    }
}

static const char *build_instructions_with_env(const char *base, char *buf, size_t buf_len) {
    if (buf == NULL || buf_len == 0) {
        return base;
    }

    if (base == NULL) {
        base = "";
    }

    float temp_c = 0.0f;
    float humidity_pct = 0.0f;
    float pressure_kpa = 0.0f;
    bool env_ready = env_monitor_get_latest(&temp_c, &humidity_pct, &pressure_kpa);

    int written;
    if (env_ready) {
        written = snprintf(buf, buf_len,
                           "%s\n\n当前设备环境读数：温度%.1f°C，湿度%.1f%%，气压%.1fkPa。"
                           "请在用户询问温度、湿度或气压时，直接基于这组读数作答，"
                           "不要泛化为默认回答。"
                           "\n\n音量调节说明："
                           "当用户说\"调大音量\"、\"大声一点\"、\"音量调高\"或类似意图时，"
                           "请直接回复\"好的，已为您调大音量\"（不要说\"我不支持\"之类的）。"
                           "当用户说\"调小音量\"、\"小声一点\"、\"音量调低\"或类似意图时，"
                           "请直接回复\"好的，已为您调小音量\"。"
                           "当用户要求\"静音\"或\"关闭声音\"时请回复\"好的，已静音\"。"
                           "每次调节幅度为3分贝，每次只调节一个单位。",
                           base, temp_c, humidity_pct, pressure_kpa);
    } else {
        written = snprintf(buf, buf_len,
                           "%s\n\n当前设备没有可用的实时环境读数。"
                           "如果用户询问温度、湿度或气压，请明确说明当前没有可用的实时读数。"
                           "\n\n音量调节说明："
                           "当用户说\"调大音量\"、\"大声一点\"、\"音量调高\"或类似意图时，"
                           "请直接回复\"好的，已为您调大音量\"。"
                           "当用户说\"调小音量\"、\"小声一点\"、\"音量调低\"或类似意图时，"
                           "请直接回复\"好的，已为您调小音量\"。"
                           "当用户要求\"静音\"或\"关闭声音\"时请回复\"好的，已静音\"。"
                           "每次调节幅度为3分贝，每次只调节一个单位。",
                           base);
    }
    if (written < 0 || (size_t)written >= buf_len) {
        buf[buf_len - 1] = '\0';
        return buf;
    }
    return buf;
}

static char *build_request_body(const char *audio_url, const char *model, const char *voice, const char *instructions) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;

    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON *stream_opts = cJSON_AddObjectToObject(root, "stream_options");
    cJSON_AddBoolToObject(stream_opts, "include_usage", true);

    cJSON *modalities = cJSON_AddArrayToObject(root, "modalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));

    cJSON *audio_obj = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddStringToObject(audio_obj, "voice", voice);
    cJSON_AddStringToObject(audio_obj, "format", "wav");

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");

    if (instructions && instructions[0] != '\0') {
        cJSON *sys_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_msg, "role", "system");
        cJSON_AddStringToObject(sys_msg, "content", instructions);
        cJSON_AddItemToArray(messages, sys_msg);
    }

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON *content = cJSON_AddArrayToObject(user_msg, "content");

    cJSON *audio_part = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_part, "type", "input_audio");
    cJSON *input_audio = cJSON_AddObjectToObject(audio_part, "input_audio");

    /* DashScope OpenAI-compatible Omni expects URL/base64 in "data", not "audio_url"
     * (wrong field triggers invalid_request_error about image_url.url vs urls). */
    cJSON_AddStringToObject(input_audio, "data", audio_url);
    cJSON_AddStringToObject(input_audio, "format", "wav");

    cJSON_AddItemToArray(content, audio_part);

    cJSON_AddItemToArray(messages, user_msg);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static size_t multipart_form_field(char *buf, size_t buf_cap,
                                    const char *field_name, const char *field_value) {
    int n = snprintf(buf, buf_cap,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"%s\"\r\n"
        "\r\n"
        "%s\r\n",
        OSS_BOUNDARY, field_name, field_value);
    return (n > 0 && (size_t)n < buf_cap) ? (size_t)n : 0;
}

static char *upload_audio_to_oss(const uint8_t *wav_data, size_t wav_len,
                                  const char *model, const char *api_key) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://dashscope.aliyuncs.com/api/v1/uploads?action=getPolicy&model=%s", model);

    char auth[300];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key);

    ESP_LOGI(TAG, "OSS: getting upload policy for model=%s", model);

    // Use persistent DashScope client if available
    esp_http_client_handle_t get_client = s_dashscope_client;
    bool using_persistent = (get_client != NULL);
    
    if (!using_persistent) {
        esp_http_client_config_t get_cfg = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = 15000,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 4096,
            .keep_alive_enable = true,
        };
        get_client = esp_http_client_init(&get_cfg);
        if (get_client == NULL) {
            ESP_LOGE(TAG, "OSS: failed to init GET client");
            return NULL;
        }
    } else {
        esp_http_client_set_url(get_client, url);
        esp_http_client_set_method(get_client, HTTP_METHOD_GET);
    }

    esp_http_client_set_header(get_client, "Authorization", auth);
    esp_http_client_set_header(get_client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(get_client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OSS: open failed: %s", esp_err_to_name(err));
        if (!using_persistent) {
            esp_http_client_cleanup(get_client);
        }
        return NULL;
    }

    int content_len = esp_http_client_fetch_headers(get_client);
    int status = esp_http_client_get_status_code(get_client);

    ESP_LOGI(TAG, "OSS: getPolicy status=%d content_len=%d", status, content_len);

    if (status != 200 || content_len <= 0) {
        ESP_LOGE(TAG, "OSS: getPolicy failed");
        esp_http_client_close(get_client);
        if (!using_persistent) {
            esp_http_client_cleanup(get_client);
        }
        return NULL;
    }

    char *policy_json = (char *)heap_caps_malloc(content_len + 1, MALLOC_CAP_SPIRAM);
    if (policy_json == NULL) {
        policy_json = (char *)malloc(content_len + 1);
    }
    if (policy_json == NULL) {
        ESP_LOGE(TAG, "OSS: alloc policy json failed");
        esp_http_client_close(get_client);
        if (!using_persistent) {
            esp_http_client_cleanup(get_client);
        }
        return NULL;
    }

    int read_len = esp_http_client_read(get_client, policy_json, content_len);
    esp_http_client_close(get_client);
    if (!using_persistent) {
        esp_http_client_cleanup(get_client);
    }

    if (read_len <= 0) {
        ESP_LOGE(TAG, "OSS: read policy failed");
        free(policy_json);
        return NULL;
    }
    policy_json[read_len] = '\0';

    ESP_LOGI(TAG, "OSS: policy response: %.300s", policy_json);

    cJSON *root = cJSON_Parse(policy_json);
    free(policy_json);
    if (root == NULL) {
        ESP_LOGE(TAG, "OSS: parse policy JSON failed");
        return NULL;
    }

    cJSON *data_obj = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data_obj)) {
        ESP_LOGE(TAG, "OSS: no data field in policy response");
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *upload_host = cJSON_GetObjectItemCaseSensitive(data_obj, "upload_host");
    cJSON *upload_dir = cJSON_GetObjectItemCaseSensitive(data_obj, "upload_dir");
    cJSON *oss_access_key_id = cJSON_GetObjectItemCaseSensitive(data_obj, "oss_access_key_id");
    cJSON *signature = cJSON_GetObjectItemCaseSensitive(data_obj, "signature");
    cJSON *policy = cJSON_GetObjectItemCaseSensitive(data_obj, "policy");
    cJSON *x_oss_object_acl = cJSON_GetObjectItemCaseSensitive(data_obj, "x_oss_object_acl");
    cJSON *x_oss_forbid_overwrite = cJSON_GetObjectItemCaseSensitive(data_obj, "x_oss_forbid_overwrite");

    if (!cJSON_IsString(upload_host) || !cJSON_IsString(upload_dir) ||
        !cJSON_IsString(oss_access_key_id) || !cJSON_IsString(signature) ||
        !cJSON_IsString(policy)) {
        ESP_LOGE(TAG, "OSS: missing required policy fields");
        cJSON_Delete(root);
        return NULL;
    }

    char *key = (char *)heap_caps_malloc(512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (key == NULL) key = (char *)malloc(512);
    if (key == NULL) { cJSON_Delete(root); return NULL; }
    snprintf(key, 512, "%s/audio.wav", upload_dir->valuestring);

    char *oss_url = (char *)heap_caps_malloc(600, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (oss_url == NULL) oss_url = (char *)malloc(600);
    if (oss_url == NULL) { free(key); cJSON_Delete(root); return NULL; }
    snprintf(oss_url, 600, "oss://%s", key);

    ESP_LOGI(TAG, "OSS: uploading %d bytes to %s", (int)wav_len, upload_host->valuestring);
    ESP_LOGI(TAG, "OSS: target URL: %s", oss_url);

    char *saved_upload_host = (char *)heap_caps_malloc(512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *saved_key = (char *)heap_caps_malloc(512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *saved_oss_access_key_id = (char *)heap_caps_malloc(256, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *saved_signature = (char *)heap_caps_malloc(512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *saved_policy = (char *)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *saved_x_oss_object_acl = (char *)heap_caps_malloc(64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *saved_x_oss_forbid_overwrite = (char *)heap_caps_malloc(64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!saved_upload_host || !saved_key || !saved_oss_access_key_id ||
        !saved_signature || !saved_policy || !saved_x_oss_object_acl || !saved_x_oss_forbid_overwrite) {
        ESP_LOGE(TAG, "OSS: alloc saved fields failed");
        free(saved_upload_host); free(saved_key); free(saved_oss_access_key_id);
        free(saved_signature); free(saved_policy);
        free(saved_x_oss_object_acl); free(saved_x_oss_forbid_overwrite);
        free(key); free(oss_url); cJSON_Delete(root);
        return NULL;
    }

    strncpy(saved_upload_host, upload_host->valuestring, 511); saved_upload_host[511] = '\0';
    strncpy(saved_key, key, 511); saved_key[511] = '\0';
    strncpy(saved_oss_access_key_id, oss_access_key_id->valuestring, 255); saved_oss_access_key_id[255] = '\0';
    strncpy(saved_signature, signature->valuestring, 511); saved_signature[511] = '\0';
    strncpy(saved_policy, policy->valuestring, 1023); saved_policy[1023] = '\0';
    strncpy(saved_x_oss_object_acl,
            cJSON_IsString(x_oss_object_acl) ? x_oss_object_acl->valuestring : "private",
            63); saved_x_oss_object_acl[63] = '\0';
    strncpy(saved_x_oss_forbid_overwrite,
            cJSON_IsString(x_oss_forbid_overwrite) ? x_oss_forbid_overwrite->valuestring : "true",
            63); saved_x_oss_forbid_overwrite[63] = '\0';

    cJSON_Delete(root);

    size_t form_fields_size = 0;
    char *field_buf = (char *)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (field_buf == NULL) field_buf = (char *)malloc(1024);
    if (field_buf == NULL) {
        ESP_LOGE(TAG, "OSS: alloc field_buf failed");
        goto oss_cleanup;
    }
    size_t n;

    n = multipart_form_field(field_buf, 1024, "OSSAccessKeyId", saved_oss_access_key_id);
    form_fields_size += n;

    n = multipart_form_field(field_buf, 1024, "Signature", saved_signature);
    form_fields_size += n;

    n = multipart_form_field(field_buf, 1024, "policy", saved_policy);
    form_fields_size += n;

    n = multipart_form_field(field_buf, 1024, "x-oss-object-acl", saved_x_oss_object_acl);
    form_fields_size += n;

    n = multipart_form_field(field_buf, 1024, "x-oss-forbid-overwrite", saved_x_oss_forbid_overwrite);
    form_fields_size += n;

    n = multipart_form_field(field_buf, 1024, "key", saved_key);
    form_fields_size += n;

    n = multipart_form_field(field_buf, 1024, "success_action_status", "200");
    form_fields_size += n;

    char file_header[256];
    int fh_len = snprintf(file_header, sizeof(file_header),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n",
        OSS_BOUNDARY);
    form_fields_size += fh_len;

    char closing[64];
    int cl_len = snprintf(closing, sizeof(closing), "\r\n--%s--\r\n", OSS_BOUNDARY);
    form_fields_size += cl_len;

    size_t total_body_len = form_fields_size + wav_len;

    ESP_LOGI(TAG, "OSS: multipart body size: %d bytes (fields=%d wav=%d)",
             (int)total_body_len, (int)form_fields_size, (int)wav_len);

    esp_http_client_config_t post_cfg = {
        .url = saved_upload_host,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 60000,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 16384,
    };

    esp_http_client_handle_t post_client = esp_http_client_init(&post_cfg);
    if (post_client == NULL) {
        ESP_LOGE(TAG, "OSS: failed to init POST client");
        return NULL;
    }

    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", OSS_BOUNDARY);
    esp_http_client_set_header(post_client, "Content-Type", content_type);

    err = esp_http_client_open(post_client, total_body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OSS: open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(post_client);
        return NULL;
    }

    int written = 0;

    n = multipart_form_field(field_buf, 1024, "OSSAccessKeyId", saved_oss_access_key_id);
    written += esp_http_client_write(post_client, field_buf, n);

    n = multipart_form_field(field_buf, 1024, "Signature", saved_signature);
    written += esp_http_client_write(post_client, field_buf, n);

    n = multipart_form_field(field_buf, 1024, "policy", saved_policy);
    written += esp_http_client_write(post_client, field_buf, n);

    n = multipart_form_field(field_buf, 1024, "x-oss-object-acl", saved_x_oss_object_acl);
    written += esp_http_client_write(post_client, field_buf, n);

    n = multipart_form_field(field_buf, 1024, "x-oss-forbid-overwrite", saved_x_oss_forbid_overwrite);
    written += esp_http_client_write(post_client, field_buf, n);

    n = multipart_form_field(field_buf, 1024, "key", saved_key);
    written += esp_http_client_write(post_client, field_buf, n);

    n = multipart_form_field(field_buf, 1024, "success_action_status", "200");
    written += esp_http_client_write(post_client, field_buf, n);

    written += esp_http_client_write(post_client, file_header, fh_len);

    int wav_written = 0;
    while (wav_written < (int)wav_len) {
        int chunk = esp_http_client_write(post_client,
                                           (const char *)(wav_data + wav_written),
                                           wav_len - wav_written);
        if (chunk <= 0) {
            ESP_LOGE(TAG, "OSS: write wav data failed at offset %d", wav_written);
            esp_http_client_close(post_client);
            esp_http_client_cleanup(post_client);
            free(field_buf); free(key); free(oss_url);
            free(saved_upload_host); free(saved_key); free(saved_oss_access_key_id);
            free(saved_signature); free(saved_policy);
            free(saved_x_oss_object_acl); free(saved_x_oss_forbid_overwrite);
            return NULL;
        }
        wav_written += chunk;
    }
    written += wav_written;

    written += esp_http_client_write(post_client, closing, cl_len);

    ESP_LOGI(TAG, "OSS: written %d / %d bytes", written, (int)total_body_len);

    int fetch_status = 0;
    esp_err_t close_err = esp_http_client_fetch_headers(post_client);
    if (close_err == ESP_OK) {
        fetch_status = esp_http_client_get_status_code(post_client);
    }

    ESP_LOGI(TAG, "OSS: upload response status=%d", fetch_status);

    esp_http_client_close(post_client);
    esp_http_client_cleanup(post_client);

    if (fetch_status != 200) {
        ESP_LOGE(TAG, "OSS: upload failed with status=%d", fetch_status);
        free(field_buf); free(key); free(oss_url);
        free(saved_upload_host); free(saved_key); free(saved_oss_access_key_id);
        free(saved_signature); free(saved_policy);
        free(saved_x_oss_object_acl); free(saved_x_oss_forbid_overwrite);
        return NULL;
    }

    ESP_LOGI(TAG, "OSS: upload success, URL=%s", oss_url);

    char *result = strdup(oss_url);
    free(field_buf); free(key); free(oss_url);
    free(saved_upload_host); free(saved_key); free(saved_oss_access_key_id);
    free(saved_signature); free(saved_policy);
    free(saved_x_oss_object_acl); free(saved_x_oss_forbid_overwrite);
    return result;

oss_cleanup:
    free(key); free(oss_url);
    free(saved_upload_host); free(saved_key); free(saved_oss_access_key_id);
    free(saved_signature); free(saved_policy);
    free(saved_x_oss_object_acl); free(saved_x_oss_forbid_overwrite);
    return NULL;
}

esp_err_t omni_client_init(const omni_client_config_t *config,
                           const omni_client_callbacks_t *callbacks) {
    if (s_initialized) return ESP_OK;
    if (config == NULL || config->api_key[0] == '\0') {
        ESP_LOGE(TAG, "api_key is required");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(s_config));
    if (callbacks) {
        memcpy(&s_callbacks, callbacks, sizeof(s_callbacks));
    }

    // Initialize persistent DashScope client for getPolicy (no event handler, reusable)
    esp_http_client_config_t dashscope_cfg = {
        .url = "https://dashscope.aliyuncs.com",
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .keep_alive_enable = true,
        .keep_alive_idle = 30,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
    };
    s_dashscope_client = esp_http_client_init(&dashscope_cfg);
    if (s_dashscope_client == NULL) {
        ESP_LOGE(TAG, "Failed to init persistent DashScope client");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Persistent DashScope client initialized with keep-alive");

    s_initialized = true;
    ESP_LOGI(TAG, "initialized (model=%s voice=%s)", s_config.model, s_config.voice);
    return ESP_OK;
}

void omni_client_deinit(void) {
    if (s_dashscope_client) {
        esp_http_client_cleanup(s_dashscope_client);
        s_dashscope_client = NULL;
    }
    s_initialized = false;
    ESP_LOGI(TAG, "deinitialized");
}

esp_err_t omni_client_send_audio(const int16_t *pcm_data, size_t pcm_frames,
                                  uint32_t sample_rate) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (pcm_data == NULL || pcm_frames == 0) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "encoding %d frames @ %dHz", pcm_frames, sample_rate);

    size_t pcm_data_size = pcm_frames * sizeof(int16_t);
    size_t wav_total_len = 44 + pcm_data_size;
    uint8_t *wav_data = (uint8_t *)heap_caps_malloc(wav_total_len, MALLOC_CAP_SPIRAM);
    if (wav_data == NULL) {
        ESP_LOGE(TAG, "wav alloc failed (%d bytes)", wav_total_len);
        return ESP_ERR_NO_MEM;
    }

    make_wav_header(wav_data, pcm_frames, sample_rate);
    memcpy(wav_data + 44, pcm_data, pcm_data_size);

    const char *model_raw = s_config.model[0] ? s_config.model : CONFIG_VOICE_MODEL;
    const char *voice = s_config.voice[0] ? s_config.voice : CONFIG_VOICE_OMNI_VOICE;
    const char *instructions = s_config.instructions[0] ? s_config.instructions : NULL;
    char instructions_with_env[1536];
    if (instructions != NULL) {
        instructions = build_instructions_with_env(instructions, instructions_with_env,
                                                   sizeof(instructions_with_env));
    }

    char model_clean[64];
    strncpy(model_clean, model_raw, sizeof(model_clean) - 1);
    model_clean[sizeof(model_clean) - 1] = '\0';
    sanitize_model_name(model_clean);
    if (strcmp(model_clean, model_raw) != 0) {
        ESP_LOGW(TAG, "Sanitized model name: %s -> %s", model_raw, model_clean);
    }
    const char *model = model_clean;

    char *oss_url = upload_audio_to_oss(wav_data, wav_total_len, model, s_config.api_key);
    heap_caps_free(wav_data);

    if (oss_url == NULL) {
        ESP_LOGE(TAG, "OSS upload failed, cannot send audio");
        if (s_callbacks.on_error) {
            s_callbacks.on_error(ESP_FAIL, "OSS upload failed", s_callbacks.user_data);
        }
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "audio URL: %s", oss_url);

    char *body = build_request_body(oss_url, model, voice, instructions);
    free(oss_url);

    if (body == NULL) {
        ESP_LOGE(TAG, "failed to build request body");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "request body: %d bytes", (int)strlen(body));
    ESP_LOGI(TAG, "request body preview: %.500s", body);

    char auth[300];
    snprintf(auth, sizeof(auth), "Bearer %s", s_config.api_key);

    sse_ctx_t ctx;
    sse_ctx_init(&ctx);

    // SSE request needs a fresh client each time because event_handler user_data
    // is a local variable (sse_ctx_t ctx) and cannot be safely changed on a
    // persistent client (esp_http_client_set_event_handler does not exist).
    esp_http_client_config_t http_cfg = {
        .url = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
        .method = HTTP_METHOD_POST,
        .timeout_ms = OMNI_REQUEST_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 16384,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        free(body);
        sse_ctx_cleanup(&ctx);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-DashScope-OssResourceResolve", "enable");
    esp_http_client_set_post_field(client, body, strlen(body));

    ESP_LOGI(TAG, "sending HTTP request...");
    ESP_LOGI(TAG, "internal RAM free before request: %d bytes",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    free(body);

    ESP_LOGI(TAG, "HTTP complete: err=%s status=%d sse_data=%d audio_chunks=%d",
             esp_err_to_name(err), status, ctx.sse_data_count, ctx.audio_chunk_count);
    ESP_LOGI(TAG, "internal RAM free after request: %d bytes",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    esp_http_client_cleanup(client);

    if (ctx.has_error) {
        ESP_LOGE(TAG, "API error (HTTP %d): %s", status, ctx.error_msg);
        if (ctx.http_error_body[0]) {
            ESP_LOGE(TAG, "Raw SSE HTTP error body (%d bytes): %s", (int)ctx.http_error_len, ctx.http_error_body);
        }
        if (ctx.sse_line_len > 0) {
            ESP_LOGE(TAG, "Raw SSE parsed line (%d bytes): %.*s", (int)ctx.sse_line_len,
                     (int)ctx.sse_line_len, ctx.sse_line_buf);
        }
        if (s_callbacks.on_error) {
            s_callbacks.on_error(ESP_FAIL, ctx.error_msg, s_callbacks.user_data);
        }
        err = ESP_FAIL;
    } else if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        if (ctx.http_error_body[0]) {
            ESP_LOGE(TAG, "HTTP error body: %s", ctx.http_error_body);
        }
        if (s_callbacks.on_error) {
            s_callbacks.on_error(ESP_ERR_HTTP_CONNECT, "HTTP error", s_callbacks.user_data);
        }
        err = ESP_FAIL;
    }

    if (err == ESP_OK && !ctx.response_done) {
        ESP_LOGW(TAG, "response stream ended without done signal");
    }

    if (err == ESP_OK && s_callbacks.on_response_done) {
        s_callbacks.on_response_done(s_callbacks.user_data);
    }

    sse_ctx_cleanup(&ctx);
    return err;
}
