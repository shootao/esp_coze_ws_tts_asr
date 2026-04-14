/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "bsp/esp32_s3_korvo_2.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

#include "esp_coze_ws_tts_asr.h"

static const char *TAG = "COZE_WS_APP";

static esp_coze_ws_tts_asr_handle_t s_client = NULL;
static esp_coze_ws_tts_asr_config_t s_client_cfg;
static bool                         s_wifi_connected             = false;
static TaskHandle_t                 s_asr_capture_task           = NULL;
static volatile bool                s_asr_capture_running        = false;
static volatile bool                s_asr_capture_stop_requested = false;
#if CONFIG_COZE_WS_ENABLE_TTS
static esp_codec_dev_handle_t s_speaker_dev    = NULL;
static bool                   s_speaker_opened = false;
#endif  /* CONFIG_COZE_WS_ENABLE_TTS */
#if CONFIG_COZE_WS_ENABLE_ASR
static esp_codec_dev_handle_t s_microphone_dev    = NULL;
static bool                   s_microphone_opened = false;
#endif  /* CONFIG_COZE_WS_ENABLE_ASR */

static int cmd_asr_complete_common(void);

#if CONFIG_COZE_WS_ENABLE_TTS || CONFIG_COZE_WS_ENABLE_ASR
static esp_err_t codec_dev_ret_to_esp_err(int ret)
{
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : (esp_err_t)ret;
}

static esp_codec_dev_sample_info_t build_codec_sample_info(uint32_t sample_rate)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = sample_rate,
        .mclk_multiple = 0,
    };

    return fs;
}
#endif  /* CONFIG_COZE_WS_ENABLE_TTS || CONFIG_COZE_WS_ENABLE_ASR */

#if CONFIG_COZE_WS_ENABLE_TTS
static const char *audio_source_to_name(esp_coze_ws_tts_asr_audio_source_t source)
{
    switch (source) {
        case ESP_COZE_WS_TTS_ASR_AUDIO_SOURCE_TTS:
            return "tts";
        case ESP_COZE_WS_TTS_ASR_AUDIO_SOURCE_ASR:
            return "asr";
        case ESP_COZE_WS_TTS_ASR_AUDIO_SOURCE_UNKNOWN:
        default:
            return "unknown";
    }
}
#endif  /* CONFIG_COZE_WS_ENABLE_TTS */

#if CONFIG_COZE_WS_ENABLE_TTS
static esp_err_t init_output_device(void)
{
    if (s_speaker_dev == NULL) {
        s_speaker_dev = bsp_audio_codec_speaker_init();
        ESP_RETURN_ON_FALSE(s_speaker_dev != NULL, ESP_FAIL, TAG, "speaker init failed");
    }
    if (s_speaker_opened) {
        return ESP_OK;
    }

    esp_codec_dev_sample_info_t fs = build_codec_sample_info(s_client_cfg.tts.sample_rate);
    int ret = esp_codec_dev_open(s_speaker_dev, &fs);
    ESP_RETURN_ON_FALSE(ret == ESP_CODEC_DEV_OK, ret, TAG, "speaker open failed");

    ret = esp_codec_dev_set_out_vol(s_speaker_dev, s_client_cfg.tts.output_volume);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Failed to set speaker volume: %d", ret);
    }

    s_speaker_opened = true;
    return ESP_OK;
}

static esp_err_t app_audio_write(const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid audio write args");
    ESP_RETURN_ON_FALSE(s_speaker_dev != NULL && s_speaker_opened, ESP_ERR_INVALID_STATE, TAG, "Speaker not ready");
    ESP_RETURN_ON_FALSE(len > 0 && len <= INT32_MAX, ESP_ERR_INVALID_ARG, TAG, "Invalid audio write len");

    int ret = esp_codec_dev_write(s_speaker_dev, (void *)data, (int)len);
    return codec_dev_ret_to_esp_err(ret);
}
#endif  /* CONFIG_COZE_WS_ENABLE_TTS */

#if CONFIG_COZE_WS_ENABLE_ASR
static esp_err_t init_input_device(void)
{
    if (s_microphone_dev == NULL) {
        s_microphone_dev = bsp_audio_codec_microphone_init();
        ESP_RETURN_ON_FALSE(s_microphone_dev != NULL, ESP_FAIL, TAG, "microphone init failed");
    }
    if (s_microphone_opened) {
        return ESP_OK;
    }

    esp_codec_dev_sample_info_t fs = build_codec_sample_info(s_client_cfg.asr.sample_rate);
    int ret = esp_codec_dev_open(s_microphone_dev, &fs);
    ESP_RETURN_ON_FALSE(ret == ESP_CODEC_DEV_OK, ret, TAG, "microphone open failed");

    ret = esp_codec_dev_set_in_gain(s_microphone_dev, s_client_cfg.asr.input_gain);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Failed to set microphone gain: %d", ret);
    }

    s_microphone_opened = true;
    return ESP_OK;
}

static esp_err_t app_audio_read(uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid audio read args");
    ESP_RETURN_ON_FALSE(s_microphone_dev != NULL && s_microphone_opened, ESP_ERR_INVALID_STATE, TAG, "Microphone not ready");
    ESP_RETURN_ON_FALSE(len > 0 && len <= INT32_MAX, ESP_ERR_INVALID_ARG, TAG, "Invalid audio read len");

    int ret = esp_codec_dev_read(s_microphone_dev, data, (int)len);
    return codec_dev_ret_to_esp_err(ret);
}
#endif  /* CONFIG_COZE_WS_ENABLE_ASR */

#if CONFIG_COZE_WS_ENABLE_TTS
static void coze_tts_audio_handler(esp_coze_ws_tts_asr_audio_source_t source,
                                   const uint8_t *data, size_t len,
                                   const char *payload, void *user_ctx)
{
    static size_t s_total_bytes = 0;
    static size_t s_chunk_count = 0;
    static bool s_playback_warning_emitted = false;

    (void)payload;
    (void)user_ctx;

    s_total_bytes += len;
    s_chunk_count++;
    if (s_chunk_count == 1 || (s_chunk_count % 20) == 0) {
        ESP_LOGI(TAG, "Audio callback[%s] received %u bytes, total=%u bytes, chunks=%u",
                 audio_source_to_name(source),
                 (unsigned int)len,
                 (unsigned int)s_total_bytes,
                 (unsigned int)s_chunk_count);
    }

    if (s_client_cfg.tts.auto_play && data != NULL && len > 0) {
        esp_err_t ret = app_audio_write(data, len);
        if (ret != ESP_OK && !s_playback_warning_emitted) {
            s_playback_warning_emitted = true;
            ESP_LOGW(TAG, "Audio playback skipped: %s", esp_err_to_name(ret));
        }
    }
}
#endif  /* CONFIG_COZE_WS_ENABLE_TTS */

#if CONFIG_COZE_WS_ENABLE_ASR
static esp_coze_ws_tts_asr_audio_codec_t get_uplink_codec(void)
{
#if CONFIG_COZE_WS_UPLINK_CODEC_OPUS
    return ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_OPUS;
#elif CONFIG_COZE_WS_UPLINK_CODEC_G711A
    return ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_G711A;
#else
    return ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_PCM;
#endif  /* CONFIG_COZE_WS_UPLINK_CODEC_OPUS */
}
#endif  /* CONFIG_COZE_WS_ENABLE_ASR */

#if CONFIG_COZE_WS_ENABLE_TTS
static esp_coze_ws_tts_asr_audio_codec_t get_downlink_codec(void)
{
#if CONFIG_COZE_WS_DOWNLINK_CODEC_G711A
    return ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_G711A;
#elif CONFIG_COZE_WS_DOWNLINK_CODEC_OPUS
    return ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_OPUS;
#else
    return ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_PCM;
#endif  /* CONFIG_COZE_WS_DOWNLINK_CODEC_G711A */
}
#endif  /* CONFIG_COZE_WS_ENABLE_TTS */

static uint32_t get_enabled_features(void)
{
    uint32_t features = ESP_COZE_WS_TTS_ASR_FEATURE_NONE;

#if CONFIG_COZE_WS_ENABLE_ASR
    features |= ESP_COZE_WS_TTS_ASR_FEATURE_ASR;
#endif  /* CONFIG_COZE_WS_ENABLE_ASR */
#if CONFIG_COZE_WS_ENABLE_TTS
    features |= ESP_COZE_WS_TTS_ASR_FEATURE_TTS;
#endif  /* CONFIG_COZE_WS_ENABLE_TTS */
    return features;
}

static const char *app_codec_to_name(esp_coze_ws_tts_asr_audio_codec_t codec)
{
    switch (codec) {
        case ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_OPUS:
            return "opus";
        case ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_G711A:
            return "g711a";
        case ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_PCM:
        default:
            return "pcm";
    }
}

static void coze_event_handler(esp_coze_ws_tts_asr_event_t event,
                               const char *data,
                               const char *payload,
                               void *user_ctx)
{
    (void)user_ctx;

    switch (event) {
        case ESP_COZE_WS_TTS_ASR_EVENT_CONNECTED:
            ESP_LOGD(TAG, "WebSocket connected");
            break;
        case ESP_COZE_WS_TTS_ASR_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected");
            break;
        case ESP_COZE_WS_TTS_ASR_EVENT_CHAT_UPDATED:
            ESP_LOGD(TAG, "Chat updated");
            break;
        case ESP_COZE_WS_TTS_ASR_EVENT_CHAT_COMPLETED:
            ESP_LOGI(TAG, "Chat completed");
            break;
        case ESP_COZE_WS_TTS_ASR_EVENT_SPEECH_STARTED:
            ESP_LOGD(TAG, "Speech started");
            break;
        case ESP_COZE_WS_TTS_ASR_EVENT_SPEECH_STOPPED:
            ESP_LOGD(TAG, "Speech stopped");
            break;
        case ESP_COZE_WS_TTS_ASR_EVENT_ASR_UPDATED:
            if (data) {
                ESP_LOGI(TAG, "ASR update: %s", data);
            }
            break;
        case ESP_COZE_WS_TTS_ASR_EVENT_SUBTITLE:
            if (payload) {
                ESP_LOGD(TAG, "Subtitle: %s", payload);
            }
            break;
        case ESP_COZE_WS_TTS_ASR_EVENT_MESSAGE_DELTA:
            if (payload) {
                ESP_LOGD(TAG, "Message delta: %s", payload);
            }
            break;
        case ESP_COZE_WS_TTS_ASR_EVENT_MESSAGE_COMPLETED:
            if (payload) {
                ESP_LOGD(TAG, "Message completed: %s", payload);
            }
            break;
        case ESP_COZE_WS_TTS_ASR_EVENT_ERROR:
            ESP_LOGE(TAG, "Coze error: %s", payload ? payload : "(null)");
            break;
        default:
            break;
    }
}

static void init_client_config(void)
{
    s_client_cfg = (esp_coze_ws_tts_asr_config_t)ESP_COZE_WS_TTS_ASR_DEFAULT_CONFIG();
    s_client_cfg.enabled_features = get_enabled_features();
    s_client_cfg.ws_url = CONFIG_COZE_WS_BASE_URL;
    s_client_cfg.conversation_create_url = CONFIG_COZE_WS_CONV_CREATE_URL;
    s_client_cfg.bot_id = CONFIG_COZE_WS_BOT_ID;
    s_client_cfg.access_token = CONFIG_COZE_WS_ACCESS_TOKEN;
    s_client_cfg.user_id = CONFIG_COZE_WS_USER_ID;
    s_client_cfg.enable_subtitle = CONFIG_COZE_WS_ENABLE_SUBTITLE;
    s_client_cfg.event_cb = coze_event_handler;

#if CONFIG_COZE_WS_ENABLE_ASR
    s_client_cfg.asr.codec = get_uplink_codec();
    s_client_cfg.asr.sample_rate = CONFIG_COZE_WS_UPLINK_SAMPLE_RATE;
    s_client_cfg.asr.frame_duration_ms = CONFIG_COZE_WS_UPLINK_FRAME_DURATION_MS;
    s_client_cfg.asr.bitrate = CONFIG_COZE_WS_UPLINK_BITRATE;
    s_client_cfg.asr.turn_detection = CONFIG_COZE_WS_ENABLE_SERVER_VAD ? ESP_COZE_WS_TTS_ASR_TURN_DETECTION_SERVER_VAD : ESP_COZE_WS_TTS_ASR_TURN_DETECTION_NONE;
    s_client_cfg.asr.vad_prefix_padding_ms = CONFIG_COZE_WS_VAD_PREFIX_PADDING_MS;
    s_client_cfg.asr.vad_silence_duration_ms = CONFIG_COZE_WS_VAD_SILENCE_DURATION_MS;
    s_client_cfg.asr.input_gain = (float)CONFIG_COZE_WS_INPUT_GAIN;
#endif  /* CONFIG_COZE_WS_ENABLE_ASR */

#if CONFIG_COZE_WS_ENABLE_TTS
    s_client_cfg.tts.voice_id = CONFIG_COZE_WS_VOICE_ID;
    s_client_cfg.tts.auto_play = CONFIG_COZE_WS_AUTO_PLAY_TTS;
    s_client_cfg.tts.codec = get_downlink_codec();
    s_client_cfg.tts.sample_rate = CONFIG_COZE_WS_DOWNLINK_SAMPLE_RATE;
    s_client_cfg.tts.frame_duration_ms = CONFIG_COZE_WS_DOWNLINK_FRAME_DURATION_MS;
    s_client_cfg.tts.bitrate = CONFIG_COZE_WS_DOWNLINK_BITRATE;
    s_client_cfg.tts.speech_rate = CONFIG_COZE_WS_SPEECH_RATE;
    s_client_cfg.tts.output_volume = CONFIG_COZE_WS_OUTPUT_VOLUME;
    s_client_cfg.tts.audio_cb = coze_tts_audio_handler;
#endif  /* CONFIG_COZE_WS_ENABLE_TTS */
}

static bool ensure_client_ready(void)
{
    if (s_client != NULL) {
        return true;
    }

    ESP_LOGE(TAG, "Client is not initialized, please check Coze config in menuconfig");
    return false;
}

static char *join_args(int argc, char **argv, int start_idx)
{
    size_t total_len = 1;

    for (int i = start_idx; i < argc; i++) {
        total_len += strlen(argv[i]) + 1;
    }

    char *joined = calloc(1, total_len);
    if (joined == NULL) {
        return NULL;
    }

    for (int i = start_idx; i < argc; i++) {
        if (i > start_idx) {
            strlcat(joined, " ", total_len);
        }
        strlcat(joined, argv[i], total_len);
    }

    return joined;
}

static size_t get_asr_chunk_size(void)
{
    size_t frame_ms = (size_t)(s_client_cfg.asr.frame_duration_ms > 0 ? s_client_cfg.asr.frame_duration_ms : 60);

    switch (s_client_cfg.asr.codec) {
        case ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_G711A:
            return ((size_t)s_client_cfg.asr.sample_rate * frame_ms) / 1000;
        case ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_OPUS:
            if (s_client_cfg.asr.bitrate > 0) {
                return ((size_t)s_client_cfg.asr.bitrate * frame_ms) / 8000;
            }
            return 512;
        case ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_PCM:
        default:
            return ((size_t)s_client_cfg.asr.sample_rate * frame_ms * sizeof(int16_t)) / 1000;
    }
}

static esp_err_t stop_asr_capture_task(void)
{
    if (s_asr_capture_task == NULL) {
        s_asr_capture_running = false;
        s_asr_capture_stop_requested = false;
        return ESP_OK;
    }

    s_asr_capture_stop_requested = true;
    for (int i = 0; i < 50; i++) {
        if (s_asr_capture_task == NULL) {
            s_asr_capture_stop_requested = false;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGE(TAG, "Timed out waiting for ASR capture task to stop");
    return ESP_ERR_TIMEOUT;
}

static void asr_capture_task(void *arg)
{
    (void)arg;

    size_t chunk_size = get_asr_chunk_size();
    if (chunk_size == 0) {
        chunk_size = 1024;
    }

    uint8_t *buffer = malloc(chunk_size);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "No memory for ASR capture buffer");
        goto exit;
    }

    size_t total_bytes = 0;
    size_t chunk_count = 0;
    ESP_LOGI(TAG, "ASR microphone capture started, chunk=%u bytes", (unsigned int)chunk_size);

    while (!s_asr_capture_stop_requested) {
        esp_err_t ret = app_audio_read(buffer, chunk_size);
        if (ret != ESP_OK) {
            if (!s_asr_capture_stop_requested) {
                ESP_LOGE(TAG, "ASR microphone read failed: %s", esp_err_to_name(ret));
            }
            break;
        }
        if (s_asr_capture_stop_requested) {
            break;
        }

        esp_coze_ws_tts_asr_send_request_t request = {
            .type = ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_AUDIO,
            .data.audio = {
                .data = buffer,
                .len = chunk_size,
            },
        };

        ret = esp_coze_ws_tts_asr_send(s_client, &request);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ASR microphone send failed at chunk %u: %s",
                     (unsigned int)(chunk_count + 1), esp_err_to_name(ret));
            break;
        }

        total_bytes += chunk_size;
        chunk_count++;
    }

    free(buffer);
    ESP_LOGI(TAG, "ASR microphone capture stopped, bytes=%u, chunks=%u",
             (unsigned int)total_bytes, (unsigned int)chunk_count);

exit:
    s_asr_capture_running = false;
    s_asr_capture_stop_requested = false;
    s_asr_capture_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t send_asr_file(const char *path)
{
    ESP_RETURN_ON_FALSE(path != NULL, ESP_ERR_INVALID_ARG, TAG, "ASR file path is null");
    ESP_RETURN_ON_FALSE(ensure_client_ready(), ESP_ERR_INVALID_STATE, TAG, "Client not ready");
    ESP_RETURN_ON_FALSE(esp_coze_ws_tts_asr_is_connected(s_client), ESP_ERR_INVALID_STATE, TAG, "WebSocket is not connected");

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open %s, errno=%d", path, errno);
        return ESP_FAIL;
    }

    size_t chunk_size = get_asr_chunk_size();
    if (chunk_size == 0) {
        chunk_size = 1024;
    }

    uint8_t *buffer = malloc(chunk_size);
    if (buffer == NULL) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    size_t total_bytes = 0;
    size_t chunk_count = 0;

    while (true) {
        size_t read_bytes = fread(buffer, 1, chunk_size, fp);
        if (read_bytes > 0) {
            esp_coze_ws_tts_asr_send_request_t request = {
                .type = ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_AUDIO,
                .data.audio = {
                    .data = buffer,
                    .len = read_bytes,
                },
            };

            ret = esp_coze_ws_tts_asr_send(s_client, &request);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "ASR file send failed at chunk %u: %s",
                         (unsigned int)(chunk_count + 1), esp_err_to_name(ret));
                break;
            }

            total_bytes += read_bytes;
            chunk_count++;
            if ((chunk_count % 16) == 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            continue;
        }

        if (ferror(fp)) {
            ESP_LOGE(TAG, "Read file failed: %s", path);
            ret = ESP_FAIL;
        }
        break;
    }

    fclose(fp);
    free(buffer);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ASR file sent: %s, bytes=%u, chunks=%u. Run `asr end` next.",
                 path, (unsigned int)total_bytes, (unsigned int)chunk_count);
    }
    return ret;
}

static int cmd_wifi(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (s_wifi_connected) {
        ESP_LOGI(TAG, "Wi-Fi is already connected");
        return 0;
    }

    esp_err_t ret = example_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi connect failed: %s", esp_err_to_name(ret));
        return 1;
    }

    s_wifi_connected = true;
    ESP_LOGI(TAG, "Wi-Fi connected");
    return 0;
}

static int cmd_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!ensure_client_ready()) {
        return 1;
    }
    if (!s_wifi_connected) {
        ESP_LOGE(TAG, "Wi-Fi is not connected, run `wifi` first");
        return 1;
    }

    esp_err_t ret = esp_coze_ws_tts_asr_start(s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start failed: %s", esp_err_to_name(ret));
        return 1;
    }

    ESP_LOGI(TAG, "Coze session started");
    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!ensure_client_ready()) {
        return 1;
    }
    if (stop_asr_capture_task() != ESP_OK) {
        return 1;
    }

    esp_err_t ret = esp_coze_ws_tts_asr_stop(s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Stop failed: %s", esp_err_to_name(ret));
        return 1;
    }

    ESP_LOGI(TAG, "Coze session stopped");
    return 0;
}

static int cmd_tts(int argc, char **argv)
{
    if (argc < 2) {
        ESP_LOGE(TAG, "Usage: tts <text>");
        return 1;
    }
    if (!ensure_client_ready()) {
        return 1;
    }
    if (!esp_coze_ws_tts_asr_is_connected(s_client)) {
        ESP_LOGE(TAG, "WebSocket is not connected, run `start` first");
        return 1;
    }

    char *text = join_args(argc, argv, 1);
    if (text == NULL) {
        ESP_LOGE(TAG, "No memory for TTS text");
        return 1;
    }

    esp_coze_ws_tts_asr_send_request_t request = {
        .type = ESP_COZE_WS_TTS_ASR_SEND_TYPE_TTS_TEXT,
        .data.text = text,
    };

    esp_err_t ret = esp_coze_ws_tts_asr_send(s_client, &request);
    free(text);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TTS send failed: %s", esp_err_to_name(ret));
        return 1;
    }

    ESP_LOGI(TAG, "TTS text sent");
    return 0;
}

static int cmd_asr(int argc, char **argv)
{
    if (argc < 2) {
        ESP_LOGE(TAG, "Usage: asr start");
        ESP_LOGE(TAG, "       asr end");
        ESP_LOGE(TAG, "       asr file <file_path>");
        return 1;
    }

    if (strcmp(argv[1], "start") == 0) {
        if (argc != 2) {
            ESP_LOGE(TAG, "Usage: asr start");
            return 1;
        }
        if (!ensure_client_ready()) {
            return 1;
        }
        if ((esp_coze_ws_tts_asr_get_enabled_features(s_client) & ESP_COZE_WS_TTS_ASR_FEATURE_ASR) == 0) {
            ESP_LOGE(TAG, "ASR feature is disabled");
            return 1;
        }
        if (!esp_coze_ws_tts_asr_is_connected(s_client)) {
            ESP_LOGE(TAG, "WebSocket is not connected, run `start` first");
            return 1;
        }
        if (s_client_cfg.asr.codec != ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_PCM) {
            ESP_LOGE(TAG, "Microphone capture only supports PCM uplink, current codec=%s. Use `asr file <path>` for pre-encoded data.",
                     app_codec_to_name(s_client_cfg.asr.codec));
            return 1;
        }
        if (s_asr_capture_running || s_asr_capture_task != NULL) {
            ESP_LOGW(TAG, "ASR microphone capture is already running");
            return 1;
        }
        s_asr_capture_stop_requested = false;
        BaseType_t ok = xTaskCreate(asr_capture_task, "coze_asr_cap", 4096, NULL, 5, &s_asr_capture_task);
        if (ok != pdPASS) {
            s_asr_capture_task = NULL;
            ESP_LOGE(TAG, "Failed to create ASR capture task");
            return 1;
        }
        s_asr_capture_running = true;
        return 0;
    }

    if (strcmp(argv[1], "end") == 0 || strcmp(argv[1], "stop") == 0 || strcmp(argv[1], "complete") == 0) {
        return cmd_asr_complete_common();
    }

    if (strcmp(argv[1], "file") == 0) {
        if (argc != 3) {
            ESP_LOGE(TAG, "Usage: asr file <file_path>");
            return 1;
        }
        if (s_asr_capture_running || s_asr_capture_task != NULL) {
            ESP_LOGE(TAG, "ASR microphone capture is running, stop it before sending a file");
            return 1;
        }
        return send_asr_file(argv[2]) == ESP_OK ? 0 : 1;
    }

    ESP_LOGE(TAG, "Usage: asr start");
    ESP_LOGE(TAG, "       asr end");
    ESP_LOGE(TAG, "       asr file <file_path>");
    return 1;
}

static int cmd_asr_complete_common(void)
{
    esp_err_t stop_ret = stop_asr_capture_task();
    if (stop_ret != ESP_OK) {
        return 1;
    }
    if (!ensure_client_ready()) {
        return 1;
    }
    if (!esp_coze_ws_tts_asr_is_connected(s_client)) {
        ESP_LOGE(TAG, "WebSocket is not connected, run `start` first");
        return 1;
    }

    esp_coze_ws_tts_asr_send_request_t request = {
        .type = ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_AUDIO_COMPLETE,
    };

    esp_err_t ret = esp_coze_ws_tts_asr_send(s_client, &request);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ASR complete failed: %s", esp_err_to_name(ret));
        return 1;
    }

    ESP_LOGI(TAG, "ASR complete sent");
    return 0;
}

static int cmd_complete(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return cmd_asr_complete_common();
}

static int cmd_conple(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return cmd_asr_complete_common();
}

static void register_console_command(const char *command, const char *help, esp_console_cmd_func_t func)
{
    const esp_console_cmd_t cmd = {
        .command = command,
        .help = help,
        .hint = NULL,
        .func = func,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static void console_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    repl_config.prompt = "coze_ws>";
    repl_config.max_cmdline_length = 256;

    esp_console_register_help_command();
    register_console_command("wifi", "Connect Wi-Fi using sdkconfig credentials", cmd_wifi);
    register_console_command("start", "Start Coze websocket session", cmd_start);
    register_console_command("stop", "Stop Coze websocket session", cmd_stop);
    register_console_command("tts", "Send TTS text, example: tts ni hao", cmd_tts);
    register_console_command("asr", "ASR control: asr start | asr end | asr file /sdcard/test.pcm", cmd_asr);
    register_console_command("complete", "Alias of: asr end", cmd_complete);
    register_console_command("conple", "Alias of complete", cmd_conple);

    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static void try_mount_sdcard(void)
{
    esp_err_t ret = bsp_sdcard_mount();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted at %s", BSP_SD_MOUNT_POINT);
    } else {
        ESP_LOGW(TAG, "SD card mount skipped: %s", esp_err_to_name(ret));
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    init_nvs();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    init_client_config();
    try_mount_sdcard();

#if CONFIG_COZE_WS_ENABLE_TTS
    if (init_output_device() != ESP_OK) {
        ESP_LOGE(TAG, "init_output_device failed");
    }
#endif  /* CONFIG_COZE_WS_ENABLE_TTS */
#if CONFIG_COZE_WS_ENABLE_ASR
    if (init_input_device() != ESP_OK) {
        ESP_LOGE(TAG, "init_input_device failed");
    }
#endif  /* CONFIG_COZE_WS_ENABLE_ASR */

    if (s_client_cfg.enabled_features == ESP_COZE_WS_TTS_ASR_FEATURE_NONE) {
        ESP_LOGE(TAG, "At least one of ASR/TTS must be enabled");
    } else {
        esp_err_t ret = esp_coze_ws_tts_asr_init(&s_client_cfg, &s_client);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_coze_ws_tts_asr_init failed: %s", esp_err_to_name(ret));
        }
    }

    console_init();

    ESP_LOGI(TAG, "Console ready");
    ESP_LOGI(TAG, "Commands:");
    ESP_LOGI(TAG, "  wifi");
    ESP_LOGI(TAG, "  start");
    ESP_LOGI(TAG, "  tts <text>");
    ESP_LOGI(TAG, "  asr start");
    ESP_LOGI(TAG, "  asr end");
    ESP_LOGI(TAG, "  asr file <file_path>");
    ESP_LOGI(TAG, "  complete");
}
