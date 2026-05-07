#include "afe_bridge.h"

#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vadn_models.h"
#include "model_path.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "afe_bridge";

static const esp_afe_sr_iface_t *s_afe_iface;
static esp_afe_sr_data_t       *s_afe_data;
static srmodel_list_t          *s_models;
static bool                     s_ready;

esp_err_t afe_bridge_init(const char *wakenet_model_name)
{
    if (s_ready) return ESP_OK;
    /* Reduce high-frequency AFE internal warnings (e.g., empty ringbuffer) */
    esp_log_level_set("AFE", ESP_LOG_ERROR);

    s_models = esp_srmodel_init("model");
    if (s_models == NULL) {
        ESP_LOGE(TAG, "model partition load failed");
        return ESP_ERR_NOT_FOUND;
    }

    /* "MR" = 1 Mic channel + 1 Reference (playback) channel */
    afe_config_t *cfg = afe_config_init("MR", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (cfg == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }

    /* AEC: high-performance mode for best echo cancellation */
    cfg->aec_init = true;
    cfg->aec_mode = AEC_MODE_SR_HIGH_PERF;

    cfg->ns_init       = false;

    /* VAD: prefer VADNet when present in the model partition; else WebRTC VAD */
    cfg->vad_init          = true;
    cfg->vad_mode          = VAD_MODE_0;
    char *vad_pick = esp_srmodel_filter(s_models, ESP_VADN_PREFIX, NULL);
    if (vad_pick != NULL) {
      cfg->vad_model_name = strdup(vad_pick);
      if (cfg->vad_model_name == NULL) {
        ESP_LOGE(TAG, "vad_model_name strdup failed");
        afe_config_free(cfg);
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_ERR_NO_MEM;
      }
      ESP_LOGI(TAG, "AFE VADNet model: %s", cfg->vad_model_name);
    } else {
      ESP_LOGW(TAG, "no VADNet in partition; AFE falls back to WebRTC VAD");
    }
    cfg->vad_min_speech_ms = 220;
    cfg->vad_min_noise_ms  = 900;

    /* AGC: enable */
    cfg->agc_init = true;
    cfg->agc_mode = AFE_AGC_MODE_WEBRTC;

    /* WakeNet: "Hi ESP" model */
    cfg->wakenet_init     = true;
    cfg->wakenet_mode     = DET_MODE_90;

    /* Memory: prefer PSRAM on ESP32-S3 */
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_config_check(cfg);
    cfg->agc_mode = AFE_AGC_MODE_WEBRTC;
    afe_config_print(cfg);

    s_afe_iface = esp_afe_handle_from_config(cfg);
    if (s_afe_iface == NULL) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(cfg);
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }

    s_afe_data = s_afe_iface->create_from_config(cfg);
    afe_config_free(cfg);
    if (s_afe_data == NULL) {
        ESP_LOGE(TAG, "afe create_from_config failed");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        s_afe_iface = NULL;
        return ESP_FAIL;
    }

    (void)s_afe_iface->add_wakenet_model(s_afe_data, wakenet_model_name);
    /* Slightly lower wake threshold to improve far-field / low-volume pickup. */
    s_afe_iface->set_wakenet_threshold(s_afe_data, 1, 0.42f);

    int feed_ch  = s_afe_iface->get_feed_chunksize(s_afe_data);
    int fetch_ch = s_afe_iface->get_fetch_chunksize(s_afe_data);
    int rate     = s_afe_iface->get_samp_rate(s_afe_data);
    ESP_LOGI(TAG, "ready: feed=%d fetch=%d rate=%d wakenet=%s",
             feed_ch, fetch_ch, rate, wakenet_model_name);
    s_ready = true;
    return ESP_OK;
}

int afe_bridge_get_feed_chunksize(void)
{
    if (!s_ready || s_afe_iface == NULL || s_afe_data == NULL) return 0;
    return s_afe_iface->get_feed_chunksize(s_afe_data);
}

int afe_bridge_get_fetch_chunksize(void)
{
    if (!s_ready || s_afe_iface == NULL || s_afe_data == NULL) return 0;
    return s_afe_iface->get_fetch_chunksize(s_afe_data);
}

int afe_bridge_get_sample_rate(void)
{
    if (!s_ready || s_afe_iface == NULL || s_afe_data == NULL) return 0;
    return s_afe_iface->get_samp_rate(s_afe_data);
}

esp_err_t afe_bridge_feed(const int16_t *mic_ref_interleaved, size_t samples_per_channel)
{
    if (!s_ready || s_afe_iface == NULL || s_afe_data == NULL) return ESP_ERR_INVALID_STATE;
    if (mic_ref_interleaved == NULL || samples_per_channel == 0) return ESP_ERR_INVALID_ARG;

    int expected = s_afe_iface->get_feed_chunksize(s_afe_data);
    if ((int)samples_per_channel != expected) {
        ESP_LOGW(TAG, "feed size mismatch: got=%u expected=%d", (unsigned)samples_per_channel, expected);
    }

    int rc = s_afe_iface->feed(s_afe_data, mic_ref_interleaved);
    if (rc < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

afe_fetch_result_t *afe_bridge_fetch(TickType_t ticks)
{
    if (!s_ready || s_afe_iface == NULL || s_afe_data == NULL) return NULL;
    return s_afe_iface->fetch_with_delay(s_afe_data, ticks);
}

void afe_bridge_reset(void)
{
    if (!s_ready || s_afe_iface == NULL || s_afe_data == NULL) return;
    s_afe_iface->reset_buffer(s_afe_data);
}

void afe_bridge_deinit(void)
{
    if (!s_ready) return;
    if (s_afe_data != NULL && s_afe_iface != NULL) {
        s_afe_iface->destroy(s_afe_data);
    }
    if (s_models != NULL) {
        esp_srmodel_deinit(s_models);
    }
    s_afe_data  = NULL;
    s_afe_iface = NULL;
    s_models    = NULL;
    s_ready     = false;
}

bool afe_bridge_is_ready(void) { return s_ready; }
