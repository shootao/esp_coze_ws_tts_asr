/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_COZE_WS_TTS_ASR_DEFAULT_WS_URL                  "wss://ws.coze.cn/v1/chat"
#define ESP_COZE_WS_TTS_ASR_DEFAULT_CONVERSATION_CREATE_URL "https://api.coze.cn/v1/conversation/create"

typedef void *esp_coze_ws_tts_asr_handle_t;

typedef enum {
    ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_PCM = 0,
    ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_OPUS,
    ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_G711A,
} esp_coze_ws_tts_asr_audio_codec_t;

typedef enum {
    ESP_COZE_WS_TTS_ASR_AUDIO_SOURCE_UNKNOWN = 0,
    ESP_COZE_WS_TTS_ASR_AUDIO_SOURCE_TTS,
    ESP_COZE_WS_TTS_ASR_AUDIO_SOURCE_ASR,
} esp_coze_ws_tts_asr_audio_source_t;

typedef enum {
    ESP_COZE_WS_TTS_ASR_FEATURE_NONE = 0,
    ESP_COZE_WS_TTS_ASR_FEATURE_ASR  = (1U << 0),
    ESP_COZE_WS_TTS_ASR_FEATURE_TTS  = (1U << 1),
} esp_coze_ws_tts_asr_feature_t;

typedef enum {
    ESP_COZE_WS_TTS_ASR_TURN_DETECTION_NONE = 0,
    ESP_COZE_WS_TTS_ASR_TURN_DETECTION_SERVER_VAD,
} esp_coze_ws_tts_asr_turn_detection_t;

typedef enum {
    ESP_COZE_WS_TTS_ASR_EVENT_CONNECTED = 0,
    ESP_COZE_WS_TTS_ASR_EVENT_DISCONNECTED,
    ESP_COZE_WS_TTS_ASR_EVENT_CHAT_UPDATED,
    ESP_COZE_WS_TTS_ASR_EVENT_CHAT_COMPLETED,
    ESP_COZE_WS_TTS_ASR_EVENT_SPEECH_STARTED,
    ESP_COZE_WS_TTS_ASR_EVENT_SPEECH_STOPPED,
    /* ASR transcript update event. */
    ESP_COZE_WS_TTS_ASR_EVENT_ASR_UPDATED,
    /* ASR transcript completed event. */
    ESP_COZE_WS_TTS_ASR_EVENT_ASR_COMPLETED,
    /* Interim subtitle/transcript JSON payload. */
    ESP_COZE_WS_TTS_ASR_EVENT_SUBTITLE,
    ESP_COZE_WS_TTS_ASR_EVENT_MESSAGE_DELTA,
    ESP_COZE_WS_TTS_ASR_EVENT_MESSAGE_COMPLETED,
    ESP_COZE_WS_TTS_ASR_EVENT_ERROR,
} esp_coze_ws_tts_asr_event_t;

typedef enum {
    ESP_COZE_WS_TTS_ASR_SEND_TYPE_TTS_TEXT = 0,
    ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_AUDIO,
    ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_AUDIO_COMPLETE,
    ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_CLEAR_BUFFER,
    ESP_COZE_WS_TTS_ASR_SEND_TYPE_CHAT_CANCEL,
    ESP_COZE_WS_TTS_ASR_SEND_TYPE_CUSTOM_JSON,
} esp_coze_ws_tts_asr_send_type_t;

/*
 * Event callback:
 *   - data: parsed event data when available. For ASR update/completed events
 *     this is the parsed transcript content.
 *   - payload: raw server JSON payload when available.
 */
typedef void (*esp_coze_ws_tts_asr_event_cb_t)(esp_coze_ws_tts_asr_event_t event,
                                               const char *data,
                                               const char *payload,
                                               void *user_ctx);
typedef void (*esp_coze_ws_tts_asr_audio_cb_t)(esp_coze_ws_tts_asr_audio_source_t source,
                                               const uint8_t *data, size_t len,
                                               const char *payload, void *user_ctx);

typedef struct {
    const uint8_t *data;
    size_t         len;
} esp_coze_ws_tts_asr_audio_buffer_t;

typedef struct {
    esp_coze_ws_tts_asr_send_type_t type;
    union {
        esp_coze_ws_tts_asr_audio_buffer_t audio;
        const char                        *text;
        const char                        *json;
    } data;
} esp_coze_ws_tts_asr_send_request_t;

typedef struct {
    esp_coze_ws_tts_asr_audio_codec_t    codec;
    int                                  sample_rate;
    int                                  frame_duration_ms;
    int                                  bitrate;
    esp_coze_ws_tts_asr_turn_detection_t turn_detection;
    int                                  vad_prefix_padding_ms;
    int                                  vad_silence_duration_ms;
    /* Optional app-side microphone gain setting, not consumed by the component itself. */
    float                                input_gain;
} esp_coze_ws_tts_asr_asr_config_t;

typedef struct {
    const char                    *voice_id;
    /* Optional app-side playback policy flag, not consumed by the component itself. */
    bool                           auto_play;
    esp_coze_ws_tts_asr_audio_codec_t codec;
    int                            sample_rate;
    int                            frame_duration_ms;
    int                            bitrate;
    int                            speech_rate;
    /* Optional app-side speaker volume setting, not consumed by the component itself. */
    int                            output_volume;
    /* Called for decoded downlink audio; source identifies TTS or ASR, payload is raw server JSON. */
    esp_coze_ws_tts_asr_audio_cb_t audio_cb;
} esp_coze_ws_tts_asr_tts_config_t;

typedef struct {
    const char                   *ws_url;
    const char                   *conversation_create_url;
    const char                   *conversation_id;
    const char                   *bot_id;
    const char                   *access_token;
    const char                   *user_id;
    int                           websocket_buffer_size;
    int                           websocket_network_timeout_ms;
    int                           websocket_reconnect_timeout_ms;
    int                           websocket_connect_timeout_ms;
    bool                          auto_save_history;
    bool                          enable_subtitle;
    uint32_t                      enabled_features;
    const char                  **subscribe_events;
    esp_coze_ws_tts_asr_event_cb_t event_cb;
    void                         *user_ctx;
    esp_coze_ws_tts_asr_asr_config_t asr;
    esp_coze_ws_tts_asr_tts_config_t tts;
} esp_coze_ws_tts_asr_config_t;

#define ESP_COZE_WS_TTS_ASR_ASR_DEFAULT_CONFIG() {                           \
    .codec                   = ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_PCM,          \
    .sample_rate             = 16000,                                        \
    .frame_duration_ms       = 60,                                           \
    .bitrate                 = 16000,                                        \
    .turn_detection          = ESP_COZE_WS_TTS_ASR_TURN_DETECTION_SERVER_VAD,\
    .vad_prefix_padding_ms   = 600,                                          \
    .vad_silence_duration_ms = 500,                                          \
    .input_gain              = 32.0f,                                        \
}

#define ESP_COZE_WS_TTS_ASR_TTS_DEFAULT_CONFIG() {                    \
    .voice_id          = "7426720361733144585",                       \
    .auto_play         = true,                                        \
    .codec             = ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_PCM,         \
    .sample_rate       = 16000,                                       \
    .frame_duration_ms = 60,                                          \
    .bitrate           = 16000,                                       \
    .speech_rate       = 20,                                          \
    .output_volume     = 70,                                          \
    .audio_cb          = NULL,                                        \
}

#define ESP_COZE_WS_TTS_ASR_DEFAULT_CONFIG() {                                        \
    .ws_url                         = ESP_COZE_WS_TTS_ASR_DEFAULT_WS_URL,              \
    .conversation_create_url        = ESP_COZE_WS_TTS_ASR_DEFAULT_CONVERSATION_CREATE_URL, \
    .conversation_id                = NULL,                                            \
    .bot_id                         = NULL,                                            \
    .access_token                   = NULL,                                            \
    .user_id                        = "esp32_user",                                   \
    .websocket_buffer_size          = 20480,                                           \
    .websocket_network_timeout_ms   = 5000,                                            \
    .websocket_reconnect_timeout_ms = 1000,                                            \
    .websocket_connect_timeout_ms   = 30000,                                           \
    .auto_save_history              = true,                                            \
    .enable_subtitle                = true,                                            \
    .enabled_features               = ESP_COZE_WS_TTS_ASR_FEATURE_ASR | ESP_COZE_WS_TTS_ASR_FEATURE_TTS, \
    .subscribe_events               = NULL,                                            \
    .event_cb                       = NULL,                                            \
    .user_ctx                       = NULL,                                            \
    .asr                            = ESP_COZE_WS_TTS_ASR_ASR_DEFAULT_CONFIG(),        \
    .tts                            = ESP_COZE_WS_TTS_ASR_TTS_DEFAULT_CONFIG(),        \
}

/**
 * @brief Create a Coze websocket client instance.
 *
 * Enable only the features you need through @c enabled_features:
 * - ASR only: set @c ESP_COZE_WS_TTS_ASR_FEATURE_ASR
 * - TTS only: set @c ESP_COZE_WS_TTS_ASR_FEATURE_TTS
 * - ASR + TTS: set both bits
 */
esp_err_t esp_coze_ws_tts_asr_init(const esp_coze_ws_tts_asr_config_t *config, esp_coze_ws_tts_asr_handle_t *handle);

/**
 * @brief Open websocket, create conversation, and send chat.update.
 */
esp_err_t esp_coze_ws_tts_asr_start(esp_coze_ws_tts_asr_handle_t handle);

/**
 * @brief Stop ASR uplink if needed and close websocket.
 */
esp_err_t esp_coze_ws_tts_asr_stop(esp_coze_ws_tts_asr_handle_t handle);

/**
 * @brief Destroy the client instance and release codec devices.
 */
esp_err_t esp_coze_ws_tts_asr_deinit(esp_coze_ws_tts_asr_handle_t handle);

/**
 * @brief Send a generic request.
 *
 * TTS usage:
 * @code{c}
 * esp_coze_ws_tts_asr_send_request_t req = {
 *     .type = ESP_COZE_WS_TTS_ASR_SEND_TYPE_TTS_TEXT,
 *     .data.text = "hello",
 * };
 * esp_coze_ws_tts_asr_send(handle, &req);
 * @endcode
 *
 * ASR usage:
 * @code{c}
 * esp_coze_ws_tts_asr_send_request_t req = {
 *     .type = ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_AUDIO,
 *     .data.audio = {
 *         .data = pcm,
 *         .len = pcm_len,
 *     },
 * };
 * esp_coze_ws_tts_asr_send(handle, &req);
 *
 * req.type = ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_AUDIO_COMPLETE;
 * esp_coze_ws_tts_asr_send(handle, &req);
 * @endcode
 *
 * If @c cfg.tts.audio_cb is configured, downlink audio is reported through the
 * callback together with a source tag such as
 * @c ESP_COZE_WS_TTS_ASR_AUDIO_SOURCE_TTS or
 * @c ESP_COZE_WS_TTS_ASR_AUDIO_SOURCE_ASR.
 *
 * The component itself does not access board audio devices. Applications should
 * read microphone data by themselves and send it with
 * @c ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_AUDIO, and should play received audio in
 * @c tts.audio_cb if needed.
 *
 * ASR transcript content is reported by:
 *   - @c ESP_COZE_WS_TTS_ASR_EVENT_ASR_UPDATED
 *   - @c ESP_COZE_WS_TTS_ASR_EVENT_ASR_COMPLETED
 *
 * In these callbacks:
 *   - @c data is the parsed transcript text
 *   - @c payload is the raw server JSON
 */
esp_err_t esp_coze_ws_tts_asr_send(esp_coze_ws_tts_asr_handle_t handle, const esp_coze_ws_tts_asr_send_request_t *request);

/**
 * @brief Return true when websocket is connected.
 */
bool esp_coze_ws_tts_asr_is_connected(esp_coze_ws_tts_asr_handle_t handle);

/**
 * @brief Get feature bitmap configured at init time.
 */
uint32_t esp_coze_ws_tts_asr_get_enabled_features(esp_coze_ws_tts_asr_handle_t handle);

#ifndef ESP_COZE_WS_TTS_ASR_DISABLE_LEGACY_NAMES
#define coze_ws_asr_tts_handle_t                     esp_coze_ws_tts_asr_handle_t
#define coze_ws_audio_codec_t                        esp_coze_ws_tts_asr_audio_codec_t
#define coze_ws_audio_source_t                       esp_coze_ws_tts_asr_audio_source_t
#define coze_ws_feature_t                            esp_coze_ws_tts_asr_feature_t
#define coze_ws_turn_detection_t                     esp_coze_ws_tts_asr_turn_detection_t
#define coze_ws_asr_tts_event_t                      esp_coze_ws_tts_asr_event_t
#define coze_ws_send_type_t                          esp_coze_ws_tts_asr_send_type_t
#define coze_ws_asr_tts_event_cb_t                   esp_coze_ws_tts_asr_event_cb_t
#define coze_ws_tts_audio_cb_t                       esp_coze_ws_tts_asr_audio_cb_t
#define coze_ws_audio_buffer_t                       esp_coze_ws_tts_asr_audio_buffer_t
#define coze_ws_send_request_t                       esp_coze_ws_tts_asr_send_request_t
#define coze_ws_asr_config_t                         esp_coze_ws_tts_asr_asr_config_t
#define coze_ws_tts_config_t                         esp_coze_ws_tts_asr_tts_config_t
#define coze_ws_asr_tts_config_t                     esp_coze_ws_tts_asr_config_t

#define COZE_WS_AUDIO_CODEC_PCM                      ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_PCM
#define COZE_WS_AUDIO_CODEC_OPUS                     ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_OPUS
#define COZE_WS_AUDIO_CODEC_G711A                    ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_G711A
#define COZE_WS_AUDIO_SOURCE_UNKNOWN                 ESP_COZE_WS_TTS_ASR_AUDIO_SOURCE_UNKNOWN
#define COZE_WS_AUDIO_SOURCE_TTS                     ESP_COZE_WS_TTS_ASR_AUDIO_SOURCE_TTS
#define COZE_WS_AUDIO_SOURCE_ASR                     ESP_COZE_WS_TTS_ASR_AUDIO_SOURCE_ASR
#define COZE_WS_FEATURE_NONE                         ESP_COZE_WS_TTS_ASR_FEATURE_NONE
#define COZE_WS_FEATURE_ASR                          ESP_COZE_WS_TTS_ASR_FEATURE_ASR
#define COZE_WS_FEATURE_TTS                          ESP_COZE_WS_TTS_ASR_FEATURE_TTS
#define COZE_WS_TURN_DETECTION_NONE                  ESP_COZE_WS_TTS_ASR_TURN_DETECTION_NONE
#define COZE_WS_TURN_DETECTION_SERVER_VAD            ESP_COZE_WS_TTS_ASR_TURN_DETECTION_SERVER_VAD
#define COZE_WS_ASR_TTS_EVENT_CONNECTED              ESP_COZE_WS_TTS_ASR_EVENT_CONNECTED
#define COZE_WS_ASR_TTS_EVENT_DISCONNECTED           ESP_COZE_WS_TTS_ASR_EVENT_DISCONNECTED
#define COZE_WS_ASR_TTS_EVENT_CHAT_UPDATED           ESP_COZE_WS_TTS_ASR_EVENT_CHAT_UPDATED
#define COZE_WS_ASR_TTS_EVENT_CHAT_COMPLETED         ESP_COZE_WS_TTS_ASR_EVENT_CHAT_COMPLETED
#define COZE_WS_ASR_TTS_EVENT_SPEECH_STARTED         ESP_COZE_WS_TTS_ASR_EVENT_SPEECH_STARTED
#define COZE_WS_ASR_TTS_EVENT_SPEECH_STOPPED         ESP_COZE_WS_TTS_ASR_EVENT_SPEECH_STOPPED
#define COZE_WS_ASR_TTS_EVENT_ASR_UPDATED            ESP_COZE_WS_TTS_ASR_EVENT_ASR_UPDATED
#define COZE_WS_ASR_TTS_EVENT_ASR_COMPLETED          ESP_COZE_WS_TTS_ASR_EVENT_ASR_COMPLETED
#define COZE_WS_ASR_TTS_EVENT_SUBTITLE               ESP_COZE_WS_TTS_ASR_EVENT_SUBTITLE
#define COZE_WS_ASR_TTS_EVENT_MESSAGE_DELTA          ESP_COZE_WS_TTS_ASR_EVENT_MESSAGE_DELTA
#define COZE_WS_ASR_TTS_EVENT_MESSAGE_COMPLETED      ESP_COZE_WS_TTS_ASR_EVENT_MESSAGE_COMPLETED
#define COZE_WS_ASR_TTS_EVENT_ERROR                  ESP_COZE_WS_TTS_ASR_EVENT_ERROR
#define COZE_WS_SEND_TYPE_TTS_TEXT                   ESP_COZE_WS_TTS_ASR_SEND_TYPE_TTS_TEXT
#define COZE_WS_SEND_TYPE_ASR_AUDIO                  ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_AUDIO
#define COZE_WS_SEND_TYPE_ASR_AUDIO_COMPLETE         ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_AUDIO_COMPLETE
#define COZE_WS_SEND_TYPE_ASR_CLEAR_BUFFER           ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_CLEAR_BUFFER
#define COZE_WS_SEND_TYPE_CHAT_CANCEL                ESP_COZE_WS_TTS_ASR_SEND_TYPE_CHAT_CANCEL
#define COZE_WS_SEND_TYPE_CUSTOM_JSON                ESP_COZE_WS_TTS_ASR_SEND_TYPE_CUSTOM_JSON

#define COZE_WS_ASR_DEFAULT_CONFIG()                 ESP_COZE_WS_TTS_ASR_ASR_DEFAULT_CONFIG()
#define COZE_WS_TTS_DEFAULT_CONFIG()                 ESP_COZE_WS_TTS_ASR_TTS_DEFAULT_CONFIG()
#define COZE_WS_ASR_TTS_DEFAULT_CONFIG()             ESP_COZE_WS_TTS_ASR_DEFAULT_CONFIG()

#define coze_ws_asr_tts_init                         esp_coze_ws_tts_asr_init
#define coze_ws_asr_tts_start                        esp_coze_ws_tts_asr_start
#define coze_ws_asr_tts_stop                         esp_coze_ws_tts_asr_stop
#define coze_ws_asr_tts_deinit                       esp_coze_ws_tts_asr_deinit
#define coze_ws_asr_tts_send                         esp_coze_ws_tts_asr_send
#define coze_ws_asr_tts_is_connected                 esp_coze_ws_tts_asr_is_connected
#define coze_ws_asr_tts_get_enabled_features         esp_coze_ws_tts_asr_get_enabled_features
#endif

#ifdef __cplusplus
}
#endif
