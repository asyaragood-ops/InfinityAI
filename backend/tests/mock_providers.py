#!/usr/bin/env python3
"""Имитатор API провайдеров для интеграционного теста каскада (только стандартная библиотека).

  POST /gemini/models/<model>:generateContent  -> 429 RESOURCE_EXHAUSTED (как бесплатный тир Gemini)
  POST /gemini-ok/models/<model>:generateContent -> 200 в формате Gemini
  POST /openai/chat/completions                  -> 200 в формате OpenAI
  POST /anthropic                                -> 200 в формате Anthropic Messages

Запуск: python3 mock_providers.py 18999
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

GEMINI_429 = {
    "error": {
        "code": 429,
        "message": "You exceeded your current quota, please check your plan and billing details.",
        "status": "RESOURCE_EXHAUSTED",
        "details": [
            {"@type": "type.googleapis.com/google.rpc.QuotaFailure",
             "violations": [{"quotaId": "GenerateRequestsPerMinutePerProjectPerModel-FreeTier"}]},
            {"@type": "type.googleapis.com/google.rpc.RetryInfo", "retryDelay": "41s"},
        ],
    }
}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):  # тихо
        pass

    def reply(self, code, obj):
        data = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        if self.path.startswith("/gemini/"):
            if self.headers.get("x-goog-api-key") != "AIza-test-1111":  # ключ из «Настроек» сайта
                return self.reply(400, {"error": {"code": 400, "message": "API key not valid."}})
            return self.reply(429, GEMINI_429)
        if self.path.startswith("/gemini-ok/"):
            text = body["contents"][-1]["parts"][0]["text"]
            return self.reply(200, {"candidates": [{"content": {"parts": [{"text": f"Gemini: {text}"}]}}],
                                    "usageMetadata": {"promptTokenCount": 5, "candidatesTokenCount": 7}})
        if self.path == "/openai/chat/completions":
            if self.headers.get("Authorization") != "Bearer sk-test-2222":  # ключ из «Настроек» сайта
                return self.reply(401, {"error": {"message": "Incorrect API key provided"}})
            last = body["messages"][-1]["content"]
            return self.reply(200, {"choices": [{"message": {"role": "assistant", "content": f"GPT-4o отвечает: {last}"}}],
                                    "usage": {"prompt_tokens": 11, "completion_tokens": 9}})
        if self.path == "/catalog/chat/completions":
            if self.headers.get("Authorization") != "Bearer cat-key-3333":  # ключ каталожного провайдера из «Настроек»
                return self.reply(401, {"error": {"message": "Incorrect API key provided"}})
            return self.reply(200, {"choices": [{"message": {"role": "assistant", "content": f"Каталог: {len(body['messages'])} сообщ."}}]})
        if self.path == "/anthropic":
            if self.headers.get("x-api-key") != "a" or self.headers.get("anthropic-version") != "2023-06-01":  # ключ из .env
                return self.reply(401, {"type": "error", "error": {"type": "authentication_error", "message": "invalid x-api-key"}})
            return self.reply(200, {"content": [{"type": "text", "text": "Claude: ok"}],
                                    "usage": {"input_tokens": 3, "output_tokens": 2}})
        self.reply(404, {"error": {"message": "not found"}})


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18999
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
