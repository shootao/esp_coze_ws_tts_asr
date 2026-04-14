#include "esp_coze_ws_tts_asr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "ESP_COZE_WS_TTS_ASR";

#define WS_CONNECTED_BIT        BIT0
#define WS_DISCONNECTED_BIT     BIT1
#define WS_CHAT_READY_BIT       BIT2

typedef struct {
    char   *body;
    size_t  len;
    size_t  cap;
} http_response_ctx_t;

typedef struct {
    esp_coze_ws_tts_asr_config_t        cfg;
    esp_websocket_client_handle_t   ws_client;
    EventGroupHandle_t              events;
    SemaphoreHandle_t               lock;
    char                           *ws_uri;
    char                           *auth_header;
    char                           *conversation_id;
    char                           *event_id;
    char                           *rx_text_buf;
    size_t                          rx_text_cap;
    bool                            ws_started;
    bool                            connected;
    bool                            asr_ready;
    bool                            asr_running;
    bool                            tts_ready;
    size_t                          asr_tx_bytes;
    size_t                          asr_tx_chunks;
} esp_coze_ws_tts_asr_t;

static const char *s_default_events[] = {
    "conversation.audio.delta",
    "conversation.audio_transcript.update",
    "conversation.audio_transcript.completed",
    "conversation.chat.completed",
    "conversation.message.completed",
    "conversation.message.delta",
    "input_audio_buffer.speech_started",
    "input_audio_buffer.speech_stopped",
    "transcriptions.message.update",
    "transcriptions.message.completed",
    "chat.created",
    "conversation.chat.failed",
    "error",
    NULL,
};

static esp_err_t esp_coze_ws_asr_init(esp_coze_ws_tts_asr_t *client);
static esp_err_t esp_coze_ws_asr_stop(esp_coze_ws_tts_asr_t *client);
static esp_err_t esp_coze_ws_asr_deinit(esp_coze_ws_tts_asr_t *client);
static esp_err_t esp_coze_ws_asr_send_audio(esp_coze_ws_tts_asr_t *client, const uint8_t *data, size_t len);
static esp_err_t esp_coze_ws_asr_send_audio_complete(esp_coze_ws_tts_asr_t *client);
static esp_err_t esp_coze_ws_asr_clear_buffer(esp_coze_ws_tts_asr_t *client);
static esp_err_t esp_coze_ws_asr_cancel(esp_coze_ws_tts_asr_t *client);
static esp_err_t esp_coze_ws_tts_init(esp_coze_ws_tts_asr_t *client);
static esp_err_t esp_coze_ws_tts_deinit(esp_coze_ws_tts_asr_t *client);
static esp_err_t esp_coze_ws_tts_send_text(esp_coze_ws_tts_asr_t *client, const char *text);
static esp_err_t esp_coze_ws_tts_asr_send_custom_json(esp_coze_ws_tts_asr_t *client, const char *json);

static char *dup_string(const char *src)
{
    if (src == NULL) {
        return NULL;
    }
    size_t len = strlen(src) + 1;
    char *copy = malloc(len);
    if (copy) {
        memcpy(copy, src, len);
    }
    return copy;
}

static bool has_bearer_prefix(const char *token)
{
    static const char *prefix = "Bearer ";

    if (token == NULL) {
        return false;
    }
    return strncasecmp(token, prefix, strlen(prefix)) == 0;
}

static char *build_authorization_header(const char *token)
{
    static const char *prefix = "Bearer ";

    if (token == NULL) {
        return NULL;
    }
    if (has_bearer_prefix(token)) {
        return dup_string(token);
    }

    size_t len = strlen(prefix) + strlen(token) + 1;
    char *header = calloc(1, len);
    if (header == NULL) {
        return NULL;
    }
    snprintf(header, len, "%s%s", prefix, token);
    return header;
}

#if CONFIG_ESP_COZE_WS_TTS_ASR_ENABLE_DEBUG_LOG
static void redact_json_audio_field(cJSON *data, const char *field_name)
{
    cJSON *field = cJSON_GetObjectItem(data, field_name);
    if (!cJSON_IsString(field) || field->valuestring == NULL) {
        return;
    }

    char placeholder[80];
    snprintf(placeholder, sizeof(placeholder), "<voice omitted, len=%u>",
             (unsigned int)strlen(field->valuestring));
    cJSON_ReplaceItemInObject(data, field_name, cJSON_CreateString(placeholder));
}

static char *redact_payload_for_log(const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        return dup_string(payload);
    }

    cJSON *event_type = cJSON_GetObjectItem(root, "event_type");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    const char *type = cJSON_IsString(event_type) ? event_type->valuestring : NULL;

    if (type != NULL && cJSON_IsObject(data)) {
        if (strcmp(type, "input_audio_buffer.append") == 0) {
            redact_json_audio_field(data, "delta");
        } else if (strcmp(type, "conversation.audio.delta") == 0) {
            redact_json_audio_field(data, "content");
        }
    }

    char *redacted = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return redacted;
}

static void debug_log_payload(const char *direction, const char *payload)
{
    char *redacted = redact_payload_for_log(payload);
    if (redacted == NULL) {
        ESP_LOGI(TAG, "%s %s", direction, payload);
        return;
    }

    ESP_LOGI(TAG, "%s %s", direction, redacted);
    free(redacted);
}
#else
static void debug_log_payload(const char *direction, const char *payload)
{
    (void)direction;
    (void)payload;
}
#endif

static char **dup_string_array(const char **src)
{
    if (src == NULL) {
        return NULL;
    }
    size_t count = 0;
    while (src[count] != NULL) {
        count++;
    }
    char **dst = calloc(count + 1, sizeof(char *));
    if (dst == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        dst[i] = dup_string(src[i]);
        if (dst[i] == NULL) {
            for (size_t j = 0; j < i; j++) {
                free(dst[j]);
            }
            free(dst);
            return NULL;
        }
    }
    return dst;
}

static void free_string_array(char **arr)
{
    if (arr == NULL) {
        return;
    }
    for (size_t i = 0; arr[i] != NULL; i++) {
        free(arr[i]);
    }
    free(arr);
}

static const char *codec_to_name(esp_coze_ws_tts_asr_audio_codec_t codec)
{
    switch (codec) {
        case ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_PCM:
            return "pcm";
        case ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_OPUS:
            return "opus";
        case ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_G711A:
            return "g711a";
        default:
            return "pcm";
    }
}

static bool feature_enabled(const esp_coze_ws_tts_asr_t *client, uint32_t feature)
{
    return client != NULL && (client->cfg.enabled_features & feature) == feature;
}

static bool feature_configured(uint32_t enabled_features, uint32_t feature)
{
    return (enabled_features & feature) == feature;
}

static void dispatch_event(esp_coze_ws_tts_asr_t *client, esp_coze_ws_tts_asr_event_t event,
                           const char *data, const char *payload)
{
    if (client->cfg.event_cb) {
        client->cfg.event_cb(event, data, payload, client->cfg.user_ctx);
    }
}

static void dispatch_audio_callback(esp_coze_ws_tts_asr_t *client, esp_coze_ws_tts_asr_audio_source_t source,
                                    const uint8_t *data, size_t len, const char *payload)
{
    if (client->cfg.tts.audio_cb) {
        client->cfg.tts.audio_cb(source, data, len, payload, client->cfg.user_ctx);
    }
}

static const char *get_event_content(cJSON *root)
{
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *content = data ? cJSON_GetObjectItem(data, "content") : NULL;

    if (!cJSON_IsString(content) || content->valuestring == NULL) {
        return NULL;
    }
    return content->valuestring;
}

static void handle_asr_update_event(esp_coze_ws_tts_asr_t *client, cJSON *root, const char *payload)
{
    const char *content = get_event_content(root);

    dispatch_event(client, ESP_COZE_WS_TTS_ASR_EVENT_ASR_UPDATED, content, payload);
    dispatch_event(client, ESP_COZE_WS_TTS_ASR_EVENT_SUBTITLE, content, payload);
}

static void handle_asr_completed_event(esp_coze_ws_tts_asr_t *client, cJSON *root, const char *payload)
{
    const char *content = get_event_content(root);

    dispatch_event(client, ESP_COZE_WS_TTS_ASR_EVENT_ASR_COMPLETED, content, payload);
}

static void generate_event_id(esp_coze_ws_tts_asr_t *client)
{
    if (client->event_id == NULL) {
        client->event_id = calloc(1, 48);
    }
    if (client->event_id) {
        snprintf(client->event_id, 48, "%08x-%04x-%04x-%04x-%04x%08x",
                 (unsigned int)(esp_random() & 0x1FFFFFFF),
                 (unsigned int)(esp_random() & 0xFFFF),
                 (unsigned int)(esp_random() & 0xFFFF),
                 (unsigned int)(esp_random() & 0xFFFF),
                 (unsigned int)(esp_random() & 0xFFFF),
                 (unsigned int)esp_random());
    }
}

static esp_err_t http_response_append(http_response_ctx_t *ctx, const char *data, int len)
{
    if (len <= 0) {
        return ESP_OK;
    }
    if (ctx->body == NULL) {
        ctx->cap = 1024;
        ctx->body = calloc(1, ctx->cap);
        ESP_RETURN_ON_FALSE(ctx->body != NULL, ESP_ERR_NO_MEM, TAG, "No mem for HTTP response");
    }
    if (ctx->len + (size_t)len + 1 > ctx->cap) {
        size_t new_cap = ctx->cap;
        while (ctx->len + (size_t)len + 1 > new_cap) {
            new_cap *= 2;
        }
        char *new_buf = realloc(ctx->body, new_cap);
        ESP_RETURN_ON_FALSE(new_buf != NULL, ESP_ERR_NO_MEM, TAG, "No mem for HTTP grow");
        ctx->body = new_buf;
        memset(ctx->body + ctx->cap, 0, new_cap - ctx->cap);
        ctx->cap = new_cap;
    }
    memcpy(ctx->body + ctx->len, data, len);
    ctx->len += (size_t)len;
    ctx->body[ctx->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_ctx_t *ctx = (http_response_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx != NULL) {
        return http_response_append(ctx, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

static esp_err_t send_json_locked(esp_coze_ws_tts_asr_t *client, const char *json)
{
    ESP_RETURN_ON_FALSE(client != NULL && json != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid JSON send args");
    ESP_RETURN_ON_FALSE(client->connected && client->ws_client != NULL, ESP_ERR_INVALID_STATE, TAG, "WebSocket not connected");

    debug_log_payload("WS >>>", json);

    if (xSemaphoreTake(client->lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int ret = esp_websocket_client_send_text(client->ws_client, json, strlen(json), pdMS_TO_TICKS(3000));
    xSemaphoreGive(client->lock);
    ESP_RETURN_ON_FALSE(ret > 0, ESP_FAIL, TAG, "WebSocket send failed");
    return ESP_OK;
}

static esp_err_t send_cjson(esp_coze_ws_tts_asr_t *client, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    ESP_RETURN_ON_FALSE(json != NULL, ESP_ERR_NO_MEM, TAG, "cJSON print failed");
    esp_err_t ret = send_json_locked(client, json);
    cJSON_free(json);
    return ret;
}

static void fill_input_audio_json(const esp_coze_ws_tts_asr_t *client, cJSON *input_audio)
{
    const char *codec_name = codec_to_name(client->cfg.asr.codec);
    cJSON_AddStringToObject(input_audio, "format", codec_name);
    cJSON_AddStringToObject(input_audio, "codec", codec_name);
    cJSON_AddNumberToObject(input_audio, "sample_rate", client->cfg.asr.sample_rate);
    cJSON_AddNumberToObject(input_audio, "channel", 1);
    cJSON_AddNumberToObject(input_audio, "bit_depth", 16);
}

static void fill_output_audio_json(const esp_coze_ws_tts_asr_t *client, cJSON *output_audio)
{
    cJSON *codec_cfg = NULL;
    cJSON *limit_cfg = NULL;

    switch (client->cfg.tts.codec) {
        case ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_OPUS:
            cJSON_AddStringToObject(output_audio, "codec", "opus");
            codec_cfg = cJSON_CreateObject();
            cJSON_AddNumberToObject(codec_cfg, "sample_rate", client->cfg.tts.sample_rate);
            cJSON_AddNumberToObject(codec_cfg, "bitrate", client->cfg.tts.bitrate);
            cJSON_AddNumberToObject(codec_cfg, "frame_size_ms", client->cfg.tts.frame_duration_ms);
            limit_cfg = cJSON_CreateObject();
            cJSON_AddNumberToObject(limit_cfg, "period", 1);
            cJSON_AddNumberToObject(limit_cfg, "max_frame_num", 18);
            cJSON_AddItemToObject(codec_cfg, "limit_config", limit_cfg);
            cJSON_AddItemToObject(output_audio, "opus_config", codec_cfg);
            break;
        case ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_G711A:
            cJSON_AddStringToObject(output_audio, "codec", "g711a");
            codec_cfg = cJSON_CreateObject();
            cJSON_AddNumberToObject(codec_cfg, "sample_rate", client->cfg.tts.sample_rate);
            cJSON_AddNumberToObject(codec_cfg, "frame_size_ms", client->cfg.tts.frame_duration_ms);
            limit_cfg = cJSON_CreateObject();
            cJSON_AddNumberToObject(limit_cfg, "period", 1);
            cJSON_AddNumberToObject(limit_cfg, "max_frame_num", 18);
            cJSON_AddItemToObject(codec_cfg, "limit_config", limit_cfg);
            cJSON_AddItemToObject(output_audio, "pcm_config", codec_cfg);
            break;
        case ESP_COZE_WS_TTS_ASR_AUDIO_CODEC_PCM:
        default:
            cJSON_AddStringToObject(output_audio, "codec", "pcm");
            codec_cfg = cJSON_CreateObject();
            cJSON_AddNumberToObject(codec_cfg, "sample_rate", client->cfg.tts.sample_rate);
            cJSON_AddNumberToObject(codec_cfg, "frame_size_ms", client->cfg.tts.frame_duration_ms);
            limit_cfg = cJSON_CreateObject();
            cJSON_AddNumberToObject(limit_cfg, "period", 1);
            cJSON_AddNumberToObject(limit_cfg, "max_frame_num", 18);
            cJSON_AddItemToObject(codec_cfg, "limit_config", limit_cfg);
            cJSON_AddItemToObject(output_audio, "pcm_config", codec_cfg);
            break;
    }

    cJSON_AddNumberToObject(output_audio, "speech_rate", client->cfg.tts.speech_rate);
    if (client->cfg.tts.voice_id) {
        cJSON_AddStringToObject(output_audio, "voice_id", client->cfg.tts.voice_id);
    }
}

static void add_event_subscriptions(const esp_coze_ws_tts_asr_t *client, cJSON *data)
{
    cJSON *events = cJSON_CreateArray();
    const char **event_list = client->cfg.subscribe_events ? client->cfg.subscribe_events : s_default_events;

    for (size_t i = 0; event_list[i] != NULL; i++) {
        if (!client->cfg.enable_subtitle &&
            (strcmp(event_list[i], "conversation.audio_transcript.update") == 0 ||
             strcmp(event_list[i], "transcriptions.message.update") == 0)) {
            continue;
        }
        if (!feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_TTS) &&
            strcmp(event_list[i], "conversation.audio.delta") == 0) {
            continue;
        }
        if (!feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_ASR) &&
            (strcmp(event_list[i], "input_audio_buffer.speech_started") == 0 ||
             strcmp(event_list[i], "input_audio_buffer.speech_stopped") == 0)) {
            continue;
        }
        cJSON_AddItemToArray(events, cJSON_CreateString(event_list[i]));
    }
    cJSON_AddItemToObject(data, "event_subscriptions", events);
}

static esp_err_t create_conversation(esp_coze_ws_tts_asr_t *client)
{
    if (client->conversation_id != NULL) {
        return ESP_OK;
    }

    http_response_ctx_t resp = {0};
    esp_http_client_config_t http_cfg = {
        .url = client->cfg.conversation_create_url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t http = esp_http_client_init(&http_cfg);
    ESP_RETURN_ON_FALSE(http != NULL, ESP_FAIL, TAG, "HTTP init failed");

    esp_http_client_set_method(http, HTTP_METHOD_POST);
    esp_http_client_set_header(http, "Authorization", client->auth_header);
    esp_http_client_set_header(http, "Content-Type", "application/json");

    esp_err_t ret = esp_http_client_perform(http);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Conversation create failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(http);
        free(resp.body);
        return ret;
    }

    if (resp.body == NULL) {
        esp_http_client_cleanup(http);
        return ESP_FAIL;
    }

    debug_log_payload("HTTP <<<", resp.body);

    cJSON *root = cJSON_Parse(resp.body);
    if (root == NULL) {
        ESP_LOGE(TAG, "Invalid conversation response: %s", resp.body);
        esp_http_client_cleanup(http);
        free(resp.body);
        return ESP_FAIL;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *id = data ? cJSON_GetObjectItem(data, "id") : NULL;
    if (!cJSON_IsString(id) || id->valuestring == NULL) {
        ESP_LOGE(TAG, "conversation_id missing: %s", resp.body);
        cJSON_Delete(root);
        esp_http_client_cleanup(http);
        free(resp.body);
        return ESP_FAIL;
    }

    client->conversation_id = dup_string(id->valuestring);
    cJSON_Delete(root);
    esp_http_client_cleanup(http);
    free(resp.body);
    ESP_RETURN_ON_FALSE(client->conversation_id != NULL, ESP_ERR_NO_MEM, TAG, "No mem for conversation_id");
    ESP_LOGI(TAG, "Conversation ID: %s", client->conversation_id);
    return ESP_OK;
}

static esp_err_t send_chat_update(esp_coze_ws_tts_asr_t *client)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *chat_config = cJSON_CreateObject();
    cJSON *input_audio = NULL;
    cJSON *output_audio = NULL;

    ESP_RETURN_ON_FALSE(root && data && chat_config, ESP_ERR_NO_MEM, TAG, "No mem for chat.update");

    generate_event_id(client);
    cJSON_AddStringToObject(root, "id", client->event_id);
    cJSON_AddStringToObject(root, "event_type", "chat.update");
    cJSON_AddItemToObject(root, "data", data);

    cJSON_AddItemToObject(data, "chat_config", chat_config);
    cJSON_AddBoolToObject(chat_config, "auto_save_history", client->cfg.auto_save_history);
    if (client->conversation_id) {
        cJSON_AddStringToObject(chat_config, "conversation_id", client->conversation_id);
    }
    if (client->cfg.user_id) {
        cJSON_AddStringToObject(chat_config, "user_id", client->cfg.user_id);
    }
    cJSON_AddItemToObject(chat_config, "meta_data", cJSON_CreateObject());
    cJSON_AddItemToObject(chat_config, "custom_variables", cJSON_CreateObject());
    cJSON_AddItemToObject(chat_config, "extra_params", cJSON_CreateObject());
    cJSON_AddItemToObject(chat_config, "parameters", cJSON_CreateObject());

    if (feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_ASR)) {
        input_audio = cJSON_CreateObject();
        ESP_RETURN_ON_FALSE(input_audio != NULL, ESP_ERR_NO_MEM, TAG, "No mem for input_audio");
        fill_input_audio_json(client, input_audio);
        cJSON_AddItemToObject(data, "input_audio", input_audio);
    }
    if (feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_TTS)) {
        output_audio = cJSON_CreateObject();
        ESP_RETURN_ON_FALSE(output_audio != NULL, ESP_ERR_NO_MEM, TAG, "No mem for output_audio");
        fill_output_audio_json(client, output_audio);
        cJSON_AddItemToObject(data, "output_audio", output_audio);
    }

    if (feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_ASR) &&
        client->cfg.asr.turn_detection == ESP_COZE_WS_TTS_ASR_TURN_DETECTION_SERVER_VAD) {
        cJSON *turn_detection = cJSON_CreateObject();
        cJSON_AddStringToObject(turn_detection, "type", "server_vad");
        cJSON_AddNumberToObject(turn_detection, "prefix_padding_ms", client->cfg.asr.vad_prefix_padding_ms);
        cJSON_AddNumberToObject(turn_detection, "silence_duration_ms", client->cfg.asr.vad_silence_duration_ms);
        cJSON_AddItemToObject(data, "turn_detection", turn_detection);
    }

    add_event_subscriptions(client, data);

    esp_err_t ret = send_cjson(client, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t decode_audio_and_dispatch(esp_coze_ws_tts_asr_t *client,
                                           esp_coze_ws_tts_asr_audio_source_t source,
                                           const char *payload,
                                           const char *base64_audio)
{
    size_t decoded_len = 0;
    mbedtls_base64_decode(NULL, 0, &decoded_len, (const unsigned char *)base64_audio, strlen(base64_audio));
    ESP_RETURN_ON_FALSE(decoded_len > 0, ESP_FAIL, TAG, "Invalid base64 audio");

    uint8_t *decoded = malloc(decoded_len + 1);
    ESP_RETURN_ON_FALSE(decoded != NULL, ESP_ERR_NO_MEM, TAG, "No mem for decoded audio");

    int ret = mbedtls_base64_decode(decoded, decoded_len, &decoded_len,
                                    (const unsigned char *)base64_audio, strlen(base64_audio));
    if (ret != 0) {
        free(decoded);
        ESP_LOGE(TAG, "base64 decode failed: %d", ret);
        return ESP_FAIL;
    }

    dispatch_audio_callback(client, source, decoded, decoded_len, payload);
    free(decoded);
    return ESP_OK;
}

static void process_ws_text_payload(esp_coze_ws_tts_asr_t *client, const char *payload)
{
    debug_log_payload("WS <<<", payload);

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        ESP_LOGW(TAG, "Invalid WS payload: %s", payload);
        return;
    }

    cJSON *event_type = cJSON_GetObjectItem(root, "event_type");
    const char *type = cJSON_IsString(event_type) ? event_type->valuestring : NULL;

    if (type == NULL) {
        cJSON_Delete(root);
        return;
    }

    ESP_LOGD(TAG, "WS event: %s", type);

    if (strcmp(type, "conversation.audio.delta") == 0) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        cJSON *content = data ? cJSON_GetObjectItem(data, "content") : NULL;
        if (feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_TTS) &&
            cJSON_IsString(content) && content->valuestring) {
            decode_audio_and_dispatch(client, ESP_COZE_WS_TTS_ASR_AUDIO_SOURCE_TTS, payload, content->valuestring);
        }
    } else if (strcmp(type, "transcriptions.message.update") == 0 ||
               strcmp(type, "conversation.audio_transcript.update") == 0) {
        /* transcriptions.message.update is the documented ASR update event.
         * conversation.audio_transcript.update is kept as a compatibility fallback.
         */
        handle_asr_update_event(client, root, payload);
    } else if (strcmp(type, "transcriptions.message.completed") == 0 ||
               strcmp(type, "conversation.audio_transcript.completed") == 0) {
        /* transcriptions.message.completed is the documented final ASR event.
         * conversation.audio_transcript.completed is kept as a compatibility fallback.
         */
        handle_asr_completed_event(client, root, payload);
    } else if (strcmp(type, "conversation.message.delta") == 0) {
        dispatch_event(client, ESP_COZE_WS_TTS_ASR_EVENT_MESSAGE_DELTA, NULL, payload);
    } else if (strcmp(type, "conversation.message.completed") == 0) {
        dispatch_event(client, ESP_COZE_WS_TTS_ASR_EVENT_MESSAGE_COMPLETED, NULL, payload);
    } else if (strcmp(type, "conversation.chat.completed") == 0) {
        dispatch_event(client, ESP_COZE_WS_TTS_ASR_EVENT_CHAT_COMPLETED, NULL, payload);
    } else if (strcmp(type, "input_audio_buffer.speech_started") == 0 &&
               feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_ASR)) {
        dispatch_event(client, ESP_COZE_WS_TTS_ASR_EVENT_SPEECH_STARTED, NULL, payload);
    } else if (strcmp(type, "input_audio_buffer.speech_stopped") == 0 &&
               feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_ASR)) {
        dispatch_event(client, ESP_COZE_WS_TTS_ASR_EVENT_SPEECH_STOPPED, NULL, payload);
    } else if (strcmp(type, "chat.created") == 0) {
        xEventGroupSetBits(client->events, WS_CHAT_READY_BIT);
        dispatch_event(client, ESP_COZE_WS_TTS_ASR_EVENT_CHAT_UPDATED, NULL, payload);
    } else if (strcmp(type, "error") == 0 || strcmp(type, "conversation.chat.failed") == 0) {
        dispatch_event(client, ESP_COZE_WS_TTS_ASR_EVENT_ERROR, NULL, payload);
    } else {
        dispatch_event(client, ESP_COZE_WS_TTS_ASR_EVENT_MESSAGE_DELTA, NULL, payload);
    }

    cJSON_Delete(root);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_coze_ws_tts_asr_t *client = (esp_coze_ws_tts_asr_t *)((esp_websocket_event_data_t *)event_data)->user_context;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    if (client == NULL) {
        return;
    }

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            client->connected = true;
            xEventGroupClearBits(client->events, WS_DISCONNECTED_BIT);
            xEventGroupSetBits(client->events, WS_CONNECTED_BIT);
            dispatch_event(client, ESP_COZE_WS_TTS_ASR_EVENT_CONNECTED, NULL, NULL);
            ESP_LOGI(TAG, "WebSocket connected");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            client->connected = false;
            xEventGroupSetBits(client->events, WS_DISCONNECTED_BIT);
            dispatch_event(client, ESP_COZE_WS_TTS_ASR_EVENT_DISCONNECTED, NULL, NULL);
            ESP_LOGW(TAG, "WebSocket disconnected");
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code != 0x1) {
                break;
            }
            if (data->payload_offset == 0) {
                free(client->rx_text_buf);
                client->rx_text_buf = calloc(1, data->payload_len + 1);
                client->rx_text_cap = data->payload_len + 1;
            }
            if (client->rx_text_buf == NULL || client->rx_text_cap < (size_t)data->payload_len + 1) {
                break;
            }
            memcpy(client->rx_text_buf + data->payload_offset, data->data_ptr, data->data_len);
            if ((int)(data->payload_offset + data->data_len) == data->payload_len) {
                client->rx_text_buf[data->payload_len] = '\0';
                process_ws_text_payload(client, client->rx_text_buf);
                free(client->rx_text_buf);
                client->rx_text_buf = NULL;
                client->rx_text_cap = 0;
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            dispatch_event(client, ESP_COZE_WS_TTS_ASR_EVENT_ERROR, NULL, NULL);
            ESP_LOGE(TAG, "WebSocket error");
            break;
        default:
            break;
    }
}

static esp_err_t build_ws_uri(esp_coze_ws_tts_asr_t *client)
{
    const char *sep = strchr(client->cfg.ws_url, '?') ? "&" : "?";
    size_t len = strlen(client->cfg.ws_url) + strlen(sep) + strlen("bot_id=") + strlen(client->cfg.bot_id) + 1;
    client->ws_uri = calloc(1, len);
    ESP_RETURN_ON_FALSE(client->ws_uri != NULL, ESP_ERR_NO_MEM, TAG, "No mem for ws uri");
    snprintf(client->ws_uri, len, "%s%sbot_id=%s", client->cfg.ws_url, sep, client->cfg.bot_id);
    return ESP_OK;
}

esp_err_t esp_coze_ws_tts_asr_init(const esp_coze_ws_tts_asr_config_t *config, esp_coze_ws_tts_asr_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(config != NULL && handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid init args");
    ESP_RETURN_ON_FALSE(config->bot_id != NULL && config->access_token != NULL, ESP_ERR_INVALID_ARG, TAG, "bot_id/access_token required");
    ESP_RETURN_ON_FALSE(config->ws_url != NULL && config->conversation_create_url != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "ws_url/conversation_create_url required");
    ESP_RETURN_ON_FALSE(config->enabled_features != ESP_COZE_WS_TTS_ASR_FEATURE_NONE, ESP_ERR_INVALID_ARG,
                        TAG, "At least one feature should be enabled");
    ESP_RETURN_ON_FALSE((config->enabled_features & ~(ESP_COZE_WS_TTS_ASR_FEATURE_ASR | ESP_COZE_WS_TTS_ASR_FEATURE_TTS)) == 0,
                        ESP_ERR_INVALID_ARG, TAG, "Unsupported feature bits");
    if (feature_configured(config->enabled_features, ESP_COZE_WS_TTS_ASR_FEATURE_ASR)) {
        ESP_RETURN_ON_FALSE(config->asr.sample_rate > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid ASR sample_rate");
        ESP_RETURN_ON_FALSE(config->asr.frame_duration_ms > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid ASR frame_duration_ms");
    }
    if (feature_configured(config->enabled_features, ESP_COZE_WS_TTS_ASR_FEATURE_TTS)) {
        ESP_RETURN_ON_FALSE(config->tts.sample_rate > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid TTS sample_rate");
        ESP_RETURN_ON_FALSE(config->tts.frame_duration_ms > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid TTS frame_duration_ms");
    }

    esp_coze_ws_tts_asr_t *client = calloc(1, sizeof(esp_coze_ws_tts_asr_t));
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "No mem for handle");

    client->cfg = *config;
    client->cfg.ws_url = dup_string(config->ws_url);
    client->cfg.conversation_create_url = dup_string(config->conversation_create_url);
    client->cfg.conversation_id = dup_string(config->conversation_id);
    client->cfg.bot_id = dup_string(config->bot_id);
    client->cfg.access_token = dup_string(config->access_token);
    client->auth_header = build_authorization_header(config->access_token);
    client->cfg.user_id = dup_string(config->user_id);
    client->cfg.tts.voice_id = dup_string(config->tts.voice_id);
    client->cfg.subscribe_events = (const char **)dup_string_array(config->subscribe_events);
    client->events = xEventGroupCreate();
    client->lock = xSemaphoreCreateMutex();
    if (client->cfg.conversation_id) {
        client->conversation_id = dup_string(client->cfg.conversation_id);
    }

    if (client->events == NULL || client->lock == NULL ||
        client->cfg.ws_url == NULL || client->cfg.conversation_create_url == NULL ||
        client->cfg.bot_id == NULL || client->cfg.access_token == NULL || client->auth_header == NULL) {
        esp_coze_ws_tts_asr_deinit(client);
        return ESP_ERR_NO_MEM;
    }

    if (feature_configured(config->enabled_features, ESP_COZE_WS_TTS_ASR_FEATURE_ASR)) {
        esp_err_t ret = esp_coze_ws_asr_init(client);
        if (ret != ESP_OK) {
            esp_coze_ws_tts_asr_deinit(client);
            return ret;
        }
    }
    if (feature_configured(config->enabled_features, ESP_COZE_WS_TTS_ASR_FEATURE_TTS)) {
        esp_err_t ret = esp_coze_ws_tts_init(client);
        if (ret != ESP_OK) {
            esp_coze_ws_tts_asr_deinit(client);
            return ret;
        }
    }

    *handle = client;
    return ESP_OK;
}

esp_err_t esp_coze_ws_tts_asr_start(esp_coze_ws_tts_asr_handle_t handle)
{
    esp_coze_ws_tts_asr_t *client = (esp_coze_ws_tts_asr_t *)handle;
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    if (client->connected) {
        return ESP_OK;
    }

    free(client->ws_uri);
    client->ws_uri = NULL;
    ESP_RETURN_ON_ERROR(build_ws_uri(client), TAG, "build ws uri failed");

    esp_websocket_client_config_t ws_cfg = {
        .uri = client->ws_uri,
        .buffer_size = client->cfg.websocket_buffer_size,
        .network_timeout_ms = client->cfg.websocket_network_timeout_ms,
        .reconnect_timeout_ms = client->cfg.websocket_reconnect_timeout_ms,
        .task_prio = 12,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_context = client,
    };

    client->ws_client = esp_websocket_client_init(&ws_cfg);
    ESP_RETURN_ON_FALSE(client->ws_client != NULL, ESP_FAIL, TAG, "ws init failed");
    ESP_RETURN_ON_ERROR(esp_websocket_register_events(client->ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, client), TAG, "ws register failed");
    ESP_RETURN_ON_ERROR(esp_websocket_client_append_header(client->ws_client, "Authorization", client->auth_header), TAG, "ws auth header failed");
    ESP_RETURN_ON_ERROR(esp_websocket_client_start(client->ws_client), TAG, "ws start failed");

    EventBits_t bits = xEventGroupWaitBits(client->events,
                                           WS_CONNECTED_BIT | WS_DISCONNECTED_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(client->cfg.websocket_connect_timeout_ms));
    if (!(bits & WS_CONNECTED_BIT)) {
        return ESP_ERR_TIMEOUT;
    }

    client->ws_started = true;
    ESP_RETURN_ON_ERROR(create_conversation(client), TAG, "create_conversation failed");
    ESP_RETURN_ON_ERROR(send_chat_update(client), TAG, "chat.update failed");
    return ESP_OK;
}

esp_err_t esp_coze_ws_tts_asr_stop(esp_coze_ws_tts_asr_handle_t handle)
{
    esp_coze_ws_tts_asr_t *client = (esp_coze_ws_tts_asr_t *)handle;
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    esp_coze_ws_asr_stop(client);

    if (client->ws_client) {
        esp_websocket_client_stop(client->ws_client);
        esp_websocket_client_destroy(client->ws_client);
        client->ws_client = NULL;
    }

    client->connected = false;
    client->ws_started = false;
    xEventGroupClearBits(client->events, WS_CONNECTED_BIT | WS_CHAT_READY_BIT);
    return ESP_OK;
}

static esp_err_t esp_coze_ws_asr_init(esp_coze_ws_tts_asr_t *client)
{
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_ASR), ESP_ERR_NOT_SUPPORTED, TAG, "ASR disabled");
    if (client->asr_ready) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "ASR ready, sample_rate=%d codec=%s",
             client->cfg.asr.sample_rate, codec_to_name(client->cfg.asr.codec));
    client->asr_ready = true;
    return ESP_OK;
}

static esp_err_t esp_coze_ws_asr_stop(esp_coze_ws_tts_asr_t *client)
{
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    if (!feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_ASR)) {
        return ESP_OK;
    }

    if (client->asr_running && client->connected) {
        esp_err_t ret = esp_coze_ws_asr_send_audio_complete(client);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send input_audio_buffer.complete during stop: %s", esp_err_to_name(ret));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    client->asr_running = false;
    return ESP_OK;
}

static esp_err_t esp_coze_ws_asr_deinit(esp_coze_ws_tts_asr_t *client)
{
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    if (!feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_ASR)) {
        return ESP_OK;
    }

    esp_coze_ws_asr_stop(client);
    client->asr_ready = false;
    return ESP_OK;
}

static esp_err_t esp_coze_ws_tts_init(esp_coze_ws_tts_asr_t *client)
{
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_TTS), ESP_ERR_NOT_SUPPORTED, TAG, "TTS disabled");
    if (client->tts_ready) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "TTS ready, sample_rate=%d codec=%s",
             client->cfg.tts.sample_rate, codec_to_name(client->cfg.tts.codec));
    client->tts_ready = true;
    return ESP_OK;
}

static esp_err_t esp_coze_ws_tts_deinit(esp_coze_ws_tts_asr_t *client)
{
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    if (!feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_TTS)) {
        return ESP_OK;
    }

    client->tts_ready = false;
    return ESP_OK;
}

static esp_err_t esp_coze_ws_asr_send_audio(esp_coze_ws_tts_asr_t *client, const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(client != NULL && data != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid ASR send args");
    ESP_RETURN_ON_FALSE(feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_ASR), ESP_ERR_NOT_SUPPORTED, TAG, "ASR disabled");
    ESP_RETURN_ON_FALSE(client->asr_ready, ESP_ERR_INVALID_STATE, TAG, "ASR not initialized");
    ESP_RETURN_ON_FALSE(len > 0, ESP_OK, TAG, "Empty ASR audio");

    size_t encoded_len = 0;
    mbedtls_base64_encode(NULL, 0, &encoded_len, data, len);
    char *encoded = calloc(1, encoded_len + 1);
    ESP_RETURN_ON_FALSE(encoded != NULL, ESP_ERR_NO_MEM, TAG, "No mem for ASR base64");

    int ret = mbedtls_base64_encode((unsigned char *)encoded, encoded_len + 1, &encoded_len, data, len);
    if (ret != 0) {
        free(encoded);
        ESP_LOGE(TAG, "ASR base64 encode failed: %d", ret);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    generate_event_id(client);
    cJSON_AddStringToObject(root, "id", client->event_id);
    cJSON_AddStringToObject(root, "event_type", "input_audio_buffer.append");
    cJSON_AddStringToObject(payload, "delta", encoded);
    cJSON_AddItemToObject(root, "data", payload);

    if (!client->asr_running) {
        client->asr_tx_bytes = 0;
        client->asr_tx_chunks = 0;
    }
    client->asr_running = true;
    esp_err_t err = send_cjson(client, root);
    cJSON_Delete(root);
    free(encoded);
    if (err != ESP_OK) {
        client->asr_running = false;
    } else {
        client->asr_tx_bytes += len;
        client->asr_tx_chunks++;
        if (client->asr_tx_chunks == 1 || (client->asr_tx_chunks % 10) == 0) {
            ESP_LOGD(TAG, "ASR uplink sent: chunk=%u bytes=%u total=%u",
                     (unsigned int)client->asr_tx_chunks,
                     (unsigned int)len,
                     (unsigned int)client->asr_tx_bytes);
        }
    }
    return err;
}

static esp_err_t esp_coze_ws_asr_send_audio_complete(esp_coze_ws_tts_asr_t *client)
{
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_ASR), ESP_ERR_NOT_SUPPORTED, TAG, "ASR disabled");
    ESP_RETURN_ON_FALSE(client->asr_ready, ESP_ERR_INVALID_STATE, TAG, "ASR not initialized");

    cJSON *root = cJSON_CreateObject();
    generate_event_id(client);
    cJSON_AddStringToObject(root, "id", client->event_id);
    cJSON_AddStringToObject(root, "event_type", "input_audio_buffer.complete");
    cJSON_AddItemToObject(root, "data", cJSON_CreateObject());
    esp_err_t ret = send_cjson(client, root);
    cJSON_Delete(root);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ASR complete sent, total uplink bytes=%u chunks=%u",
                 (unsigned int)client->asr_tx_bytes,
                 (unsigned int)client->asr_tx_chunks);
        client->asr_running = false;
        client->asr_tx_bytes = 0;
        client->asr_tx_chunks = 0;
    }
    return ret;
}

static esp_err_t esp_coze_ws_asr_clear_buffer(esp_coze_ws_tts_asr_t *client)
{
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_ASR), ESP_ERR_NOT_SUPPORTED, TAG, "ASR disabled");
    ESP_RETURN_ON_FALSE(client->asr_ready, ESP_ERR_INVALID_STATE, TAG, "ASR not initialized");

    cJSON *root = cJSON_CreateObject();
    generate_event_id(client);
    cJSON_AddStringToObject(root, "id", client->event_id);
    cJSON_AddStringToObject(root, "event_type", "input_audio_buffer.clear");
    esp_err_t ret = send_cjson(client, root);
    cJSON_Delete(root);
    if (ret == ESP_OK) {
        client->asr_tx_bytes = 0;
        client->asr_tx_chunks = 0;
    }
    return ret;
}

static esp_err_t esp_coze_ws_asr_cancel(esp_coze_ws_tts_asr_t *client)
{
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    cJSON *root = cJSON_CreateObject();
    generate_event_id(client);
    cJSON_AddStringToObject(root, "id", client->event_id);
    cJSON_AddStringToObject(root, "event_type", "conversation.chat.cancel");
    esp_err_t ret = send_cjson(client, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t esp_coze_ws_tts_send_text(esp_coze_ws_tts_asr_t *client, const char *text)
{
    ESP_RETURN_ON_FALSE(client != NULL && text != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid TTS text args");
    ESP_RETURN_ON_FALSE(feature_enabled(client, ESP_COZE_WS_TTS_ASR_FEATURE_TTS), ESP_ERR_NOT_SUPPORTED, TAG, "TTS disabled");
    ESP_RETURN_ON_FALSE(client->tts_ready, ESP_ERR_INVALID_STATE, TAG, "TTS not initialized");

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();

    generate_event_id(client);
    cJSON_AddStringToObject(root, "id", client->event_id);
    cJSON_AddStringToObject(root, "event_type", "conversation.message.create");
    cJSON_AddStringToObject(data, "role", "user");
    cJSON_AddStringToObject(data, "type", "question");
    cJSON_AddStringToObject(data, "content_type", "text");
    cJSON_AddStringToObject(data, "content", text);
    cJSON_AddItemToObject(root, "data", data);

    esp_err_t ret = send_cjson(client, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t esp_coze_ws_tts_asr_send_custom_json(esp_coze_ws_tts_asr_t *client, const char *json)
{
    ESP_RETURN_ON_FALSE(client != NULL && json != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid custom JSON");
    return send_json_locked(client, json);
}

esp_err_t esp_coze_ws_tts_asr_send(esp_coze_ws_tts_asr_handle_t handle, const esp_coze_ws_tts_asr_send_request_t *request)
{
    esp_coze_ws_tts_asr_t *client = (esp_coze_ws_tts_asr_t *)handle;
    ESP_RETURN_ON_FALSE(client != NULL && request != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid send request");

    switch (request->type) {
        case ESP_COZE_WS_TTS_ASR_SEND_TYPE_TTS_TEXT:
            return esp_coze_ws_tts_send_text(client, request->data.text);
        case ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_AUDIO:
            return esp_coze_ws_asr_send_audio(client, request->data.audio.data, request->data.audio.len);
        case ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_AUDIO_COMPLETE:
            return esp_coze_ws_asr_send_audio_complete(client);
        case ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_CLEAR_BUFFER:
            return esp_coze_ws_asr_clear_buffer(client);
        case ESP_COZE_WS_TTS_ASR_SEND_TYPE_CHAT_CANCEL:
            return esp_coze_ws_asr_cancel(client);
        case ESP_COZE_WS_TTS_ASR_SEND_TYPE_CUSTOM_JSON:
            return esp_coze_ws_tts_asr_send_custom_json(client, request->data.json);
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

bool esp_coze_ws_tts_asr_is_connected(esp_coze_ws_tts_asr_handle_t handle)
{
    esp_coze_ws_tts_asr_t *client = (esp_coze_ws_tts_asr_t *)handle;
    return client != NULL && client->connected;
}

uint32_t esp_coze_ws_tts_asr_get_enabled_features(esp_coze_ws_tts_asr_handle_t handle)
{
    esp_coze_ws_tts_asr_t *client = (esp_coze_ws_tts_asr_t *)handle;
    return client ? client->cfg.enabled_features : ESP_COZE_WS_TTS_ASR_FEATURE_NONE;
}

esp_err_t esp_coze_ws_tts_asr_deinit(esp_coze_ws_tts_asr_handle_t handle)
{
    esp_coze_ws_tts_asr_t *client = (esp_coze_ws_tts_asr_t *)handle;
    if (client == NULL) {
        return ESP_OK;
    }

    esp_coze_ws_tts_asr_stop(client);
    esp_coze_ws_asr_deinit(client);
    esp_coze_ws_tts_deinit(client);

    free(client->ws_uri);
    free(client->auth_header);
    free(client->conversation_id);
    free(client->event_id);
    free(client->rx_text_buf);
    free((char *)client->cfg.ws_url);
    free((char *)client->cfg.conversation_create_url);
    free((char *)client->cfg.conversation_id);
    free((char *)client->cfg.bot_id);
    free((char *)client->cfg.access_token);
    free((char *)client->cfg.user_id);
    free((char *)client->cfg.tts.voice_id);
    free_string_array((char **)client->cfg.subscribe_events);

    if (client->lock) {
        vSemaphoreDelete(client->lock);
    }
    if (client->events) {
        vEventGroupDelete(client->events);
    }

    free(client);
    return ESP_OK;
}
