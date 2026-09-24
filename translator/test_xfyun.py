#!/usr/bin/env python3
"""讯飞流式语音翻译（itrans）协议验证脚本

用法:
  python3 test_xfyun.py <APPID> <APIKey> <APISecret> <audio.wav> [en|ja]

验证内容：WS 鉴权签名、首帧业务参数、分帧上传、响应 JSON 结构。
会把服务端每条消息原样打印，用来锁定字段名后写入固件。
"""
import base64
import hashlib
import hmac
import json
import sys
import wave
from datetime import datetime, timezone
from urllib.parse import urlencode, quote

import websocket  # pip install websocket-client

HOST = "ist-api.xfyun.cn"
PATH = "/v2/ist"
FRAME_BYTES = 1280          # 40ms @16k16bit
FRAME_INTERVAL = 0.04

LANG_MAP = {"en": "en", "ja": "ja"}


def build_auth(api_key, api_secret):
    date = datetime.now(timezone.utc).strftime("%a, %d %b %Y %H:%M:%S GMT")
    signature_origin = f"host: {HOST}\ndate: {date}\nGET {PATH} HTTP/1.1"
    signature = base64.b64encode(
        hmac.new(api_secret.encode(), signature_origin.encode(),
                 hashlib.sha256).digest()).decode()
    authorization_origin = (f'api_key="{api_key}", algorithm="hmac-sha256", '
                            f'headers="host date request-line", '
                            f'signature="{signature}"')
    authorization = base64.b64encode(authorization_origin.encode()).decode()
    return (f"wss://{HOST}{PATH}?{urlencode({'authorization': authorization})}"
            f"&date={quote(date)}&host={HOST}")


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        sys.exit(1)
    appid, api_key, api_secret, wav_path = sys.argv[1:5]
    to = LANG_MAP.get(sys.argv[5] if len(sys.argv) > 5 else "en", "en")

    w = wave.open(wav_path)
    assert w.getframerate() == 16000 and w.getnchannels() == 1, "需 16k 单声道 wav"
    pcm = w.readframes(w.getnframes())

    url = build_auth(api_key, api_secret)
    print(f"[ws] connecting {HOST}{PATH} -> {to}")
    ws = websocket.create_connection(url, timeout=15)
    print("[ws] connected")

    # 首帧：业务参数（from=cn 固定；to 跟随目标语言）
    first = {
        "common": {"app_id": appid},
        "business": {"from": "cn", "to": to, "domain": "iner"},
        "data": {"status": 0, "format": "audio/L16;rate=16000",
                 "encoding": "raw", "audio": ""},
    }
    ws.send(json.dumps(first))

    import time
    frames = [pcm[i:i + FRAME_BYTES] for i in range(0, len(pcm), FRAME_BYTES)]
    for i, fr in enumerate(frames):
        msg = {
            "data": {
                "status": 2 if i == len(frames) - 1 else 1,
                "format": "audio/L16;rate=16000",
                "encoding": "raw",
                "audio": base64.b64encode(fr).decode(),
            }
        }
        ws.send(json.dumps(msg))
        # 边发边收，模拟真实流式
        try:
            ws.settimeout(0.01)
            while True:
                r = ws.recv()
                print(f"[recv@{i}] {r[:300]}")
        except Exception:
            pass
        time.sleep(FRAME_INTERVAL)
    ws.send(json.dumps({"data": {"status": 2}}))

    ws.settimeout(10)
    final_text, final_trans = "", ""
    while True:
        try:
            r = ws.recv()
        except Exception:
            break
        print(f"[recv-end] {r[:400]}")
        try:
            d = json.loads(r)
            if d.get("code") != 0:
                print("!! 错误码:", d.get("code"), d.get("message"))
                break
            res = d.get("data", {}).get("result", {})
            # 字段以实测为准，常见组合都尝试读取
            t = res.get("trans_result") or {}
            if isinstance(t, dict):
                final_trans = t.get("dst") or final_trans
                final_text = t.get("src") or final_text
        except Exception:
            pass
        if d.get("data", {}).get("status") == 2:
            break
    ws.close()
    print(f"\n原文: {final_text}\n译文({to}): {final_trans}")


if __name__ == "__main__":
    main()
