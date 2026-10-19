#!/usr/bin/env bash
# Интеграционный тест каскада по настоящему HTTP:
#   ИИ №1 (Gemini) отвечает 429 RESOURCE_EXHAUSTED -> роутер переключается на ИИ №2 (OpenAI-формат).
# Использование: tests/integration_test.sh path/to/infinity-router
set -euo pipefail
BIN=${1:-./build/infinity-router}
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
MOCK_PORT=18999
PORT=18080
PY=$(command -v python3 || command -v python)

cleanup() { kill ${MOCK_PID:-} ${SRV_PID:-} 2>/dev/null || true; rm -rf "$TMP"; }
trap cleanup EXIT

cat > "$TMP/config.json" <<EOF
{
  "port": $PORT, "db_path": "$TMP/test.db", "timeout_ms": 5000,
  "cascade": [
    { "id": "gemini", "label": "Gemini", "provider": "gemini", "model": "gemini-2.5-flash",
      "base_url": "http://127.0.0.1:$MOCK_PORT/gemini", "key_name": "Google AI Studio", "tier": "free" },
    { "id": "gpt-4o", "label": "GPT-4o", "provider": "openai", "model": "gpt-4o",
      "base_url": "http://127.0.0.1:$MOCK_PORT/openai", "key_name": "OpenAI", "tier": "paid" },
    { "id": "claude", "label": "Claude", "provider": "anthropic", "model": "claude-sonnet-5-5",
      "base_url": "http://127.0.0.1:$MOCK_PORT/anthropic", "api_key_env": "IT_ANTHROPIC_KEY", "tier": "paid" }
  ]
}
EOF
# Модель из каталога: попадает в каскад автоматически, как только для её провайдера введён ключ
cat > "$TMP/catalog.json" <<EOF
{ "models": [ { "id": "cat-mock", "label": "Каталог", "provider": "openai", "model": "m",
  "base_url": "http://127.0.0.1:$MOCK_PORT/catalog", "key_name": "MockCat", "tier": "free", "priority": 2, "limits": { "rpd": 1000 } } ] }
EOF

"$PY" "$HERE/mock_providers.py" $MOCK_PORT & MOCK_PID=$!
for i in $(seq 1 50); do curl -s -o /dev/null -X POST "http://127.0.0.1:$MOCK_PORT/ping" -d "{}" && break; sleep 0.1; done
IT_ANTHROPIC_KEY=a "$BIN" --config "$TMP/config.json" --env /dev/null 2> "$TMP/router.log" & SRV_PID=$!
for i in $(seq 1 50); do curl -s "http://127.0.0.1:$PORT/api/health" >/dev/null && break; sleep 0.1; done

fail() { echo "FAIL: $*"; echo "--- лог роутера:"; cat "$TMP/router.log"; exit 1; }
post() { curl -s -X POST "http://127.0.0.1:$PORT/api/chat" -H 'Content-Type: application/json' -d "$1"; }

# Ключи Gemini и OpenAI — как из «Настроек» сайта; Claude — из окружения (.env)
K=$(curl -s -X POST "http://127.0.0.1:$PORT/api/keys/bulk" -H 'Content-Type: application/json' -H 'Origin: http://localhost:4200' \
  -d '{"keys":{"Google AI Studio":"AIza-test-1111","OpenAI":"sk-test-2222"}}')
echo "$K" | grep -q '"saved":2' || fail "ключи не сохранились: $K"
KL=$(curl -s "http://127.0.0.1:$PORT/api/keys")
echo "$KL" | grep -q '"provider":"OpenAI","set":true,"source":"settings","hint":"…2222"' || fail "GET /api/keys: $KL"
echo "$KL" | grep -q '"provider":"Anthropic","set":true,"source":"env"' || fail "Anthropic должен браться из .env: $KL"
echo "$KL" | grep -q 'sk-test-2222' && fail "ключ целиком не должен возвращаться"
EVIL=$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/api/keys" -H 'Origin: https://evil.example' -d '{"provider":"OpenAI","key":"stolen"}')
[ "$EVIL" = "403" ] || fail "запрос с чужого сайта должен быть отклонён, получено $EVIL"

R1=$(post '{"messages":[{"role":"user","content":"Привет, кто ответит?"}]}')
echo "Ответ 1: $R1"
echo "$R1" | grep -q '"badge":"Ответил: GPT-4o"' || fail "ожидался бэйдж GPT-4o"
echo "$R1" | grep -q '"switched":true' || fail "ожидалось switched=true"
echo "$R1" | grep -q '"status":"quota","limit":true,"http_status":429' || fail "ожидалась попытка Gemini с 429/quota"
grep -q "Основной лимит исчерпан" "$TMP/router.log" || fail "нет записи о переключении в логе"
grep -q "Gemini на паузе 41 с" "$TMP/router.log" || fail "пауза должна браться из retryDelay (41s)"

R2=$(post '{"messages":[{"role":"user","content":"Ещё раз"}]}')
echo "$R2" | grep -q '"status":"cooldown"' || fail "Gemini должен быть на паузе и не вызываться"

R3=$(post '{"messages":[{"role":"user","content":"Закреплённая модель"}],"model":"claude"}')
echo "$R3" | grep -q '"badge":"Ответил: Claude"' || fail "закреплённая модель Claude должна ответить первой"

M=$(curl -s "http://127.0.0.1:$PORT/api/models")
echo "$M" | grep -q '"id":"gemini"' || fail "/api/models без gemini"

echo "$M" | grep -q '"id":"cat-mock"' && fail "модель каталога без ключа не должна попадать в каскад"

# --- Настройки ---
SET=$(curl -s "http://127.0.0.1:$PORT/api/settings")
echo "$SET" | grep -q '"threshold":90' || fail "GET /api/settings: $SET"
BAD=$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/api/settings" -H 'Origin: https://evil.example' -d '{"autoSwitch":false}')
[ "$BAD" = "403" ] || fail "настройки с чужого сайта должны отклоняться, получено $BAD"
setp() { curl -s -X POST "http://127.0.0.1:$PORT/api/settings" -H 'Content-Type: application/json' -H 'Origin: http://localhost:4200' -d "$1"; }

# Ввели ключ каталожного провайдера — модель сразу в каскаде, без перезапуска
curl -s -X POST "http://127.0.0.1:$PORT/api/keys" -H 'Origin: http://localhost:4200' -d '{"provider":"MockCat","key":"cat-key-3333"}' >/dev/null
M=$(curl -s "http://127.0.0.1:$PORT/api/models")
echo "$M" | grep -q '"id":"cat-mock"[^}]*"auto":true' || fail "модель каталога с ключом должна появиться: $M"
R4=$(post '{"messages":[{"role":"user","content":"a"},{"role":"assistant","content":"b"},{"role":"user","content":"c"}]}')
echo "$R4" | grep -q '"badge":"Ответил: Каталог"' || fail "ожидался ответ модели каталога: $R4"
echo "$R4" | grep -q 'Каталог: 3 сообщ' || fail "контекст должен передаваться целиком: $R4"

# Выключили модель на странице «Модели» — каскад её обходит
setp '{"disabled":["cat-mock"]}' | grep -q '"ok":true' || fail "disabled не сохранился"
R5=$(post '{"messages":[{"role":"user","content":"d"}]}')
echo "$R5" | grep -q '"badge":"Ответил: GPT-4o"' || fail "выключенная модель не должна отвечать: $R5"
setp '{"disabled":[]}' >/dev/null

# Шифрование ключей: в базе — только шифротекст, роутер по-прежнему работает
setp '{"encryptKeys":true}' | grep -q '"encryptKeys":true' || fail "encryptKeys не включилось"
RAW=$("$PY" -c "import sqlite3,sys; print([r[0] for r in sqlite3.connect(sys.argv[1]).execute('select api_key from api_keys')])" "$TMP/test.db")
echo "$RAW" | grep -q 'cat-key-3333\|sk-test-2222' && fail "ключи в базе должны быть зашифрованы: $RAW"
echo "$RAW" | grep -q 'enc:v1:' || fail "нет шифротекста: $RAW"
[ -s "$TMP/master.key" ] || fail "мастер-ключ не создан"
R6=$(post '{"messages":[{"role":"user","content":"e"}]}')
echo "$R6" | grep -q '"badge":"Ответил: Каталог"' || fail "после шифрования ключи должны расшифровываться: $R6"

echo "--- лог роутера:"; cat "$TMP/router.log"
echo "OK: интеграционный тест каскада пройден"
