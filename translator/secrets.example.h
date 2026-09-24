#pragma once
// 复制本文件为 secrets.h 并填入你的信息（secrets.h 已被 .gitignore 排除）
//
// API key 申请：智谱开放平台 https://open.bigmodel.cn
//   控制台 -> API Keys -> 创建。glm-asr / cogtts 按量计费，glm-4-flash 免费。
// 如需换其他 OpenAI 兼容服务，改 API_HOST 与三个 PATH 即可。

static const char *WIFI_SSID = "your-wifi-name";
static const char *WIFI_PASS = "your-wifi-password";
static const char *API_KEY = "your-api-key";

static const char *API_HOST = "open.bigmodel.cn";
static const char *ASR_PATH = "/api/paas/v4/audio/asr";
static const char *CHAT_PATH = "/api/paas/v4/chat/completions";
static const char *TTS_PATH = "/api/paas/v4/audio/speech";
