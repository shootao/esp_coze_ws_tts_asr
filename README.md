# esp_coze_ws_tts_asr

`esp_coze_ws_tts_asr` 是一个基于 Coze WebSocket 的 ASR / TTS 组件，当前主要用于 ESP 板端语音交互场景。

对应 example：

- `components/esp_coze_ws_tts_asr/examples/esp_coze_ws_tts_asr`

## 功能

- 支持只开 ASR、只开 TTS，或者同时开启
- 统一的发送接口
- 支持 WebSocket 事件回调
- 支持下行音频回调
- 组件本身不依赖板级 codec，音频输入输出由应用层处理

## 主要接口

- `esp_coze_ws_tts_asr_init()`
- `esp_coze_ws_tts_asr_start()`
- `esp_coze_ws_tts_asr_send()`
- `esp_coze_ws_tts_asr_stop()`
- `esp_coze_ws_tts_asr_deinit()`

## 基本流程

1. 配置 `esp_coze_ws_tts_asr_config_t`
2. 设置 `bot_id`、`access_token`
3. 注册 `event_cb`，需要的话注册 `tts.audio_cb`
4. 调用 `esp_coze_ws_tts_asr_init()`
5. 调用 `esp_coze_ws_tts_asr_start()`
6. TTS 发送 `ESP_COZE_WS_TTS_ASR_SEND_TYPE_TTS_TEXT`
7. 应用层自己读取麦克风数据后发送 `ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_AUDIO`
8. 一轮 ASR 结束后发送 `ESP_COZE_WS_TTS_ASR_SEND_TYPE_ASR_AUDIO_COMPLETE`

## 事件说明

- `ESP_COZE_WS_TTS_ASR_EVENT_ASR_UPDATED`
  ASR 中间识别结果，`data` 是解析后的文本
- `ESP_COZE_WS_TTS_ASR_EVENT_ASR_COMPLETED`
  ASR 完成事件，`data` 是解析后的文本
- `ESP_COZE_WS_TTS_ASR_EVENT_CHAT_COMPLETED`
  对话完成事件
- `ESP_COZE_WS_TTS_ASR_EVENT_ERROR`
  错误事件

回调里：

- `data` 是解析后的内容
- `payload` 是服务端原始 JSON

Coze 官方事件文档：

- ASR: https://www.coze.cn/open/docs/developer_guides/asr_event
- TTS: https://www.coze.cn/open/docs/developer_guides/tts_event

## TTS 音色

`voice_id` 可参考 Coze 官方音色列表：

- https://www.coze.cn/open/docs/developer_guides/list_voices

## Kconfig

- `ESP_COZE_WS_TTS_ASR_ENABLE_DEBUG_LOG`

打开后会打印上下行 JSON，音频字段会做裁剪。
