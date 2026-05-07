
#include "voice_api_client.h"
#include "voice_internal_types.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "esp_timer.h"

#include <string.h>
#include <stdlib.h>

#define TAG "voice_api_client"

#define VOICE_REQUEST_WAV_HEADER_SIZE 44
#define VOICE_REQUEST_LINE_BUF_INITIAL_CAP 8192
#define VOICE_REQUEST_TIMEOUT_MS 30000

static voice_api_config_t s_config = {0};
static bool s_initialized = false;

static const char* k_audio_wav_data_prefix = "data:audio/wav;base64,";

typedef struct {
    char* text_dst;
    size_t text_cap;
    size_t text_len;
    voice_response_audio_cb_t audio_cb;
    void* audio_cb_user_data;
    char* line_buf;
    size_t line_buf_cap;
    size_t line_len;
    bool line_dropping;
    bool has_audio;
} stream_ctx_t;

static void put_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint8_t* wav_from_pcm(const int16_t* pcm, size_t frames, uint32_t sample_rate, size_t* out_len) {
    if (pcm == NULL || frames == 0 || out_len == NULL) return NULL;

    size_t data_size = frames * sizeof(int16_t);
    size_t file_size = VOICE_REQUEST_WAV_HEADER_SIZE + data_size;

    uint8_t* buf = (uint8_t*)malloc(file_size);
    if (buf == NULL) {
        ESP_LOGE(TAG, "wav_from_pcm alloc failed");
        return NULL;
    }

    memcpy(buf, "RIFF", 4);
    put_le32(buf + 4, (uint32_t)(file_size - 8));
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    put_le32(buf + 16, 16);
    put_le16(buf + 20, 1);
    put_le16(buf + 22, 1);
    put_le32(buf + 24, sample_rate);
    put_le32(buf + 28, sample_rate * 2);
    put_le16(buf + 32, 2);
    put_le16(buf + 34, 16);
    memcpy(buf + 36, "data", 4);
    put_le32(buf + 40, (uint32_t)data_size);
    memcpy(buf + VOICE_REQUEST_WAV_HEADER_SIZE, pcm, data_size);

    *out_len = file_size;
    return buf;
}

static char* base64_encode_wav(const uint8_t* input, size_t in_len) {
    if (input == NULL || in_len == 0) {
        return NULL;
    }

    size_t encoded_len_required = 0;
    int rc = mbedtls_base64_encode(NULL, 0, &encoded_len_required, input, in_len);
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || encoded_len_required == 0) {
        return NULL;
    }

    size_t prefix_len = strlen(k_audio_wav_data_prefix);
    size_t total_len = prefix_len + encoded_len_required + 1;

    char* buf = (char*)malloc(total_len);
    if (buf == NULL) {
        return NULL;
    }

    memcpy(buf, k_audio_wav_data_prefix, prefix_len);

    size_t encoded_len_actual = 0;
    rc = mbedtls_base64_encode((unsigned char*)buf + prefix_len, encoded_len_required + 1,
                               &encoded_len_actual, input, in_len);
    if (rc != 0) {
        free(buf);
        return NULL;
    }

    buf[prefix_len + encoded_len_actual] = '\0';
    return buf;
}

static void endpoint_from_url(const char* url, char* dst, size_t cap) {
    const char* base = "https://dashscope.aliyuncs.com/compatible-mode/v1";
    if (url != NULL && strncmp(url, "http", 4) == 0 && strstr(url, "compatible-mode") != NULL) {
        base = url;
    }
    if (strstr(base, "/chat/completions") != NULL) {
        snprintf(dst, cap, "%s", base);
    } else {
        snprintf(dst, cap, "%s/chat/completions", base);
    }
}

static bool parse_wav_format(const uint8_t* buf, size_t len, uint32_t* sample_rate, size_t* data_offset) {
    if (buf == NULL || len < 44) {
        return false;
    }
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0 ||
        memcmp(buf + 12, "fmt ", 4) != 0 || memcmp(buf + 36, "data", 4) != 0) {
        return false;
    }
    if (sample_rate) {
        *sample_rate = (uint32_t)buf[24] | ((uint32_t)buf[25] << 8) |
                       ((uint32_t)buf[26] << 16) | ((uint32_t)buf[27] << 24);
    }
    if (data_offset) {
        *data_offset = 44;
    }
    return true;
}

static void handle_audio_data(const char* b64_data, size_t b64_len, stream_ctx_t* ctx) {
    if (b64_data == NULL || b64_len == 0 || ctx->audio_cb == NULL) return;

    size_t decoded_len = 0;
    int rc = mbedtls_base64_decode(NULL, 0, &decoded_len, (const unsigned char*)b64_data, b64_len);
    if (!(rc == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || rc == 0) || decoded_len == 0) {
        return;
    }

    uint8_t* raw = (uint8_t*)malloc(decoded_len);
    if (raw == NULL) {
        return;
    }

    size_t actual = 0;
    rc = mbedtls_base64_decode(raw, decoded_len, &actual, (const unsigned char*)b64_data, b64_len);
    if (rc != 0 || actual < sizeof(int16_t)) {
        free(raw);
        return;
    }

    uint32_t sample_rate = 24000;
    size_t data_offset = 0;

    if (parse_wav_format(raw, actual, &sample_rate, &data_offset)) {
        if (data_offset < actual) {
            size_t pcm_frames = (actual - data_offset) / sizeof(int16_t);
            if (pcm_frames > 0) {
                ctx->has_audio = true;
                ctx->audio_cb((int16_t*)(raw + data_offset), pcm_frames, sample_rate, ctx->audio_cb_user_data);
            }
        }
    } else {
        size_t pcm_frames = actual / sizeof(int16_t);
        if (pcm_frames > 0) {
            ctx->has_audio = true;
            ctx->audio_cb((int16_t*)raw, pcm_frames, sample_rate, ctx->audio_cb_user_data);
        }
    }

    free(raw);
}

static void stream_handle_sse_line(const char* line, size_t line_len, stream_ctx_t* ctx) {
    if (line_len < 5 || strncmp(line, "data:", 5) != 0) return;

    const char* json_str = line + 5;
    size_t json_len = line_len - 5;
    while (json_len > 0 && (*json_str == ' ' || *json_str == '\t')) {
        json_str++;
        json_len--;
    }

    if (json_len >= 6 && strncmp(json_str, "[DONE]", 6) == 0) return;

    cJSON* root = cJSON_Parse(json_str);
    if (root == NULL) {
        return;
    }

    cJSON* choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (cJSON_IsArray(choices)) {
        cJSON* first = cJSON_GetArrayItem(choices, 0);
        if (first) {
            cJSON* delta = cJSON_GetObjectItemCaseSensitive(first, "delta");
            if (delta) {
                cJSON* content = cJSON_GetObjectItemCaseSensitive(delta, "content");
                if (cJSON_IsString(content) && content->valuestring && ctx->text_dst != NULL) {
                    size_t clen = strlen(content->valuestring);
                    size_t to_copy = (ctx->text_len + clen < ctx->text_cap - 1) ? clen : (ctx->text_cap - 1 - ctx->text_len);
                    if (to_copy > 0) {
                        memcpy(ctx->text_dst + ctx->text_len, content->valuestring, to_copy);
                        ctx->text_len += to_copy;
                        ctx->text_dst[ctx->text_len] = '\0';
                    }
                }

                cJSON* audio_obj = cJSON_GetObjectItemCaseSensitive(delta, "audio");
                if (audio_obj != NULL) {
                    if (cJSON_IsObject(audio_obj)) {
                        cJSON* audio_data = cJSON_GetObjectItemCaseSensitive(audio_obj, "data");
                        if (cJSON_IsString(audio_data) && audio_data->valuestring) {
                            handle_audio_data(audio_data->valuestring, strlen(audio_data->valuestring), ctx);
                        }
                    } else if (cJSON_IsString(audio_obj) && audio_obj->valuestring) {
                        handle_audio_data(audio_obj->valuestring, strlen(audio_obj->valuestring), ctx);
                    }
                }
            }

            cJSON* message = cJSON_GetObjectItemCaseSensitive(first, "message");
            if (cJSON_IsObject(message)) {
                cJSON* msg_audio = cJSON_GetObjectItemCaseSensitive(message, "audio");
                if (cJSON_IsObject(msg_audio)) {
                    cJSON* audio_data = cJSON_GetObjectItemCaseSensitive(msg_audio, "data");
                    if (cJSON_IsString(audio_data) && audio_data->valuestring) {
                        handle_audio_data(audio_data->valuestring, strlen(audio_data->valuestring), ctx);
                    }
                } else if (cJSON_IsString(msg_audio) && msg_audio->valuestring) {
                    handle_audio_data(msg_audio->valuestring, strlen(msg_audio->valuestring), ctx);
                }
            }
        }
    }

    cJSON_Delete(root);
}

static esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    if (evt == NULL || evt->user_data == NULL) return ESP_OK;

    stream_ctx_t* ctx = (stream_ctx_t*)evt->user_data;

    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }

    const char* src = (const char*)evt->data;
    size_t src_len = (size_t)evt->data_len;

    while (src_len > 0) {
        const char* nl = (const char*)memchr(src, '\n', src_len);
        size_t chunk = nl != NULL ? (size_t)(nl - src) : src_len;

        if (chunk > 0 && !ctx->line_dropping) {
            size_t needed = ctx->line_len + chunk + 1;
            if (needed > ctx->line_buf_cap) {
                size_t new_cap = ctx->line_buf_cap * 2;
                if (new_cap < needed) new_cap = needed;
                char* new_buf = (char*)realloc(ctx->line_buf, new_cap);
                if (new_buf != NULL) {
                    ctx->line_buf = new_buf;
                    ctx->line_buf_cap = new_cap;
                } else {
                    ctx->line_dropping = true;
                }
            }

            if (!ctx->line_dropping) {
                memcpy(ctx->line_buf + ctx->line_len, src, chunk);
                ctx->line_len += chunk;
                ctx->line_buf[ctx->line_len] = '\0';
            }
        }

        if (nl != NULL) {
            if (!ctx->line_dropping && ctx->line_len > 0) {
                stream_handle_sse_line(ctx->line_buf, ctx->line_len, ctx);
            }
            ctx->line_len = 0;
            ctx->line_dropping = false;
            src = nl + 1;
            src_len -= chunk + 1;
        } else {
            src += chunk;
            src_len -= chunk;
        }
    }

    return ESP_OK;
}

static char* build_request_body_audio(const char* audio_b64, const char* model) {
    const char* effective_model = (model != NULL && model[0] != '\0') ? model : "qwen3.5-omni-flash";

    cJSON* root = cJSON_CreateObject();
    if (root == NULL) return NULL;

    cJSON_AddStringToObject(root, "model", effective_model);

    cJSON* modalities = cJSON_AddArrayToObject(root, "modalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));

    cJSON_AddBoolToObject(root, "enable_thinking", false);

    cJSON* audio_cfg = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddStringToObject(audio_cfg, "voice", "Tina");
    cJSON_AddStringToObject(audio_cfg, "format", "wav");

    cJSON_AddBoolToObject(root, "stream", true);
    cJSON* stream_opts = cJSON_AddObjectToObject(root, "stream_options");
    cJSON_AddBoolToObject(stream_opts, "include_usage", true);

    cJSON* messages = cJSON_AddArrayToObject(root, "messages");

    cJSON* sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content",
        "You are a realtime assistant running on M5 CoreS3. Always reply in English only. Reply with natural spoken 1-3 sentence answers suitable for TTS.");
    cJSON_AddItemToArray(messages, sys_msg);

    cJSON* user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON* user_content = cJSON_AddArrayToObject(user_msg, "content");

    cJSON* audio_part = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_part, "type", "input_audio");
    cJSON* input_audio = cJSON_AddObjectToObject(audio_part, "input_audio");
    cJSON_AddStringToObject(input_audio, "data", audio_b64);
    cJSON_AddStringToObject(input_audio, "format", "wav");
    cJSON_AddItemToArray(user_content, audio_part);

    cJSON* text_part = cJSON_CreateObject();
    cJSON_AddStringToObject(text_part, "type", "text");
    cJSON_AddStringToObject(text_part, "text", "Please answer my question briefly.");
    cJSON_AddItemToArray(user_content, text_part);

    cJSON_AddItemToArray(messages, user_msg);

    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static char* build_request_body_text(const char* text, const char* model) {
    const char* effective_model = (model != NULL && model[0] != '\0') ? model : "qwen3.5-omni-flash";

    cJSON* root = cJSON_CreateObject();
    if (root == NULL) return NULL;

    cJSON_AddStringToObject(root, "model", effective_model);

    cJSON* modalities = cJSON_AddArrayToObject(root, "modalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));

    cJSON_AddBoolToObject(root, "enable_thinking", false);

    cJSON* audio_cfg = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddStringToObject(audio_cfg, "voice", "Tina");
    cJSON_AddStringToObject(audio_cfg, "format", "wav");

    cJSON_AddBoolToObject(root, "stream", true);
    cJSON* stream_opts = cJSON_AddObjectToObject(root, "stream_options");
    cJSON_AddBoolToObject(stream_opts, "include_usage", true);

    cJSON* messages = cJSON_AddArrayToObject(root, "messages");

    cJSON* sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content",
        "You are a realtime assistant running on M5 CoreS3. Always reply in English only. Reply with natural spoken 1-3 sentence answers suitable for TTS.");
    cJSON_AddItemToArray(messages, sys_msg);

    cJSON* user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", text);
    cJSON_AddItemToArray(messages, user_msg);

    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

esp_err_t voice_api_client_init(const voice_api_config_t* config) {
    if (s_initialized) {
        return ESP_OK;
    }

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(s_config));
    s_initialized = true;
    ESP_LOGI(TAG, "initialized");

    return ESP_OK;
}

void voice_api_client_deinit(void) {
    s_initialized = false;
    ESP_LOGI(TAG, "deinitialized");
}

esp_err_t voice_api_client_send_audio(const voice_audio_request_t* request,
                                       voice_response_audio_cb_t audio_cb,
                                       void* audio_cb_user_data,
                                       char* out_text,
                                       size_t out_text_len) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (request == NULL || request->pcm_data == NULL || request->pcm_frames == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "send audio: frames=%d rate=%d", request->pcm_frames, request->sample_rate);

    size_t wav_len = 0;
    uint8_t* wav_data = wav_from_pcm(request->pcm_data, request->pcm_frames, request->sample_rate, &wav_len);
    if (wav_data == NULL) {
        ESP_LOGE(TAG, "Failed to create WAV");
        return ESP_FAIL;
    }

    char* b64 = base64_encode_wav(wav_data, wav_len);
    free(wav_data);

    if (b64 == NULL) {
        ESP_LOGE(TAG, "Failed to encode base64");
        return ESP_FAIL;
    }

    char* body = build_request_body_audio(b64, s_config.model);
    free(b64);

    if (body == NULL) {
        ESP_LOGE(TAG, "Failed to build request body");
        return ESP_FAIL;
    }

    char endpoint[320];
    endpoint_from_url(s_config.url, endpoint, sizeof(endpoint));

    char auth[300];
    snprintf(auth, sizeof(auth), "Bearer %s", s_config.api_key);

    char* line_buf = (char*)malloc(VOICE_REQUEST_LINE_BUF_INITIAL_CAP);
    if (line_buf == NULL) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    stream_ctx_t ctx = {
        .text_dst = out_text,
        .text_cap = out_text_len,
        .text_len = 0,
        .audio_cb = audio_cb,
        .audio_cb_user_data = audio_cb_user_data,
        .line_buf = line_buf,
        .line_buf_cap = VOICE_REQUEST_LINE_BUF_INITIAL_CAP,
        .line_len = 0,
        .line_dropping = false,
        .has_audio = false,
    };

    if (out_text != NULL && out_text_len > 0) {
        out_text[0] = '\0';
    }

    esp_http_client_config_t http_cfg = {
        .url = endpoint,
        .method = HTTP_METHOD_POST,
        .timeout_ms = VOICE_REQUEST_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        free(line_buf);
        free(body);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (!ctx.line_dropping && ctx.line_len > 0) {
        stream_handle_sse_line(ctx.line_buf, ctx.line_len, &ctx);
    }

    esp_http_client_cleanup(client);
    free(line_buf);
    free(body);

    if (err == ESP_OK && (status < 200 || status >= 300)) {
        err = ESP_FAIL;
    }

    ESP_LOGI(TAG, "request complete: err=%s status=%d has_audio=%d",
             esp_err_to_name(err), status, ctx.has_audio);

    return err;
}

esp_err_t voice_api_client_send_text(const char* text,
                                     voice_response_audio_cb_t audio_cb,
                                     void* audio_cb_user_data,
                                     char* out_text,
                                     size_t out_text_len) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "send text: %s", text);

    char* body = build_request_body_text(text, s_config.model);
    if (body == NULL) {
        ESP_LOGE(TAG, "Failed to build request body");
        return ESP_FAIL;
    }

    char endpoint[320];
    endpoint_from_url(s_config.url, endpoint, sizeof(endpoint));

    char auth[300];
    snprintf(auth, sizeof(auth), "Bearer %s", s_config.api_key);

    char* line_buf = (char*)malloc(VOICE_REQUEST_LINE_BUF_INITIAL_CAP);
    if (line_buf == NULL) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    stream_ctx_t ctx = {
        .text_dst = out_text,
        .text_cap = out_text_len,
        .text_len = 0,
        .audio_cb = audio_cb,
        .audio_cb_user_data = audio_cb_user_data,
        .line_buf = line_buf,
        .line_buf_cap = VOICE_REQUEST_LINE_BUF_INITIAL_CAP,
        .line_len = 0,
        .line_dropping = false,
        .has_audio = false,
    };

    if (out_text != NULL && out_text_len > 0) {
        out_text[0] = '\0';
    }

    esp_http_client_config_t http_cfg = {
        .url = endpoint,
        .method = HTTP_METHOD_POST,
        .timeout_ms = VOICE_REQUEST_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        free(line_buf);
        free(body);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (!ctx.line_dropping && ctx.line_len > 0) {
        stream_handle_sse_line(ctx.line_buf, ctx.line_len, &ctx);
    }

    esp_http_client_cleanup(client);
    free(line_buf);
    free(body);

    if (err == ESP_OK && (status < 200 || status >= 300)) {
        err = ESP_FAIL;
    }

    ESP_LOGI(TAG, "request complete: err=%s status=%d has_audio=%d",
             esp_err_to_name(err), status, ctx.has_audio);

    return err;
}
