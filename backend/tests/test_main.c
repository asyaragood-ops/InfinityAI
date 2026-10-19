/* Тесты ядра роутера: JSON, классификация ошибок, паузы, каскад, локальные лимиты, API-обработчик.
 * Сеть не нужна — используются mock-провайдеры. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "crypto.h"
#include "settings.h"
#include "json.h"
#include "log.h"
#include "platform.h"
#include "providers/provider.h"
#include "quota.h"
#include "router.h"
#include "server.h"
#include "strbuf.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (cond) g_pass++;                                                         \
        else { g_fail++; fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static void test_json(void) {
    char err[128];
    json *j = json_parse("{\"a\":[1,2,{\"b\":\"при\\u0432\\u0435\\u0442 \\ud83d\\ude00\"}],\"t\":true,\"n\":null,\"x\":-1.5e2}", err, sizeof err);
    CHECK(j != NULL);
    CHECK(json_num(json_path(j, "a.1"), 0) == 2);
    CHECK(!strcmp(json_str(json_path(j, "a.2.b"), ""), "привет 😀"));
    CHECK(json_bool(json_get(j, "t"), 0) == 1);
    CHECK(json_get(j, "n")->type == JSON_NULL);
    CHECK(json_num(json_get(j, "x"), 0) == -150);
    CHECK(json_path(j, "a.9") == NULL);
    json_free(j);
    CHECK(json_parse("{\"a\":1,}", err, sizeof err) == NULL);
    CHECK(json_parse("[1,2", err, sizeof err) == NULL);
    CHECK(json_parse("{} x", err, sizeof err) == NULL);

    strbuf sb;
    sb_init(&sb);
    sb_json_str(&sb, "кавычка \" слэш \\ перевод\nстроки\t\x01");
    json *r = json_parse(sb.data, err, sizeof err);
    CHECK(r && !strcmp(r->str, "кавычка \" слэш \\ перевод\nстроки\t\x01"));
    json_free(r);
    sb_free(&sb);
}

static void test_classify(void) {
    CHECK(classify_http_error(429, "{\"error\":{\"message\":\"Rate limit reached for requests\"}}") == RES_RATE_LIMIT);
    CHECK(classify_http_error(429, "{\"error\":{\"status\":\"RESOURCE_EXHAUSTED\",\"message\":\"You exceeded your current quota\"}}") == RES_QUOTA);
    CHECK(classify_http_error(429, "{\"error\":{\"code\":\"insufficient_quota\"}}") == RES_QUOTA);
    CHECK(classify_http_error(402, "") == RES_QUOTA);
    CHECK(classify_http_error(401, "") == RES_AUTH);
    CHECK(classify_http_error(400, "{\"error\":{\"message\":\"API key not valid\"}}") == RES_AUTH);
    CHECK(classify_http_error(400, "{\"error\":{\"message\":\"bad\"}}") == RES_BAD_REQUEST);
    CHECK(classify_http_error(529, "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\"}}") == RES_SERVER);
    CHECK(classify_http_error(0, "") == RES_NETWORK);

    call_result r;
    memset(&r, 0, sizeof r);
    fill_http_error(&r, 429,
                    "{\"error\":{\"code\":429,\"message\":\"You exceeded your current quota, please check your plan and billing details\","
                    "\"status\":\"RESOURCE_EXHAUSTED\",\"details\":[{\"@type\":\"type.googleapis.com/google.rpc.RetryInfo\",\"retryDelay\":\"34s\"}]}}",
                    0, NULL);
    CHECK(r.kind == RES_QUOTA);
    CHECK(r.retry_after_s == 34);
    CHECK(r.limit_billing == 0); /* «billing details» у Google — не признак кончившихся денег */
    CHECK(strstr(r.error, "HTTP 429") != NULL);
}

static void test_cooldown(void) {
    int64_t now = 1792370000; /* фиксированное время */
    call_result r;
    memset(&r, 0, sizeof r);
    r.kind = RES_RATE_LIMIT;
    CHECK(router_cooldown_seconds(&r, now) == 60);
    r.retry_after_s = 12;
    CHECK(router_cooldown_seconds(&r, now) == 12);
    memset(&r, 0, sizeof r);
    r.kind = RES_QUOTA;
    r.limit_daily = 1;
    CHECK(router_cooldown_seconds(&r, now) == (long)(quota_next_day(now) - now));
    r.limit_daily = 0;
    r.limit_billing = 1;
    CHECK(router_cooldown_seconds(&r, now) == 3600);
    r.limit_billing = 0;
    CHECK(router_cooldown_seconds(&r, now) == 60);
    r.kind = RES_BAD_REQUEST;
    CHECK(router_cooldown_seconds(&r, now) == 0);
}

static model_cfg mk(const char *id, const char *label, const char *behavior) {
    model_cfg m;
    memset(&m, 0, sizeof m);
    xstrlcpy(m.id, id, sizeof m.id);
    xstrlcpy(m.label, label, sizeof m.label);
    xstrlcpy(m.provider, "mock", sizeof m.provider);
    xstrlcpy(m.tier, "free", sizeof m.tier);
    xstrlcpy(m.mock_behavior, behavior, sizeof m.mock_behavior);
    m.enabled = 1;
    m.max_tokens = 100;
    return m;
}

static app_config mkcfg(model_cfg *models, size_t n) {
    app_config c;
    memset(&c, 0, sizeof c);
    c.models = models;
    c.nmodels = n;
    c.max_history = 20;
    c.timeout_ms = 1000;
    xstrlcpy(c.cors_origin, "*", sizeof c.cors_origin);
    return c;
}

static const chat_msg HELLO[] = { { "user", "Привет" } };

static void test_cascade(quota_db *q) {
    /* ИИ №1 упирается в квоту → отвечает ИИ №2 */
    model_cfg ms[2] = { mk("gemini", "Gemini", "quota"), mk("gpt", "GPT-4o", "ok") };
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    router_reply rep;
    router_chat(&rt, HELLO, 1, "auto", NULL, &rep);
    CHECK(rep.ok);
    CHECK(rep.model == &ms[1]);
    CHECK(rep.switched == 1);
    CHECK(rep.nattempts == 2);
    CHECK(rep.attempts[0].kind == RES_QUOTA && rep.attempts[0].http_status == 429);
    CHECK(rep.attempts[1].kind == RES_OK);
    char *js = router_reply_json(&rep);
    json *j = json_parse(js, NULL, 0);
    CHECK(j && !strcmp(json_str(json_get(j, "badge"), ""), "Ответил: GPT-4o"));
    CHECK(!strcmp(json_str(json_path(j, "model.id"), ""), "gpt"));
    CHECK(json_bool(json_get(j, "switched"), 0) == 1);
    CHECK(!strcmp(json_str(json_path(j, "attempts.0.status"), ""), "quota"));
    CHECK(json_bool(json_path(j, "attempts.0.limit"), 0) == 1);
    json_free(j);
    free(js);
    router_reply_free(&rep);

    /* второй запрос: Gemini на паузе — даже не вызывается */
    router_chat(&rt, HELLO, 1, "auto", NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1]);
    CHECK(rep.attempts[0].kind == RES_COOLDOWN);
    router_reply_free(&rep);

    /* после сброса Gemini снова вызывается (и снова упирается в квоту) */
    quota_reset(q, "gemini");
    router_chat(&rt, HELLO, 1, "auto", NULL, &rep);
    CHECK(rep.attempts[0].kind == RES_QUOTA);
    router_reply_free(&rep);
    quota_reset(q, "gemini");
}

static void set_settings(quota_db *q, const char *js) {
    char err[200];
    if (settings_apply_json(q, js, err, sizeof err) != 0) fprintf(stderr, "  settings: %s\n", err);
}
#define DEFAULTS "{\"autoSwitch\":true,\"threshold\":90,\"strategy\":\"free\",\"keepContext\":true,\"notifySwitch\":true,\"encryptKeys\":false,\"disabled\":[]}"

static void test_local_limit(quota_db *q) {
    /* «дешевле» — строго по порядку конфига: «свободнее» сам обошёл бы исчерпанную модель */
    set_settings(q, "{\"threshold\":100,\"strategy\":\"cheap\"}");
    model_cfg ms[2] = { mk("free1", "Free", "ok"), mk("paid", "Paid", "ok") };
    ms[0].limits.rpd = 2;
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    router_reply rep;
    for (int i = 0; i < 2; i++) {
        router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
        CHECK(rep.ok && rep.model == &ms[0] && !rep.switched);
        router_reply_free(&rep);
    }
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1] && rep.switched);
    CHECK(rep.attempts[0].kind == RES_LOCAL_LIMIT);
    router_reply_free(&rep);
    set_settings(q, DEFAULTS);
}

static void test_errors_and_order(quota_db *q) {
    /* сеть/5xx/неверный ключ тоже ведут к резерву; ключ не задан — модель пропускается */
    model_cfg ms[5] = { mk("net", "Net", "network"), mk("srv", "Srv", "server_error"), mk("auth", "Auth", "auth_error"),
                        mk("nokey", "NoKey", "ok"), mk("last", "Last", "ok") };
    xstrlcpy(ms[3].provider, "openai", sizeof ms[3].provider);
    xstrlcpy(ms[3].api_key_env, "IAI_TEST_UNSET_KEY_42", sizeof ms[3].api_key_env);
    app_config cfg = mkcfg(ms, 5);
    router rt = { &cfg, q };
    router_reply rep;
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[4]);
    CHECK(rep.nattempts == 5);
    CHECK(rep.attempts[0].kind == RES_NETWORK);
    CHECK(rep.attempts[1].kind == RES_SERVER);
    CHECK(rep.attempts[2].kind == RES_AUTH);
    CHECK(rep.attempts[3].kind == RES_NOT_CONFIGURED);
    router_reply_free(&rep);
    for (int i = 0; i < 3; i++) quota_reset(q, ms[i].id);

    /* закреплённая модель идёт первой; навык поднимает подходящие модели */
    model_cfg ms2[3] = { mk("a", "A", "ok"), mk("b", "B", "ok"), mk("c", "C", "ok") };
    xstrlcpy(ms2[2].skills[0], "code", sizeof ms2[2].skills[0]);
    ms2[2].nskills = 1;
    xstrlcpy(ms2[0].skills[0], "text", sizeof ms2[0].skills[0]);
    ms2[0].nskills = 1;
    app_config cfg2 = mkcfg(ms2, 3);
    router rt2 = { &cfg2, q };
    router_chat(&rt2, HELLO, 1, "b", NULL, &rep);
    CHECK(rep.ok && rep.model == &ms2[1] && !rep.switched);
    router_reply_free(&rep);
    router_chat(&rt2, HELLO, 1, "auto", "code", &rep);
    CHECK(rep.ok && rep.model == &ms2[2]); /* «a» умеет только text, «b» универсальная, но «c» явно code */
    router_reply_free(&rep);

    /* все недоступны → ok=false и понятная ошибка */
    model_cfg bad[2] = { mk("x1", "X1", "quota"), mk("x2", "X2", "rate_limit") };
    app_config cfg3 = mkcfg(bad, 2);
    router rt3 = { &cfg3, q };
    router_chat(&rt3, HELLO, 1, NULL, NULL, &rep);
    CHECK(!rep.ok && rep.nattempts == 2 && strstr(rep.error, "каскаде"));
    router_reply_free(&rep);
}

static void test_history(quota_db *q) {
    /* склейка подряд идущих ролей и отбрасывание ведущего ответа ассистента не ломают запрос */
    model_cfg ms[1] = { mk("echo", "Echo", "ok") };
    app_config cfg = mkcfg(ms, 1);
    router rt = { &cfg, q };
    chat_msg msgs[] = { { "assistant", "приветствие" }, { "user", "раз" }, { "user", "два" }, { "assistant", "" }, { "user", "три" } };
    router_reply rep;
    router_chat(&rt, msgs, 5, NULL, NULL, &rep);
    CHECK(rep.ok && strstr(rep.text, "три"));
    router_reply_free(&rep);
}

static void test_api(quota_db *q) {
    model_cfg ms[2] = { mk("gemini", "Gemini", "rate_limit"), mk("gpt", "GPT-4o", "ok") };
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    server_ctx ctx = { &cfg, &rt, q };
    char *resp;
    CHECK(server_handle(&ctx, "GET", "/api/health", "", 1, &resp) == 200);
    free(resp);
    CHECK(server_handle(&ctx, "POST", "/api/chat", "не json", 1, &resp) == 400);
    free(resp);
    CHECK(server_handle(&ctx, "POST", "/api/chat", "{\"messages\":[]}", 1, &resp) == 400);
    free(resp);
    CHECK(server_handle(&ctx, "POST", "/api/chat", "{\"messages\":[{\"role\":\"system\",\"content\":\"x\"}]}", 1, &resp) == 400);
    free(resp);
    CHECK(server_handle(&ctx, "GET", "/api/chat", "", 1, &resp) == 405);
    free(resp);
    int code = server_handle(&ctx, "POST", "/api/chat",
                             "{\"messages\":[{\"role\":\"user\",\"content\":\"Сделай сценарий\"}],\"model\":\"auto\",\"skill\":\"scripts\"}", 1, &resp);
    CHECK(code == 200);
    json *j = json_parse(resp, NULL, 0);
    CHECK(j && !strcmp(json_str(json_get(j, "badge"), ""), "Ответил: GPT-4o"));
    CHECK(!strcmp(json_str(json_path(j, "attempts.0.status"), ""), "rate_limit"));
    json_free(j);
    free(resp);
    CHECK(server_handle(&ctx, "GET", "/api/models", "", 1, &resp) == 200);
    j = json_parse(resp, NULL, 0);
    CHECK(j && json_len(json_get(j, "models")) == 2);
    CHECK(json_num(json_path(j, "models.0.cooldown_until"), 0) > 0); /* Gemini на паузе после 429 */
    CHECK(json_num(json_path(j, "models.1.usage.day"), 0) >= 1);
    json_free(j);
    free(resp);
    CHECK(server_handle(&ctx, "POST", "/api/models/gemini/reset", "", 1, &resp) == 200);
    free(resp);
    CHECK(server_handle(&ctx, "GET", "/nope", "", 1, &resp) == 404);
    free(resp);
}

static void test_keys(quota_db *q) {
    /* ключ из «Настроек» сохраняется на сервере и используется роутером вместо .env */
    model_cfg ms[2] = { mk("groq", "Llama", "ok"), mk("fallback", "Fallback", "ok") };
    xstrlcpy(ms[0].provider, "openai", sizeof ms[0].provider);
    xstrlcpy(ms[0].model, "llama", sizeof ms[0].model);
    xstrlcpy(ms[0].base_url, "http://127.0.0.1:9", sizeof ms[0].base_url); /* закрытый порт → быстрая сетевая ошибка */
    xstrlcpy(ms[0].key_name, "Groq", sizeof ms[0].key_name);
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    server_ctx ctx = { &cfg, &rt, q };
    char *resp;
    router_reply rep;

    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.attempts[0].kind == RES_NOT_CONFIGURED && strstr(rep.attempts[0].message, "Groq"));
    router_reply_free(&rep);

    CHECK(server_handle(&ctx, "POST", "/api/keys", "{\"provider\":\"Groq\",\"key\":\"gsk_secret_ABCD\"}", 0, &resp) == 403);
    free(resp);
    CHECK(server_handle(&ctx, "POST", "/api/keys", "{\"provider\":\"Groq\",\"key\":\"bad\\r\\nX-Evil: 1\"}", 1, &resp) == 400);
    free(resp);
    CHECK(server_handle(&ctx, "POST", "/api/keys", "{\"provider\":\"Groq\",\"key\":\"gsk_secret_ABCD\"}", 1, &resp) == 200);
    CHECK(strstr(resp, "\"hint\":\"…ABCD\"") && !strstr(resp, "gsk_secret"));
    free(resp);

    char kb[600];
    const char *src = NULL;
    CHECK(router_api_key(&rt, &ms[0], kb, sizeof kb, &src) && !strcmp(kb, "gsk_secret_ABCD") && !strcmp(src, "settings"));
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.attempts[0].kind == RES_NETWORK); /* ключ взят — модель реально вызывалась */
    CHECK(rep.ok && rep.model == &ms[1]);
    router_reply_free(&rep);

    CHECK(server_handle(&ctx, "GET", "/api/keys", "", 1, &resp) == 200);
    CHECK(strstr(resp, "\"provider\":\"Groq\",\"set\":true,\"source\":\"settings\"") && !strstr(resp, "gsk_secret"));
    free(resp);

    /* смена ключа снимает паузу модели после ошибки */
    CHECK(server_handle(&ctx, "POST", "/api/keys/bulk", "{\"keys\":{\"Groq\":\"gsk_new_EFGH\",\"OpenAI\":\"sk-1234\",\"Bad\":\"a b\"}}", 1, &resp) == 200);
    CHECK(strstr(resp, "\"saved\":2") && strstr(resp, "\"skipped\":1"));
    free(resp);
    quota_status st;
    quota_get_status(q, &ms[0], plat_now(), &st);
    CHECK(st.cooldown_until == 0);
    CHECK(store_key_get(q, "Groq", kb, sizeof kb) && !strcmp(kb, "gsk_new_EFGH"));

    /* пустой ключ удаляет */
    CHECK(server_handle(&ctx, "POST", "/api/keys", "{\"provider\":\"Groq\",\"key\":\"\"}", 1, &resp) == 200);
    free(resp);
    CHECK(!store_key_get(q, "Groq", kb, sizeof kb));
    store_key_set(q, "OpenAI", NULL);
}

/* ---------------- настройки роутера ---------------- */

static void test_settings_api(quota_db *q) {
    set_settings(q, DEFAULTS);
    model_cfg ms[1] = { mk("a", "A", "ok") };
    app_config cfg = mkcfg(ms, 1);
    router rt = { &cfg, q };
    server_ctx ctx = { &cfg, &rt, q };
    char *resp;
    CHECK(server_handle(&ctx, "GET", "/api/settings", "", 1, &resp) == 200);
    CHECK(strstr(resp, "\"threshold\":90") && strstr(resp, "\"strategy\":\"free\"") && strstr(resp, "\"encryption_available\":true"));
    free(resp);
    CHECK(server_handle(&ctx, "POST", "/api/settings", "{\"threshold\":30}", 1, &resp) == 400);
    free(resp);
    CHECK(server_handle(&ctx, "POST", "/api/settings", "{\"strategy\":\"fastest\"}", 1, &resp) == 400);
    free(resp);
    CHECK(server_handle(&ctx, "POST", "/api/settings", "{\"threshold\":75}", 0, &resp) == 403);
    free(resp);
    CHECK(server_handle(&ctx, "POST", "/api/settings", "{\"threshold\":75,\"strategy\":\"quality\"}", 1, &resp) == 200);
    CHECK(strstr(resp, "\"threshold\":75") && strstr(resp, "\"strategy\":\"quality\"") && strstr(resp, "\"autoSwitch\":true"));
    free(resp);
    /* настройки сохраняются в БД и переживают перезапуск */
    settings_init(q);
    router_settings s;
    settings_get(&s);
    CHECK(s.threshold == 75 && s.strategy == STRATEGY_QUALITY);
    set_settings(q, DEFAULTS);
}

static void test_threshold(quota_db *q) {
    /* порог 50%: при лимите 10 запросов/сутки переключение после 5-го */
    set_settings(q, "{\"threshold\":50}");
    model_cfg ms[2] = { mk("thr", "Thr", "ok"), mk("res", "Res", "ok") };
    ms[0].limits.rpd = 10;
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    router_reply rep;
    int first = 0;
    for (int i = 0; i < 7; i++) {
        router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
        if (rep.model == &ms[0]) first++;
        router_reply_free(&rep);
    }
    CHECK(first == 5);
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.attempts[0].kind == RES_LOCAL_LIMIT && strstr(rep.attempts[0].message, "порог 50%"));
    router_reply_free(&rep);
    /* при выключенном автопереключении порог не действует — модель работает до 100% */
    set_settings(q, "{\"autoSwitch\":false}");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[0]);
    router_reply_free(&rep);
    quota_reset(q, "thr");
    quota_reset(q, "res");
    set_settings(q, DEFAULTS);
}

static void test_autoswitch_off(quota_db *q) {
    model_cfg ms[2] = { mk("main", "Main", "quota"), mk("backup", "Backup", "ok") };
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    router_reply rep;
    set_settings(q, "{\"autoSwitch\":false}");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(!rep.ok && rep.nattempts == 1 && strstr(rep.error, "автопереключение"));
    router_reply_free(&rep);
    quota_reset(q, "main");
    set_settings(q, "{\"autoSwitch\":true}");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1]);
    router_reply_free(&rep);
    quota_reset(q, "main");
    set_settings(q, DEFAULTS);
}

static void test_strategies(quota_db *q) {
    /* f1: бесплатная, слабая, загружена на 80%; f2: бесплатная, средняя, свободна; p: платная, сильная */
    model_cfg ms[3] = { mk("f1", "F1", "ok"), mk("f2", "F2", "ok"), mk("p", "P", "ok") };
    ms[0].priority = 3; ms[0].limits.rpd = 10;
    ms[1].priority = 2; ms[1].limits.rpd = 10;
    ms[2].priority = 1; xstrlcpy(ms[2].tier, "paid", sizeof ms[2].tier);
    for (int i = 0; i < 8; i++) quota_record(q, &ms[0], plat_now(), 1);
    app_config cfg = mkcfg(ms, 3);
    router rt = { &cfg, q };
    router_reply rep;
    set_settings(q, "{\"strategy\":\"cheap\",\"threshold\":100}");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.model == &ms[0]); /* дешевле: бесплатные по порядку конфига */
    router_reply_free(&rep);
    set_settings(q, "{\"strategy\":\"free\"}");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.model == &ms[1]); /* свободнее: бесплатная с наибольшим запасом */
    router_reply_free(&rep);
    set_settings(q, "{\"strategy\":\"quality\"}");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.model == &ms[2]); /* качественнее: сильнейшая, даже платная */
    router_reply_free(&rep);
    for (int i = 0; i < 3; i++) quota_reset(q, ms[i].id);
    set_settings(q, DEFAULTS);
}

static void test_keep_context(quota_db *q) {
    model_cfg ms[2] = { mk("lim", "Lim", "rate_limit"), mk("ctx", "Ctx", "ok") };
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    chat_msg hist[] = { { "user", "раз" }, { "assistant", "ответ" }, { "user", "два" } };
    router_reply rep;
    router_chat(&rt, hist, 3, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1] && strstr(rep.text, "сообщений: 3"));
    router_reply_free(&rep);
    quota_reset(q, "lim");
    set_settings(q, "{\"keepContext\":false}");
    router_chat(&rt, hist, 3, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1] && strstr(rep.text, "сообщений: 1") && strstr(rep.text, "два"));
    router_reply_free(&rep);
    quota_reset(q, "lim");
    set_settings(q, DEFAULTS);
}

static void test_disabled(quota_db *q) {
    model_cfg ms[2] = { mk("off", "Off", "ok"), mk("on", "On", "ok") };
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    router_reply rep;
    set_settings(q, "{\"disabled\":[\"off\"]}");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1] && rep.nattempts == 1 && !rep.switched);
    router_reply_free(&rep);
    /* закрепить выключенную модель тоже нельзя */
    router_chat(&rt, HELLO, 1, "off", NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1]);
    router_reply_free(&rep);
    set_settings(q, DEFAULTS);
}

static void test_catalog_auto(quota_db *q) {
    /* модель каталога без ключа в каскаде не участвует, с ключом — участвует */
    model_cfg ms[2] = { mk("cat-x", "CatX", "ok"), mk("last", "Last", "ok") };
    xstrlcpy(ms[0].provider, "openai", sizeof ms[0].provider);
    xstrlcpy(ms[0].model, "x", sizeof ms[0].model);
    xstrlcpy(ms[0].base_url, "http://127.0.0.1:9", sizeof ms[0].base_url);
    xstrlcpy(ms[0].key_name, "TestProv", sizeof ms[0].key_name);
    ms[0].auto_added = 1;
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    router_reply rep;
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1] && rep.nattempts == 1 && !rep.switched);
    router_reply_free(&rep);
    store_key_set(q, "TestProv", "tp-key");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.nattempts == 2 && rep.attempts[0].model == &ms[0] && rep.attempts[0].kind == RES_NETWORK);
    router_reply_free(&rep);
    store_key_set(q, "TestProv", NULL);
    quota_reset(q, "cat-x");

    /* загрузка catalog.json: дубликат id из config.json пропускается */
    const char *path = "test_catalog.json";
    FILE *f = fopen(path, "w");
    fputs("{\"models\":[{\"id\":\"last\",\"label\":\"Dup\",\"provider\":\"openai\",\"model\":\"m\",\"key_name\":\"Groq\"},"
          "{\"id\":\"cat-groq-y\",\"label\":\"Y\",\"provider\":\"openai\",\"base_url\":\"https://api.groq.com/openai/v1\",\"model\":\"y\","
          "\"tier\":\"free\",\"priority\":1,\"limits\":{\"rpd\":1000}},{\"bad\":true}]}", f);
    fclose(f);
    model_cfg *buf = calloc(CFG_MAX_MODELS, sizeof *buf);
    buf[0] = mk("last", "Last", "ok");
    app_config c2 = mkcfg(buf, 1);
    char err[200];
    CHECK(config_load_catalog(&c2, path, err, sizeof err) == 1);
    CHECK(c2.nmodels == 2 && c2.models[1].auto_added && !strcmp(c2.models[1].key_name, "Groq") && c2.models[1].limits.rpd == 1000);
    free(buf);
    remove(path);
}

static void test_cloudflare_key(void) {
    model_cfg m = mk("cf", "CF", "ok");
    xstrlcpy(m.provider, "openai", sizeof m.provider);
    xstrlcpy(m.model, "@cf/meta/llama", sizeof m.model);
    xstrlcpy(m.key_name, "Cloudflare Workers AI", sizeof m.key_name);
    xstrlcpy(m.base_url, "http://127.0.0.1:9/{account}/ai/v1", sizeof m.base_url);
    chat_request req = { HELLO, 1, NULL };
    call_result r;
    provider_openai(&m, "token-without-account", &req, 1000, &r);
    CHECK(r.kind == RES_AUTH && strstr(r.error, "ACCOUNT_ID:API_TOKEN"));
    provider_openai(&m, "acc123:tok", &req, 1000, &r);
    CHECK(r.kind == RES_NETWORK); /* формат принят, дошли до сети */
    call_result_free(&r);
    call_result c;
    memset(&c, 0, sizeof c);
    c.kind = RES_BAD_REQUEST;
    c.http_status = 404;
    CHECK(router_cooldown_seconds(&c, 0) == 6 * 3600);
}

/* ---------------- шифрование ключей ---------------- */

static void test_encryption(quota_db *q) {
    char enc[4096], dec[600], raw[4096];
    CHECK(crypto_available());
    CHECK(crypto_encrypt("sk-secret-123", enc, sizeof enc) == 0 && crypto_is_encrypted(enc) && !strstr(enc, "secret"));
    CHECK(crypto_decrypt(enc, dec, sizeof dec) == 0 && !strcmp(dec, "sk-secret-123"));
    char enc2[4096];
    crypto_encrypt("sk-secret-123", enc2, sizeof enc2);
    CHECK(strcmp(enc, enc2) != 0); /* случайный nonce: одинаковые ключи шифруются по-разному */
    char bad[4096];
    strcpy(bad, enc);
    bad[strlen(bad) - 6] = bad[strlen(bad) - 6] == 'A' ? 'B' : 'A'; /* подмена байта */
    CHECK(crypto_decrypt(bad, dec, sizeof dec) != 0);

    /* включение шифрования перешифровывает уже сохранённые ключи */
    store_key_set(q, "EncProv", "plain-key-777");
    CHECK(store_key_get_raw(q, "EncProv", raw, sizeof raw) && !strcmp(raw, "plain-key-777"));
    set_settings(q, "{\"encryptKeys\":true}");
    CHECK(store_key_get_raw(q, "EncProv", raw, sizeof raw) && crypto_is_encrypted(raw) && !strstr(raw, "777"));
    CHECK(store_key_get(q, "EncProv", dec, sizeof dec) && !strcmp(dec, "plain-key-777"));
    store_key_set(q, "EncProv2", "new-key-888"); /* новые ключи тоже шифруются */
    CHECK(store_key_get_raw(q, "EncProv2", raw, sizeof raw) && crypto_is_encrypted(raw));
    /* выключение — обратно в открытый вид */
    set_settings(q, "{\"encryptKeys\":false}");
    CHECK(store_key_get_raw(q, "EncProv", raw, sizeof raw) && !strcmp(raw, "plain-key-777"));
    CHECK(store_key_get_raw(q, "EncProv2", raw, sizeof raw) && !strcmp(raw, "new-key-888"));

    /* чужой мастер-ключ не расшифрует */
    crypto_shutdown();
    remove("test_master2.key");
    char err[200];
    CHECK(crypto_init("test_master2.key", err, sizeof err) == 0);
    CHECK(crypto_decrypt(enc, dec, sizeof dec) != 0);
    crypto_shutdown();
    CHECK(crypto_init("test_master.key", err, sizeof err) == 0); /* обратно свой ключ */
    CHECK(crypto_decrypt(enc, dec, sizeof dec) == 0);
    remove("test_master2.key");
    store_key_set(q, "EncProv", NULL);
    store_key_set(q, "EncProv2", NULL);
    set_settings(q, DEFAULTS);
}

int main(void) {
    log_set_level(LOG_ERROR); /* в тестах не шумим */
    const char *db = "test_quota.db";
    remove(db);
    remove("test_quota.db-wal");
    remove("test_quota.db-shm");
    char err[256];
    quota_db *q = quota_open(db, err, sizeof err);
    if (!q) { fprintf(stderr, "quota_open: %s\n", err); return 1; }
    remove("test_master.key");
    if (crypto_init("test_master.key", err, sizeof err) != 0) { fprintf(stderr, "crypto: %s\n", err); return 1; }
    settings_init(q);

    test_json();
    test_classify();
    test_cooldown();
    test_cascade(q);
    test_local_limit(q);
    test_errors_and_order(q);
    test_history(q);
    test_api(q);
    test_keys(q);
    test_settings_api(q);
    test_threshold(q);
    test_autoswitch_off(q);
    test_strategies(q);
    test_keep_context(q);
    test_disabled(q);
    test_catalog_auto(q);
    test_cloudflare_key();
    test_encryption(q);

    quota_close(q);
    crypto_shutdown();
    remove("test_master.key");
    remove(db);
    remove("test_quota.db-wal");
    remove("test_quota.db-shm");
    printf("%s: %d проверок пройдено, %d провалено\n", g_fail ? "FAIL" : "OK", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
