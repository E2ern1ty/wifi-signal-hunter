#!/usr/bin/env python3
"""验证翻译器三步云端链路（硅基流动）：ASR → LLM 翻译 → TTS

用法:
  python3 test_api.py <api-key> <audio.wav> [en|ja]

已验证可用模型：
  ASR : FunAudioLLM/SenseVoiceSmall  (免费)
  LLM : Qwen/Qwen2.5-7B-Instruct     (免费)
  TTS : FunAudioLLM/CosyVoice2-0.5B  (WAV, chunked)
"""
import io
import json
import sys
import wave
import urllib.request
import urllib.error

HOST = "api.siliconflow.cn"
ASR_PATH = "/v1/audio/transcriptions"
CHAT_PATH = "/v1/chat/completions"
TTS_PATH = "/v1/audio/speech"
BOUNDARY = "----StickS3Boundary7f3a9c"
ASR_MODEL = "FunAudioLLM/SenseVoiceSmall"
LLM_MODEL = "Qwen/Qwen2.5-7B-Instruct"
TTS_MODEL = "FunAudioLLM/CosyVoice2-0.5B"
TTS_VOICE = "FunAudioLLM/CosyVoice2-0.5B:alex"


def post(path, data, headers):
    req = urllib.request.Request(f"https://{HOST}{path}", data=data,
                                 headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()


def asr(key, wav_path):
    raw = open(wav_path, "rb").read()
    w = wave.open(io.BytesIO(raw))
    if w.getframerate() != 16000 or w.getnchannels() != 1 or w.getsampwidth() != 2:
        print(f"[warn] wav 是 {w.getframerate()}Hz/{w.getnchannels()}ch/"
              f"{w.getsampwidth()*8}bit，固件固定 16k/mono/16bit")
    body = io.BytesIO()
    body.write(f"--{BOUNDARY}\r\n".encode())
    body.write(f'Content-Disposition: form-data; name="model"\r\n\r\n{ASR_MODEL}\r\n'.encode())
    body.write(f"--{BOUNDARY}\r\n".encode())
    body.write(b'Content-Disposition: form-data; name="file"; filename="a.wav"\r\n'
               b"Content-Type: audio/wav\r\n\r\n")
    body.write(raw)
    body.write(f"\r\n--{BOUNDARY}--\r\n".encode())
    code, _, resp = post(ASR_PATH, body.getvalue(), {
        "Authorization": f"Bearer {key}",
        "Content-Type": f"multipart/form-data; boundary={BOUNDARY}",
    })
    print(f"[asr] HTTP {code}: {resp[:300]!r}")
    if code != 200:
        sys.exit(1)
    text = json.loads(resp)["text"]
    print(f"[asr] 识别: {text}")
    return text


def translate(key, text, target):
    payload = json.dumps({
        "model": LLM_MODEL, "temperature": 0.1, "max_tokens": 300,
        "messages": [
            {"role": "system",
             "content": f"You are a translation engine. Translate the user's "
                        f"text into natural {target}. Output ONLY the "
                        f"translation itself, no quotes, no notes."},
            {"role": "user", "content": text},
        ],
    }, ensure_ascii=False).encode()
    code, _, resp = post(CHAT_PATH, payload, {
        "Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    print(f"[llm] HTTP {code}: {resp[:300]!r}")
    if code != 200:
        sys.exit(1)
    out = json.loads(resp)["choices"][0]["message"]["content"].strip()
    print(f"[llm] 译文({target}): {out}")
    return out


def tts(key, text):
    payload = json.dumps({
        "model": TTS_MODEL, "input": text, "voice": TTS_VOICE,
        "response_format": "wav",
    }, ensure_ascii=False).encode()
    code, hdrs, resp = post(TTS_PATH, payload, {
        "Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    print(f"[tts] HTTP {code} type={hdrs.get('Content-Type')} "
          f"len={hdrs.get('Content-Length')} enc={hdrs.get('Transfer-Encoding')}")
    if code != 200:
        print(f"[tts] body: {resp[:300]!r}")
        sys.exit(1)
    open("out.wav", "wb").write(resp)
    print(f"[tts] 已保存 out.wav ({len(resp)} 字节) — afplay out.wav 试听")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    key = sys.argv[1]
    wav = sys.argv[2]
    target = {"en": "English", "ja": "Japanese"}.get(
        sys.argv[3] if len(sys.argv) > 3 else "en", "English")
    tts(key, translate(key, asr(key, wav), target))
    print("全链路 OK ✓")
