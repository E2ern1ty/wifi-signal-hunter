#pragma once
// 复制本文件为 secrets.h 并填入你的信息（secrets.h 已被 .gitignore 排除）
//
// API key 申请：硅基流动 https://siliconflow.cn / cloud.siliconflow.cn
//   SenseVoiceSmall 语音识别：免费
//   Qwen2.5-7B 翻译：免费
//   CosyVoice2 语音合成：按量计费（很便宜）

static const char *WIFI_SSID = "your-wifi-name";
static const char *WIFI_PASS = "your-wifi-password";
static const char *API_KEY = "your-api-key";

static const char *API_HOST = "api.siliconflow.cn";
static const char *ASR_PATH = "/v1/audio/transcriptions";
static const char *CHAT_PATH = "/v1/chat/completions";
static const char *TTS_PATH = "/v1/audio/speech";

// 讯飞开放平台（语音听写流式 iat）
static const char *XF_APPID = "your-xfyun-appid";
static const char *XF_API_KEY = "your-xfyun-apikey";
static const char *XF_API_SECRET = "your-xfyun-apisecret";
