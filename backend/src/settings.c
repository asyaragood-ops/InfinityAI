#include "settings.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"
#include "json.h"
#include "log.h"

static router_settings g_settings;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_inited = 0;

void settings_defaults(router_settings *s) {
    memset(s, 0, sizeof *s);
    s->auto_switch = 1;
    s->threshold = 90;
    s->strategy = STRATEGY_FREE;
    s->keep_context = 1;
    s->notify_switch = 1;
    s->encrypt_keys = 0;
}

const char *strategy_code(strategy_t s) {
    switch (s) {
        case STRATEGY_CHEAP: return "cheap";
        case STRATEGY_QUALITY: return "quality";
        default: return "free";
    }
}

static int parse_strategy(const char *v, strategy_t *out) {
    if (!strcmp(v, "cheap")) *out = STRATEGY_CHEAP;
    else if (!strcmp(v, "free")) *out = STRATEGY_FREE;
    else if (!strcmp(v, "quality")) *out = STRATEGY_QUALITY;
    else return -1;
    return 0;
}

int settings_is_disabled(const router_settings *s, const char *model_id) {
    for (int i = 0; i < s->ndisabled; i++)
        if (!strcmp(s->disabled[i], model_id)) return 1;
    return 0;
}

/* Разбор JSON поверх dst. Неизвестные поля игнорируются. */
static int parse_into(router_settings *dst, const json *j, char *err, size_t errlen) {
    if (!j || j->type != JSON_OBJECT) { snprintf(err, errlen, "нужен JSON-объект настроек"); return -1; }
    const json *v;
    if ((v = json_get(j, "autoSwitch"))) {
        if (v->type != JSON_BOOL) { snprintf(err, errlen, "autoSwitch: true/false"); return -1; }
        dst->auto_switch = v->boolean;
    }
    if ((v = json_get(j, "threshold"))) {
        double t = json_num(v, -1);
        if (t < 50 || t > 100) { snprintf(err, errlen, "threshold: число от 50 до 100"); return -1; }
        dst->threshold = (int)t;
    }
    if ((v = json_get(j, "strategy"))) {
        if (parse_strategy(json_str(v, ""), &dst->strategy) != 0) { snprintf(err, errlen, "strategy: cheap | free | quality"); return -1; }
    }
    if ((v = json_get(j, "keepContext"))) {
        if (v->type != JSON_BOOL) { snprintf(err, errlen, "keepContext: true/false"); return -1; }
        dst->keep_context = v->boolean;
    }
    if ((v = json_get(j, "notifySwitch"))) {
        if (v->type != JSON_BOOL) { snprintf(err, errlen, "notifySwitch: true/false"); return -1; }
        dst->notify_switch = v->boolean;
    }
    if ((v = json_get(j, "encryptKeys"))) {
        if (v->type != JSON_BOOL) { snprintf(err, errlen, "encryptKeys: true/false"); return -1; }
        dst->encrypt_keys = v->boolean;
    }
    if ((v = json_get(j, "disabled"))) {
        if (v->type != JSON_ARRAY) { snprintf(err, errlen, "disabled: массив id моделей"); return -1; }
        dst->ndisabled = 0;
        for (const json *c = v->child; c && dst->ndisabled < CFG_MAX_MODELS; c = c->next) {
            const char *id = json_str(c, NULL);
            if (id && *id && strlen(id) < 64) xstrlcpy(dst->disabled[dst->ndisabled++], id, 64);
        }
    }
    return 0;
}

static void to_json(const router_settings *s, strbuf *b) {
    sb_printf(b, "{\"autoSwitch\":%s,\"threshold\":%d,\"strategy\":\"%s\",\"keepContext\":%s,\"notifySwitch\":%s,\"encryptKeys\":%s,\"disabled\":[",
              s->auto_switch ? "true" : "false", s->threshold, strategy_code(s->strategy), s->keep_context ? "true" : "false",
              s->notify_switch ? "true" : "false", s->encrypt_keys ? "true" : "false");
    for (int i = 0; i < s->ndisabled; i++) {
        if (i) sb_appendc(b, ',');
        sb_json_str(b, s->disabled[i]);
    }
    sb_append(b, "]}");
}

void settings_init(quota_db *q) {
    router_settings s;
    settings_defaults(&s);
    char *saved = quota_kv_get(q, "router_settings");
    if (saved) {
        json *j = json_parse(saved, NULL, 0);
        char err[128];
        if (j && parse_into(&s, j, err, sizeof err) != 0) settings_defaults(&s);
        json_free(j);
        free(saved);
    }
    if (s.encrypt_keys && !crypto_available()) {
        LOGW("settings", "шифрование ключей включено, но мастер-ключ недоступен — новые ключи будут храниться без шифрования");
    }
    quota_set_encryption(q, s.encrypt_keys && crypto_available(), 0);
    pthread_mutex_lock(&g_mu);
    g_settings = s;
    g_inited = 1;
    pthread_mutex_unlock(&g_mu);
}

void settings_get(router_settings *out) {
    pthread_mutex_lock(&g_mu);
    if (!g_inited) { settings_defaults(&g_settings); g_inited = 1; }
    *out = g_settings;
    pthread_mutex_unlock(&g_mu);
}

int settings_apply_json(quota_db *q, const char *json_text, char *err, size_t errlen) {
    char perr[128];
    json *j = json_parse(json_text ? json_text : "", perr, sizeof perr);
    if (!j) { snprintf(err, errlen, "ошибка JSON: %s", perr); return -1; }
    router_settings next;
    settings_get(&next);
    int was_encrypted = next.encrypt_keys;
    int rc = parse_into(&next, j, err, errlen);
    json_free(j);
    if (rc != 0) return -1;
    if (next.encrypt_keys && !crypto_available()) {
        snprintf(err, errlen, "шифрование недоступно: не удалось создать мастер-ключ (см. лог сервера)");
        return -1;
    }
    if (next.encrypt_keys != was_encrypted) {
        int n = quota_set_encryption(q, next.encrypt_keys, 1);
        if (n < 0) { snprintf(err, errlen, "не удалось перешифровать ключи"); return -1; }
        LOGI("settings", "шифрование ключей %s, обработано ключей: %d", next.encrypt_keys ? "включено" : "выключено", n);
    }
    strbuf b;
    sb_init(&b);
    to_json(&next, &b);
    quota_kv_set(q, "router_settings", b.data);
    sb_free(&b);
    pthread_mutex_lock(&g_mu);
    g_settings = next;
    pthread_mutex_unlock(&g_mu);
    LOGI("settings", "настройки роутера обновлены: автопереключение %s, порог %d%%, стратегия %s, контекст %s, выключено моделей: %d",
         next.auto_switch ? "вкл" : "выкл", next.threshold, strategy_code(next.strategy), next.keep_context ? "передаётся" : "не передаётся",
         next.ndisabled);
    return 0;
}

void settings_to_json(strbuf *b) {
    router_settings s;
    settings_get(&s);
    to_json(&s, b);
}
