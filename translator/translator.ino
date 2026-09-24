/*
 * Pocket Translator — M5StickS3 (v1)
 *
 * 手持翻译器：按住 A 键说话，松开后 语音识别(ASR) → LLM 翻译 → TTS 合成，
 * 译文显示在屏幕上并用喇叭播放。B 键切换目标语言（英/日）。
 *
 * 云端链路（硅基流动 SiliconFlow，一把 API key 通吃）：
 *   POST /v1/audio/transcriptions  16kHz WAV → 文本 (Qwen3-ASR-1.7B, ~0.3s)
 *   POST /v1/chat/completions      文本 → 译文 (Qwen2.5-7B, 免费)
 *   POST /v1/audio/speech          译文 → 语音 (CosyVoice2, WAV/chunked)
 *
 * 音频：ES8311 编解码器，M5.Mic 采集 / M5.Speaker 播放（二者互斥，用时切换）。
 * MP3 解码：ESP8266Audio(libhelix) 解到 PSRAM PCM 再 playRaw。
 * 界面：中日韩文字用 M5GFX 内置 efont 字体。
 *
 * 首次使用：复制 secrets.example.h 为 secrets.h，填 WiFi 和 API key。
 */

#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <AudioFileSourcePROGMEM.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define SECRETS_MISSING 1
#include "secrets.example.h"
#endif

// ---------- 常量 ----------
enum Lang { LANG_EN = 0, LANG_JA = 1 };
enum State { ST_BOOT, ST_NOSECRET, ST_WIFI, ST_IDLE, ST_REC,
             ST_ASR, ST_LLM, ST_TTS, ST_PLAY, ST_RESULT, ST_ERR };

static const uint32_t SAMPLE_RATE = 16000;
static const size_t CHUNK = 1024;                 // 每块 64ms
static const size_t MAX_SAMPLES = SAMPLE_RATE * 45; // 最长 45 秒
static const int MIN_SAMPLES = SAMPLE_RATE / 2;   // 短于 0.5s 视为误触
static const char *BOUNDARY = "----StickS3Boundary7f3a9c";

// ---------- 全局状态 ----------
static State state = ST_BOOT;
static Lang lang = LANG_EN;
static int16_t *recPcm = nullptr;      // PSRAM 录音缓冲
static volatile size_t recSamples = 0;
static int16_t chunkBuf[CHUNK];
static float recLevel = 0;             // 0..1 电平
static uint32_t recStartMs = 0;
static String srcText, dstText, errMsg;
static uint8_t *ttsAudio = nullptr;    // PSRAM TTS 音频
static size_t ttsLen = 0;
static int16_t *playPcm = nullptr;     // 解码后 PCM
static size_t playSamples = 0;
static uint32_t playRate = 16000;
static uint32_t resultShownMs = 0;
static uint8_t rotCur = 3, rotCandidate = 3;
static unsigned long rotCandidateSince = 0;
static constexpr float AX_SIGN = 1.0f;

// ---------- 小工具 ----------
static const char *langCode(Lang l) { return l == LANG_EN ? "English" : "Japanese"; }
static const char *langLabel(Lang l) { return l == LANG_EN ? "中→EN" : "中→日"; }

static void setState(State s) {
  state = s;
  Serial.printf("[state] %d\r\n", (int)s);
}

// UTF-8 按字符宽度折行绘制，返回实际行数
static int drawWrapped(const char *utf8, int x, int y, int maxW, int maxLines,
                       uint16_t color, int lineH) {
  M5.Lcd.setTextColor(color, TFT_BLACK);
  int line = 0, cx = x;
  const unsigned char *p = (const unsigned char *)utf8;
  while (*p && line < maxLines) {
    // 取一个 UTF-8 字符
    int len = 1;
    if ((*p & 0xE0) == 0xC0) len = 2;
    else if ((*p & 0xF0) == 0xE0) len = 3;
    else if ((*p & 0xF8) == 0xF0) len = 4;
    char tmp[8] = {0};
    strncpy(tmp, (const char *)p, len);
    int w = M5.Lcd.textWidth(tmp);
    if (cx + w > x + maxW) {
      cx = x;
      if (++line >= maxLines) { // 最后一行放省略号
        M5.Lcd.setCursor(cx, y + line * lineH);
        M5.Lcd.print("...");
        return maxLines;
      }
    }
    M5.Lcd.setCursor(cx, y + line * lineH);
    M5.Lcd.print(tmp);
    cx += w;
    p += len;
  }
  return line + (*p ? maxLines : (line ? line : 1));
}

// ---------- 网络诊断 ----------
static String netDiag(const char *stage) {
  String s = String(stage) + " wifi=" + String(WiFi.status() == WL_CONNECTED ? "ok" : "DOWN");
  s += " ip=" + WiFi.localIP().toString();
  s += " dns=" + WiFi.dnsIP().toString();
  IPAddress h;
  if (WiFi.hostByName(API_HOST, h)) {
    s += " host=" + h.toString();
  } else {
    s += " DNS-FAIL";
  }
  return s;
}

// 等待响应首字节。云端 ASR/TTS 推理需要好几秒甚至十几秒，
// 不能依赖 Stream 的 setTimeout（毫秒/秒语义随核心版本漂移）。
static bool waitResponse(WiFiClientSecure &client, uint32_t ms, const char *tag) {
  uint32_t t0 = millis();
  while (!client.available() && client.connected() && millis() - t0 < ms) delay(10);
  if (!client.available()) {
    errMsg = String(tag) + " no-resp(" + String((millis() - t0) / 1000) + "s)";
    Serial.println(errMsg);
    return false;
  }
  return true;
}

// 读到对端关闭（Connection: close）或停滞超时为止。
// 响应是滴过来的，绝不能“读一次缓冲区就走”。
static String readAll(WiFiClientSecure &client, uint32_t stallMs) {
  String out;
  uint32_t lastData = millis();
  while (millis() - lastData < stallMs) {
    if (client.available()) {
      out += (char)client.read();
      lastData = millis();
    } else if (!client.connected()) {
      break;
    } else {
      delay(2);
    }
  }
  return out;
}

// 耐心读一行（等待数据，不会瞬间放弃）
static String readLine(WiFiClientSecure &client, uint32_t stallMs) {
  String out;
  uint32_t lastData = millis();
  while (millis() - lastData < stallMs) {
    if (client.available()) {
      char c = (char)client.read();
      lastData = millis();
      if (c == '\n') break;
      out += c;
    } else if (!client.connected()) {
      break;
    } else {
      delay(1);
    }
  }
  return out;
}

// 从原始响应里拆出状态码与 body（body 在 \r\n\r\n 之后）
static bool splitResponse(const String &raw, int *code, String *body) {
  int p = raw.indexOf("\r\n\r\n");
  if (p < 0) return false;
  String head = raw.substring(0, p);
  if (!head.startsWith("HTTP/")) return false;
  *code = head.substring(9, 12).toInt();
  *body = raw.substring(p + 4);
  return true;
}

// ---------- HTTP 基础 ----------
// 发 JSON POST，返回响应体（文本，JSON 或错误信息）；失败返回空串
static String httpsPostJson(const char *path, const String &body,
                            int *httpCode) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20000);
  if (!client.connect(API_HOST, 443)) return "";
  client.printf("POST %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: M5StickS3-Translator/1.0\r\n"
                "Authorization: Bearer %s\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n",
                path, API_HOST, API_KEY, (int)body.length());
  client.print(body);
  if (!waitResponse(client, 25000, "LLM")) return "";
  String raw = readAll(client, 10000);
  int code2 = 0; String body2;
  if (!splitResponse(raw, &code2, &body2)) { errMsg = "LLM hdr? " + raw.substring(0, 60); return ""; }
  *httpCode = code2;
  return body2;
}

// multipart 上传 WAV 做 ASR，返回 JSON 响应体
static String httpsPostWav(const char *path, const int16_t *pcm, size_t samples,
                           int *httpCode) {
  // 构造 WAV 头（44 字节，字段偏移见 WAV 规范；fmt 魔数在 12-15！）
  size_t dataLen = samples * 2;
  uint8_t hdr[44];
  uint32_t u32;
  uint16_t u16;
  memcpy(hdr + 0, "RIFF", 4);
  u32 = 36 + dataLen; memcpy(hdr + 4, &u32, 4);
  memcpy(hdr + 8, "WAVE", 4);
  memcpy(hdr + 12, "fmt ", 4);
  u32 = 16; memcpy(hdr + 16, &u32, 4);               // fmt 块长度
  u16 = 1; memcpy(hdr + 20, &u16, 2);                // PCM
  u16 = 1; memcpy(hdr + 22, &u16, 2);                // mono
  u32 = SAMPLE_RATE; memcpy(hdr + 24, &u32, 4);
  u32 = SAMPLE_RATE * 2; memcpy(hdr + 28, &u32, 4);  // byte rate
  u16 = 2; memcpy(hdr + 32, &u16, 2);                // block align
  u16 = 16; memcpy(hdr + 34, &u16, 2);               // bits
  memcpy(hdr + 36, "data", 4);
  u32 = dataLen; memcpy(hdr + 40, &u32, 4);

  String pre = String("--") + BOUNDARY + "\r\n"
               "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
               "Qwen/Qwen3-ASR-1.7B\r\n";
  pre += String("--") + BOUNDARY + "\r\n";
  pre += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
  pre += "Content-Type: audio/wav\r\n\r\n";
  String post = String("\r\n--") + BOUNDARY + "--\r\n";
  size_t total = pre.length() + sizeof(hdr) + dataLen + post.length();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20);
  bool ok = client.connect(API_HOST, 443);
  if (!ok) { // 重试一次，排除瞬态
    delay(1000);
    ok = client.connect(API_HOST, 443);
  }
  if (!ok) {
    errMsg = netDiag("ASR-TCP");
    Serial.println(errMsg);
    return "";
  }
  client.printf("POST %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: M5StickS3-Translator/1.0\r\n"
                "Authorization: Bearer %s\r\n"
                "Content-Type: multipart/form-data; boundary=%s\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n",
                path, API_HOST, API_KEY, BOUNDARY, (int)total);
  client.print(pre);
  client.write(hdr, sizeof(hdr));
  const uint8_t *p = (const uint8_t *)pcm;
  size_t sent = 0;
  while (sent < dataLen) {
    size_t n = min((size_t)4096, dataLen - sent);
    size_t off = 0;
    int stall = 0;
    while (off < n) { // 部分写入是正常现象，必须循环补写
      size_t w = client.write(p + sent + off, n - off);
      if (w == 0) {
        if (++stall > 2000) { // ~2 秒无进展判定断流
          errMsg = netDiag("ASR-WR") + String(" @") + String(sent + off);
          Serial.println(errMsg);
          return "";
        }
        delay(1);
      } else {
        stall = 0;
      }
      off += w;
    }
    sent += n;
  }
  client.print(post);

  if (!waitResponse(client, 25000, "ASR")) return "";
  String raw = readAll(client, 10000);
  int code2 = 0; String body2;
  if (!splitResponse(raw, &code2, &body2)) { errMsg = "ASR hdr? " + raw.substring(0, 60); return ""; }
  if (body2.isEmpty()) { errMsg = "ASR HTTP " + String(code2) + " empty-body"; return ""; }
  *httpCode = code2;
  return body2;
}

// TTS：POST JSON，响应是二进制音频，存入 out(ps_malloc)
static bool httpsPostBinary(const char *path, const String &body,
                            const char *fileExt) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20);
  if (!client.connect(API_HOST, 443)) { errMsg = netDiag("TTS-TCP"); return false; }
  client.printf("POST %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: M5StickS3-Translator/1.0\r\n"
                "Authorization: Bearer %s\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n",
                path, API_HOST, API_KEY, (int)body.length());
  client.print(body);
  if (!waitResponse(client, 30000, "TTS")) return false;
  String statusLine = readLine(client, 5000);
  if (!statusLine.startsWith("HTTP/") || statusLine.substring(9, 12).toInt() != 200) {
    Serial.printf("[tts] bad status: %s\r\n", statusLine.c_str());
    errMsg = "TTS st? " + statusLine.substring(0, 20);
    return false;
  }
  String contentType = "", transferEnc = "";
  size_t contentLen = 0; bool hasLen = false, chunked = false;
  while (client.connected()) {
    String h = readLine(client, 5000);
    h.trim();
    if (h.length() == 0) break;
    if (h.startsWith("Content-Type:")) contentType = h.substring(13), contentType.trim();
    if (h.startsWith("Transfer-Encoding:")) transferEnc = h.substring(18), transferEnc.trim();
    if (h.startsWith("Content-Length:")) { contentLen = h.substring(15).toInt(); hasLen = true; }
  }
  chunked = transferEnc.indexOf("chunked") >= 0;
  Serial.printf("[tts] type=%s len=%d chunked=%d\r\n", contentType.c_str(),
                (int)contentLen, (int)chunked);
  size_t cap = hasLen ? contentLen : 2 * 1024 * 1024;
  ttsAudio = (uint8_t *)ps_malloc(cap + 64);
  if (!ttsAudio) return false;
  ttsLen = 0;
  if (chunked) {
    // 解 chunked：行 = 十六进制长度，随后是等长数据，0 表示结束
    while (client.connected()) {
      String szLine = readLine(client, 5000);
      szLine.trim();
      if (szLine.isEmpty()) continue;
      size_t sz = strtol(szLine.c_str(), nullptr, 16);
      if (sz == 0) break;
      if (ttsLen + sz > cap) sz = cap - ttsLen; // 超容量截断
      uint32_t t0 = millis();
      while (sz && client.connected()) {
        if (client.available()) { ttsAudio[ttsLen++] = (uint8_t)client.read(); sz--; }
        else if (millis() - t0 > 15000) return false;
        else delay(1);
      }
      if (client.connected() && client.available()) { client.read(); client.read(); } // \r\n
    }
  } else {
    while (client.connected() && ttsLen < cap) {
      if (client.available()) ttsAudio[ttsLen++] = (uint8_t)client.read();
      else delay(1);
    }
  }
  if (fileExt) {} // 预留
  return ttsLen > 44;
}

// ---------- MP3 解码到 PSRAM ----------
class MemSink : public AudioOutput {
public:
  int16_t *buf = nullptr;
  size_t cap = 0, len = 0;
  uint32_t rate = 24000;
  bool begin() override { len = 0; return true; }
  bool stop() override { return true; }
  bool SetRate(int hz) override { rate = hz; return true; }
  bool ConsumeSample(int16_t sample[2]) override {
    if (len < cap) { buf[len++] = sample[0]; return true; }
    return false; // 满
  }
};

static bool decodeMp3(const uint8_t *mp3, size_t mp3Len) {
  size_t cap = 24000 * 2 * 120; // 估 120 秒 16bit
  playPcm = (int16_t *)ps_malloc(cap * sizeof(int16_t));
  if (!playPcm) return false;
  MemSink sink;
  sink.buf = playPcm;
  sink.cap = cap;
  AudioFileSourcePROGMEM *file = new AudioFileSourcePROGMEM(mp3, mp3Len);
  AudioGeneratorMP3 *gen = new AudioGeneratorMP3();
  gen->begin(file, &sink);
  uint32_t t0 = millis();
  while (gen->isRunning()) {
    if (!gen->loop()) break;
    if (millis() - t0 > 30000) break; // 保险
  }
  playSamples = sink.len;
  playRate = sink.rate ? sink.rate : 24000;
  bool ok = playSamples > 0;
  Serial.printf("[mp3] %d samples @%dHz (pcm %dKB)\r\n", (int)playSamples,
                (int)playRate, (int)(playSamples * 2 / 1024));
  gen->stop();
  delete gen;
  delete file;
  return ok;
}

// 解析 WAV：返回 data 偏移与格式；兼容直接 playRaw
static bool parseWav(const uint8_t *wav, size_t len, const int16_t **pcmOut,
                     size_t *samplesOut, uint32_t *rateOut) {
  if (len < 44 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) return false;
  size_t off = 12;
  uint32_t rate = 16000; uint16_t bits = 16, ch = 1;
  while (off + 8 <= len) {
    uint32_t sz; memcpy(&sz, wav + off + 4, 4);
    if (!memcmp(wav + off, "fmt ", 4)) {
      memcpy(&ch, wav + off + 10, 2);
      memcpy(&rate, wav + off + 12, 4);
      memcpy(&bits, wav + off + 22, 2);
    } else if (!memcmp(wav + off, "data", 4)) {
      if (bits != 16 || ch != 1) return false;
      *pcmOut = (const int16_t *)(wav + off + 8);
      *samplesOut = min(sz, (uint32_t)(len - off - 8)) / 2;
      *rateOut = rate;
      return true;
    }
    off += 8 + sz + (sz & 1);
  }
  return false;
}

// ---------- 录音 ----------
static void recReleased(void *, void *data, size_t samples) {
  // 麦克风任务回调：把完成块追加进 PSRAM
  if (!recPcm) return;
  size_t room = MAX_SAMPLES - recSamples;
  size_t n = min(room, samples);
  if (n) memcpy(recPcm + recSamples, data, n * sizeof(int16_t));
  recSamples += n;
}

static float chunkLevel(const int16_t *s, size_t n) {
  int32_t peak = 0;
  for (size_t i = 0; i < n; i++) peak = max(peak, (int32_t)abs(s[i]));
  return min(1.0f, peak / 12000.0f);
}

static void doRecord() {
  M5.Lcd.fillScreen(TFT_BLACK);
  recSamples = 0;
  recLevel = 0;
  recStartMs = millis();
  M5.Speaker.end();          // mic/speaker 互斥
  M5.Mic.setBufferReleaseCallback(nullptr, recReleased);
  M5.Mic.begin();
  uint32_t lastUi = 0;
  while (true) {
    M5.update();
    bool held = M5.BtnA.isPressed() && (millis() - recStartMs < MAX_SAMPLES * 1000ULL / SAMPLE_RATE - 500);
    if (!held) break;
    M5.Mic.record(chunkBuf, CHUNK, SAMPLE_RATE); // 阻塞节拍
    recLevel = 0.6f * recLevel + 0.4f * chunkLevel(chunkBuf, CHUNK);
    if (millis() - lastUi > 100) {
      lastUi = millis();
      M5.Lcd.fillScreen(TFT_BLACK);
      M5.Lcd.fillCircle(14, 12, 6, TFT_RED);
      M5.Lcd.setTextFont(2);
      M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
      M5.Lcd.setCursor(30, 5);
      M5.Lcd.printf("%.1fs", (millis() - recStartMs) / 1000.0f);
      M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
      M5.Lcd.setCursor(150, 5);
      M5.Lcd.print("release=send");
      int bw = (int)(220 * recLevel);
      M5.Lcd.fillRect(10, 50, 220, 24, TFT_DARKGREY);
      M5.Lcd.fillRect(10, 50, bw, 24, recLevel > 0.7f ? TFT_RED : TFT_GREEN);
      M5.Lcd.setTextFont(1);
      M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
      M5.Lcd.setCursor(10, 100);
      M5.Lcd.print("listening ...");
    }
    delay(1);
  }
  while (M5.Mic.isRecording()) M5.delay(1);
  M5.Mic.end();
  Serial.printf("[rec] %d samples (%.1fs)\r\n", (int)recSamples,
                recSamples / (float)SAMPLE_RATE);
}

// ---------- 三步流水线 ----------
static bool doAsr() {
  errMsg = "";
  int code = 0;
  String resp;
  for (int attempt = 0; attempt < 3; attempt++) {
    resp = httpsPostWav(ASR_PATH, recPcm, recSamples, &code);
    if (!resp.isEmpty() && code == 200) break;
    if (code >= 500 && attempt < 2) { // 瞬态服务端错误，重试
      Serial.printf("[asr] HTTP %d, retry %d\r\n", code, attempt + 1);
      drawBusy("识别重试中");
      delay(1500);
      continue;
    }
    break;
  }
  if (resp.isEmpty()) {
    if (errMsg.isEmpty()) errMsg = "ASR: empty resp";
    errMsg += String(" [") + (int)(recSamples * 100 / SAMPLE_RATE) / 100.0 + "s audio]";
    return false;
  }
  if (code != 200) { errMsg = "ASR HTTP " + String(code) + " " + resp.substring(0, 100); return false; }
  JsonDocument doc;
  if (deserializeJson(doc, resp)) { errMsg = "ASR: bad json"; return false; }
  srcText = String((const char *)(doc["text"] | ""));
  srcText.trim();
  Serial.printf("[asr] '%s'\r\n", srcText.c_str());
  if (srcText.isEmpty()) { errMsg = "no speech detected"; return false; }
  return true;
}

static bool doTranslate() {
  JsonDocument req;
  req["model"] = "Qwen/Qwen2.5-7B-Instruct";
  req["temperature"] = 0.1;
  req["max_tokens"] = 300;
  JsonArray msgs = req["messages"].to<JsonArray>();
  JsonObject sys = msgs.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = String("You are a translation engine. Translate the user's "
                          "text into natural ") + langCode(lang) +
                  ". Output ONLY the translation itself, no quotes, no notes.";
  JsonObject usr = msgs.add<JsonObject>();
  usr["role"] = "user";
  usr["content"] = srcText;
  String body;
  serializeJson(req, body);
  int code = 0;
  String resp = httpsPostJson(CHAT_PATH, body, &code);
  if (code != 200) { errMsg = "LLM HTTP " + String(code) + " " + resp.substring(0, 100); return false; }
  if (resp.isEmpty()) { errMsg = netDiag("LLM-TCP"); return false; }
  JsonDocument doc;
  if (deserializeJson(doc, resp)) { errMsg = "LLM: bad json"; return false; }
  dstText = String(doc["choices"][0]["message"]["content"] | "");
  dstText.trim();
  // 去掉模型偶尔输出的引号
  while (dstText.length() && (dstText[0] == '"' || dstText[0] == '\'')) dstText = dstText.substring(1);
  while (dstText.length() && (dstText[dstText.length()-1] == '"' || dstText[dstText.length()-1] == '\'')) dstText = dstText.substring(0, dstText.length()-1);
  Serial.printf("[llm] '%s'\r\n", dstText.c_str());
  return !dstText.isEmpty();
}

static bool doTtsFetch() {
  JsonDocument req;
  req["model"] = "FunAudioLLM/CosyVoice2-0.5B";
  req["input"] = dstText;
  req["voice"] = "FunAudioLLM/CosyVoice2-0.5B:alex";
  req["response_format"] = "wav";
  req["speed"] = 0.8; // 语速放慢
  String body;
  serializeJson(req, body);
  if (!httpsPostBinary(TTS_PATH, body, "wav")) { errMsg = "TTS failed"; return false; }
  return true;
}

static bool preparePlayback() {
  const int16_t *wavPcm; size_t wavN; uint32_t wavRate;
  if (parseWav(ttsAudio, ttsLen, &wavPcm, &wavN, &wavRate)) {
    playPcm = (int16_t *)ps_malloc(wavN * 2);
    memcpy(playPcm, wavPcm, wavN * 2);
    playSamples = wavN;
    playRate = wavRate;
    return true;
  }
  return decodeMp3(ttsAudio, ttsLen);
}

static void playTts() {
  if (playPcm && playSamples) {
    M5.Mic.end();
    M5.Speaker.begin();
    M5.Speaker.setVolume(255);
    M5.Speaker.playRaw(playPcm, playSamples, playRate, false, 1, 0);
    Serial.printf("[play] %d samples @%d\r\n", (int)playSamples, (int)playRate);
  }
}

// ---------- UI ----------
static void drawBusy(const char *zh) {
  M5.Lcd.fillScreen(TFT_BLACK);
  static const char *dots[] = {"/", "-", "\\", "|"};
  M5.Lcd.setFont(&fonts::efontCN_16_b);
  M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Lcd.setCursor(20, 40);
  M5.Lcd.printf("%s %s", zh, dots[(millis() / 200) % 4]);
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(20, 70);
  M5.Lcd.print("cloud ...");
}

static void drawIdle() {
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setTextFont(2);
  M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Lcd.setCursor(40, 8);
  M5.Lcd.print("Pocket Translator");
  // 目标语言大字
  if (lang == LANG_EN) {
    M5.Lcd.setFont(&fonts::Font4);
    M5.Lcd.setCursor(70, 34);
    M5.Lcd.print("CN > EN");
  } else {
    M5.Lcd.setFont(&fonts::efontJA_16_b);
    M5.Lcd.setCursor(70, 34);
    M5.Lcd.print("CN > JP");
  }
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Lcd.setCursor(30, 80);
  M5.Lcd.print("A:按住说话，松开翻译");
  M5.Lcd.setCursor(30, 98);
  M5.Lcd.print("B:切换 英/日 语言");
  int bat = M5.Power.getBatteryLevel();
  if (bat > 0) {
    M5.Lcd.setTextFont(1);
    M5.Lcd.setTextColor(bat <= 20 ? TFT_RED : TFT_DARKGREY, TFT_BLACK);
    M5.Lcd.setCursor(206, 122);
    M5.Lcd.printf("%d%%", bat);
  }
}

static void drawResult(bool speaking) {
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(4, 2);
  M5.Lcd.print("src:");
  M5.Lcd.setFont(&fonts::efontCN_12);
  drawWrapped(srcText.c_str(), 4, 14, 232, 3, TFT_LIGHTGREY, 14);
  M5.Lcd.drawFastHLine(4, 60, 232, TFT_DARKGREY);
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(4, 64);
  M5.Lcd.printf("dst (%s):", langCode(lang));
  if (lang == LANG_JA) {
    M5.Lcd.setFont(&fonts::efontJA_16_b);
    drawWrapped(dstText.c_str(), 4, 76, 232, 3,
                speaking ? TFT_GREEN : TFT_ORANGE, 18);
  } else {
    M5.Lcd.setFont(&fonts::Font4);
    drawWrapped(dstText.c_str(), 4, 76, 232, 3,
                speaking ? TFT_GREEN : TFT_ORANGE, 28);
  }
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(4, 124);
  M5.Lcd.print(speaking ? "speaking... A:replay B:back" : "A:replay B:back");
}

static void drawError() {
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_RED, TFT_BLACK);
  M5.Lcd.setCursor(10, 40);
  M5.Lcd.print("出错:");
  drawWrapped(errMsg.c_str(), 10, 58, 220, 4, TFT_ORANGE, 14);
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(4, 124);
  M5.Lcd.print(recSamples > MIN_SAMPLES ? "A:replay-rec B:continue" : "B:continue");
}

static void drawNoSecret() {
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_ORANGE, TFT_BLACK);
  M5.Lcd.setCursor(16, 30);
  M5.Lcd.print("请先配置 secrets.h:");
  M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Lcd.setCursor(16, 52);
  M5.Lcd.print("cp secrets.example.h secrets.h");
  M5.Lcd.setCursor(16, 70);
  M5.Lcd.print("填入 WiFi 与 API key 后重刷");
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(16, 92);
  M5.Lcd.print("key: https://open.bigmodel.cn");
}

static void checkRotation() {
  float ax, ay, az;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return;
  float g = AX_SIGN * ax;
  uint8_t want;
  if (g < -0.35f) want = 3;
  else if (g > 0.35f) want = 1;
  else return;
  if (want != rotCandidate) { rotCandidate = want; rotCandidateSince = millis(); return; }
  if (want != rotCur && millis() - rotCandidateSince > 500) {
    rotCur = want;
    M5.Lcd.setRotation(rotCur);
    if (state == ST_IDLE) drawIdle();
    else if (state == ST_RESULT) drawResult(M5.Speaker.isPlaying());
  }
}

// ---------- 生命周期 ----------
void setup() {
  auto cfg = M5.config();
  cfg.internal_mic = true;
  cfg.internal_spk = true;
  M5.begin(cfg);
  M5.Lcd.setRotation(rotCur);
  M5.Lcd.setBrightness(100);

  Serial.begin(115200);
  Serial.printf("\r\n=== Pocket Translator for M5StickS3 v1 ===\r\n");

#ifdef SECRETS_MISSING
  setState(ST_NOSECRET);
  drawNoSecret();
  return;
#endif

  Preferences prefs;
  prefs.begin("trans", true);
  lang = (Lang)(prefs.getUChar("lang", LANG_EN) & 1);
  prefs.end();

  recPcm = (int16_t *)ps_malloc(MAX_SAMPLES * sizeof(int16_t));
  if (!recPcm) {
    errMsg = "PSRAM alloc failed";
    setState(ST_ERR); drawError(); return;
  }
  memset(recPcm, 0, MAX_SAMPLES * sizeof(int16_t));

  // 连 WiFi
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Lcd.setCursor(40, 50);
  M5.Lcd.print("连接 WiFi ...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) M5.delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    errMsg = String("WiFi connect failed: ") + WIFI_SSID;
    setState(ST_ERR); drawError(); return;
  }
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  Serial.printf("[wifi] %s ip=%s\r\n", WIFI_SSID, WiFi.localIP().toString().c_str());
  Serial.printf("[wifi] diag: %s\r\n", netDiag("boot").c_str());
  setState(ST_IDLE);
  drawIdle();
}

void loop() {
  M5.update();

  if (millis() % 150 < 10) checkRotation();

  switch (state) {
  case ST_NOSECRET:
  case ST_BOOT:
    delay(100);
    break;

  case ST_ERR:
    // A：回放刚才的录音（亲耳验证录音质量）
    if (M5.BtnA.wasClicked() && recSamples > MIN_SAMPLES) {
      M5.Mic.end();
      M5.Speaker.begin();
      M5.Speaker.setVolume(255);
      M5.Speaker.playRaw(recPcm, recSamples, SAMPLE_RATE, false, 1, 0);
      while (M5.Speaker.isPlaying()) { M5.update(); M5.delay(20); }
      M5.Speaker.end();
      drawError();
    }
    // B：WiFi 错误重试连接；其他错误回主界面
    if (M5.BtnB.wasClicked()) {
      if (errMsg.startsWith("WiFi")) {
        if (recPcm) { free(recPcm); recPcm = nullptr; }
        setup(); return;
      }
      setState(ST_IDLE); drawIdle();
    }
    break;

  case ST_IDLE:
    if (M5.BtnB.wasClicked()) {
      lang = (Lang)(lang ^ 1);
      Preferences prefs;
      prefs.begin("trans", false);
      prefs.putUChar("lang", lang);
      prefs.end();
      drawIdle();
      M5.Speaker.tone(1800, 40);
    }
    if (M5.BtnA.isPressed()) {
      setState(ST_REC);
      doRecord();
      if (recSamples < MIN_SAMPLES) {
        Serial.println("[rec] too short, ignore");
        setState(ST_IDLE); drawIdle();
      } else {
        setState(ST_ASR);
      }
    }
    break;

  case ST_ASR: {
    drawBusy("语音识别中");
    bool ok = doAsr();
    if (!ok) { setState(ST_ERR); drawError(); break; }
    setState(ST_LLM);
    break;
  }

  case ST_LLM: {
    drawBusy("翻译中");
    bool ok = doTranslate();
    if (!ok) { setState(ST_ERR); drawError(); break; }
    setState(ST_TTS);
    break;
  }

  case ST_TTS: {
    drawBusy("合成语音中");
    bool ok = doTtsFetch() && preparePlayback();
    if (!ok) {
      // TTS 失败不致命：仍显示译文
      Serial.println("[tts] failed, show text only");
      setState(ST_RESULT); drawResult(false);
      resultShownMs = millis();
      break;
    }
    setState(ST_PLAY);
    break;
  }

  case ST_PLAY: {
    drawResult(true);
    playTts();
    while (M5.Speaker.isPlaying()) {
      M5.update();
      M5.delay(20);
    }
    Serial.println("[play] done");
    M5.Speaker.end();
    setState(ST_RESULT);
    drawResult(false);
    resultShownMs = millis();
    break;
  }

  case ST_RESULT:
    if (M5.BtnA.wasClicked()) {
      // 重播
      M5.Mic.end();
      M5.Speaker.begin();
      M5.Speaker.setVolume(255);
      M5.Speaker.playRaw(playPcm, playSamples, playRate, false, 1, 0);
      drawResult(true);
      while (M5.Speaker.isPlaying()) { M5.update(); M5.delay(20); }
      drawResult(false);
      resultShownMs = millis();
    }
    if (M5.BtnB.wasClicked() || millis() - resultShownMs > 30000) {
      M5.Speaker.end();
      setState(ST_IDLE);
      drawIdle();
    }
    break;
  }

  delay(10);
}
