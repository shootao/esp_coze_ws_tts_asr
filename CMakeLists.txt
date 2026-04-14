set(public_include_dirs "./include")

idf_component_register(
    INCLUDE_DIRS "${public_include_dirs}"
    SRCS "esp_coze_ws_tts_asr.c"
    PRIV_REQUIRES
        esp_http_client
        json
        mbedtls
        esp_websocket_client
)
