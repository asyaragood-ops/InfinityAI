#include "router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "settings.h"
#include "platform.h"
#include "strbuf.h"

#define MAX_COOLDOWN (2L * 86400)

long router_cooldown_seconds(const call_result *res, int64_t now) {
    long s = 0;
    switch (res->kind) {
        case RES_RATE_LIMIT:
            s = res->retry_after_s > 0 ? res->retry_after_s : 60;
            break;
        case RES_QUOTA:
            if (res->retry_after_s > 0) s = res->retry_after_s;          /* провайдер сам сказал, сколько ждать */
            else if (res->limit_daily) s = (long)(quota_next_day(now) - now); /* суточный лимит — до конца суток UTC */
            else if (res->limit_billing) s = 3600;                     /* кончились деньги — проверяем раз в час */
            else s = 60;                                               /* похоже на минутный лимит */
            break;
        case RES_AUTH: s = 600; break;                                 /* неверный ключ — не долбим провайдера */
        case RES_SERVER: s = res->retry_after_s > 0 ? res->retry_after_s : 30; break;
        case RES_NETWORK: s = 15; break;
        case RES_BAD_REQUEST: s = res->http_status == 404 ? 6 * 3600 : 0; break; /* нет такой модели — не дёргаем 6 ч */
        default: s = 0;                                                /* bad_response — без паузы */
    }
    if (s > MAX_COOLDOWN) s = MAX_COOLDOWN;
    return s;
}

const char *router_api_key(router *r, const model_cfg *m, char *buf, size_t len, const char **source) {
    const char *src = NULL, *key = NULL;
    if (!strcmp(m->provider, "mock")) { src = "mock"; key = "mock"; }
    else if (r->quota && store_key_get(r->quota, m->key_name, buf, len)) { src = "settings"; key = buf; }
    else if ((key = config_api_key(m)) != NULL) src = "env";
    if (source) *source = src;
    return key;
}

static void new_request_id(char *out, size_t n) {
    static unsigned counter = 0;
    unsigned c = __sync_fetch_and_add(&counter, 1);
    snprintf(out, n, "%llx%04x", (unsigned long long)plat_now_ms(), c & 0xFFFF);
}

/* Подготовка истории: последние max_history сообщений, первое — от пользователя, без пустых,
 * подряд идущие сообщения одной роли склеиваются (этого требуют Anthropic и Gemini). */
typedef struct {
    chat_msg *msgs;
    char    **owned;
    size_t    n, nowned;
} norm_msgs;

static int normalize(const chat_msg *in, size_t n, int max_history, norm_msgs *out) {
    memset(out, 0, sizeof *out);
    size_t start = n > (size_t)max_history ? n - (size_t)max_history : 0;
    out->msgs = calloc(n - start + 1, sizeof *out->msgs);
    out->owned = calloc(n - start + 1, sizeof *out->owned);
    if (!out->msgs || !out->owned) return -1;
    for (size_t i = start; i < n; i++) {
        const char *role = strcmp(in[i].role, "assistant") ? "user" : "assistant";
        if (!in[i].content || !*in[i].content) continue;
        if (out->n == 0 && !strcmp(role, "assistant")) continue; /* начинаем с пользователя */
        if (out->n && !strcmp(out->msgs[out->n - 1].role, role)) {
            strbuf sb;
            sb_init(&sb);
            sb_append(&sb, out->msgs[out->n - 1].content);
            sb_append(&sb, "\n\n");
            sb_append(&sb, in[i].content);
            if (sb.oom) { sb_free(&sb); return -1; }
            char *merged = sb_take(&sb);
            out->owned[out->nowned++] = merged;
            out->msgs[out->n - 1].content = merged;
            continue;
        }
        out->msgs[out->n].role = role;
        out->msgs[out->n].content = in[i].content;
        out->n++;
    }
    return 0;
}

static void norm_free(norm_msgs *nm) {
    for (size_t i = 0; i < nm->nowned; i++) free(nm->owned[i]);
    free(nm->owned);
    free(nm->msgs);
}

/* Кандидат каскада и ключи сортировки. */
typedef struct {
    size_t idx;
    int    tier;   /* 0 free, 1 trial, 2 paid */
    int    skill;  /* 0 — навык указан явно, 1 — универсальная модель, 2 — навык не подходит */
    int    prio;   /* качество: 1 — сильнейшие */
    int    load;   /* загруженность по локальным лимитам, 0–100 (для «Свободнее») */
} cand;

static int tier_rank(const char *t) { return !strcmp(t, "paid") ? 2 : !strcmp(t, "trial") ? 1 : 0; }

static int cand_cmp(const cand *a, const cand *b, strategy_t st) {
#define CMP(x) do { if (a->x != b->x) return a->x < b->x ? -1 : 1; } while (0)
    switch (st) {
        case STRATEGY_QUALITY: CMP(skill); CMP(prio); CMP(tier); break;           /* сильнейшие первыми, даже платные */
        case STRATEGY_FREE:    CMP(tier); CMP(skill); CMP(load); CMP(prio); break; /* бесплатные с наибольшим запасом */
        default:               CMP(tier); CMP(skill); break;                      /* дешевле: бесплатные, затем пробные, платные */
    }
    CMP(idx); /* порядок config.json, затем каталог */
#undef CMP
    return 0;
}

/* Порядок каскада по настройкам. Модели каталога без ключа и выключенные на сайте не участвуют.
 * Закреплённая в чате модель всегда идёт первой. Возвращает число кандидатов в order. */
static size_t build_order(router *r, const router_settings *st, const char *prefer, const char *skill, size_t *order) {
    const app_config *cfg = r->cfg;
    cand *c = malloc(cfg->nmodels * sizeof *c);
    if (!c) return 0;
    size_t n = 0, k = 0;
    int pinned = -1;
    if (prefer && *prefer && strcmp(prefer, "auto"))
        for (size_t i = 0; i < cfg->nmodels; i++)
            if (!strcmp(cfg->models[i].id, prefer) && cfg->models[i].enabled && !settings_is_disabled(st, cfg->models[i].id)) { pinned = (int)i; order[k++] = i; break; }
    int64_t now = plat_now();
    for (size_t i = 0; i < cfg->nmodels; i++) {
        const model_cfg *m = &cfg->models[i];
        if ((int)i == pinned || !m->enabled || settings_is_disabled(st, m->id)) continue;
        if (m->auto_added) {
            char kb[600];
            if (!router_api_key(r, m, kb, sizeof kb, NULL)) continue; /* модель каталога без ключа — не в каскаде */
        }
        cand x = { i, tier_rank(m->tier), !skill || !*skill ? 0 : m->nskills == 0 ? 1 : model_has_skill(m, skill) ? 0 : 2,
                   m->priority, 0 };
        if (st->strategy == STRATEGY_FREE) {
            int l = quota_load_pct(r->quota, m, now);
            x.load = l < 0 ? 50 : l; /* лимит неизвестен — считаем загруженной наполовину */
        }
        /* вставка с сохранением порядка (кандидатов не больше нескольких сотен) */
        size_t j = n++;
        while (j > 0 && cand_cmp(&x, &c[j - 1], st->strategy) < 0) { c[j] = c[j - 1]; j--; }
        c[j] = x;
    }
    for (size_t i = 0; i < n; i++) order[k++] = c[i].idx;
    free(c);
    return k;
}

static void add_attempt(router_reply *out, const model_cfg *m, result_kind kind, long status, long latency, const char *msg) {
    if (out->nattempts >= ROUTER_MAX_ATTEMPTS) return;
    attempt_info *a = &out->attempts[out->nattempts++];
    a->model = m;
    a->kind = kind;
    a->http_status = status;
    a->latency_ms = latency;
    xstrlcpy(a->message, msg ? msg : "", sizeof a->message);
}

void router_chat(router *r, const chat_msg *msgs, size_t n, const char *prefer, const char *skill, router_reply *out) {
    memset(out, 0, sizeof *out);
    new_request_id(out->request_id, sizeof out->request_id);
    int64_t t0 = plat_now_ms();
    const app_config *cfg = r->cfg;

    norm_msgs nm;
    if (normalize(msgs, n, cfg->max_history, &nm) != 0 || nm.n == 0) {
        norm_free(&nm);
        xstrlcpy(out->error, "пустой запрос: нет сообщений пользователя", sizeof out->error);
        return;
    }
    chat_request req = { nm.msgs, nm.n, cfg->system_prompt };
    /* без передачи контекста резервная модель получает только последнее сообщение пользователя */
    chat_request req_last = { nm.msgs + nm.n - 1, 1, cfg->system_prompt };

    router_settings *st = malloc(sizeof *st);
    size_t *order = malloc((cfg->nmodels + 1) * sizeof *order);
    if (!st || !order) {
        free(st);
        free(order);
        norm_free(&nm);
        xstrlcpy(out->error, "нет памяти", sizeof out->error);
        return;
    }
    settings_get(st);
    int threshold = st->auto_switch ? st->threshold : 100;
    size_t norder = build_order(r, st, prefer, skill, order);
    int tried_real = 0;  /* сколько моделей действительно вызывали (для флага switched) */
    int considered = 0;  /* моделей с ключом, до которых дошла очередь (для «без автопереключения») */

    for (size_t oi = 0; oi < norder; oi++) {
        const model_cfg *m = &cfg->models[order[oi]];
        int64_t now = plat_now();
        char reason[200];

        /* --- предварительные проверки: есть ключ, не исчерпан локальный лимит --- */
        if (!st->auto_switch && considered > 0) {
            LOGI("router", "[%s] автопереключение выключено в «Настройках» — резервные ИИ не используются", out->request_id);
            break;
        }
        char keybuf[600];
        const char *key = router_api_key(r, m, keybuf, sizeof keybuf, NULL);
        if (!key) {
            snprintf(reason, sizeof reason, "нет API-ключа «%s» (Настройки → API-ключи)", m->key_name);
            LOGI("router", "[%s] %s: пропуск — %s", out->request_id, m->label, reason);
            add_attempt(out, m, RES_NOT_CONFIGURED, 0, 0, reason);
            continue;
        }
        considered++;
        quota_state qs = quota_check(r->quota, m, now, threshold, reason, sizeof reason);
        if (qs != QUOTA_OK) {
            result_kind k = qs == QUOTA_COOLDOWN ? RES_COOLDOWN : RES_LOCAL_LIMIT;
            LOGW("router", "[%s] %s: Основной лимит исчерпан (%s). %s", out->request_id, m->label, reason,
                 !st->auto_switch ? "Автопереключение выключено." : oi + 1 < norder ? "Переключаюсь на резервный ИИ..." : "Резервных ИИ в каскаде больше нет.");
            add_attempt(out, m, k, 0, 0, reason);
            quota_log_attempt(r->quota, now, out->request_id, m->id, result_kind_code(k), 0, reason);
            continue;
        }

        /* --- «try»: вызов провайдера --- */
        provider_call_fn call = provider_for(m);
        call_result res;
        int64_t c0 = plat_now_ms();
        const chat_request *use = (tried_real > 0 && !st->keep_context) ? &req_last : &req;
        LOGI("router", "[%s] → %s (%s/%s)%s", out->request_id, m->label, m->provider, m->model,
             use == &req_last ? " — без истории чата (контекст не передаётся)" : "");
        call(m, key, use, cfg->timeout_ms, &res);
        long latency = (long)(plat_now_ms() - c0);
        tried_real++;

        if (res.kind == RES_OK) {
            quota_record(r->quota, m, plat_now(), res.tokens_in + res.tokens_out);
            quota_log_attempt(r->quota, plat_now(), out->request_id, m->id, "ok", res.http_status, NULL);
            add_attempt(out, m, RES_OK, res.http_status, latency, "");
            out->ok = 1;
            out->text = res.text; /* передаём владение */
            res.text = NULL;
            out->model = m;
            out->switched = out->nattempts > 1;
            out->tokens_in = res.tokens_in;
            out->tokens_out = res.tokens_out;
            LOGI("router", "[%s] ✓ Ответил: %s за %ld мс (%ld+%ld токенов)%s", out->request_id, m->label, latency,
                 res.tokens_in, res.tokens_out, out->switched ? " — после переключения" : "");
            break;
        }

        /* --- «catch»: разбираем ошибку и решаем, куда идти дальше --- */
        long cd = router_cooldown_seconds(&res, plat_now());
        const char *next = !st->auto_switch ? "Автопереключение выключено."
                         : oi + 1 < norder ? "Переключаюсь на резервный ИИ..." : "Резервных ИИ в каскаде больше нет.";
        if (result_is_limit(res.kind))
            LOGW("router", "[%s] %s: Основной лимит исчерпан (%s). %s", out->request_id, m->label, res.error, next);
        else
            LOGW("router", "[%s] %s: ошибка %s (%s). %s", out->request_id, m->label, result_kind_code(res.kind), res.error, next);
        if (cd > 0) {
            char why[160];
            snprintf(why, sizeof why, "%s: %.120s", result_kind_code(res.kind), res.error);
            quota_set_cooldown(r->quota, m->id, plat_now() + cd, why);
            LOGI("router", "[%s] %s на паузе %ld с", out->request_id, m->label, cd);
        }
        quota_log_attempt(r->quota, plat_now(), out->request_id, m->id, result_kind_code(res.kind), res.http_status, res.error);
        add_attempt(out, m, res.kind, res.http_status, latency, res.error);
        call_result_free(&res);
    }

    if (!out->ok) {
        if (!st->auto_switch && considered > 0)
            xstrlcpy(out->error, "Модель недоступна, а автопереключение выключено в «Настройках»", sizeof out->error);
        else if (tried_real == 0)
            xstrlcpy(out->error, "Нет доступных моделей: не заданы API-ключи или исчерпаны все лимиты", sizeof out->error);
        else
            xstrlcpy(out->error, "Все ИИ в каскаде недоступны или исчерпали лимиты", sizeof out->error);
        LOGE("router", "[%s] %s", out->request_id, out->error);
    }
    out->latency_ms = (long)(plat_now_ms() - t0);
    free(st);
    free(order);
    norm_free(&nm);
}

void router_reply_free(router_reply *out) {
    free(out->text);
    out->text = NULL;
}

static void model_json(strbuf *b, const model_cfg *m) {
    sb_append(b, "{\"id\":");
    sb_json_str(b, m->id);
    sb_append(b, ",\"label\":");
    sb_json_str(b, m->label);
    sb_append(b, ",\"provider\":");
    sb_json_str(b, m->provider);
    sb_append(b, ",\"model\":");
    sb_json_str(b, m->model);
    sb_append(b, ",\"tier\":");
    sb_json_str(b, m->tier);
    sb_append(b, "}");
}

char *router_reply_json(const router_reply *out) {
    strbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"ok\":%s,\"request_id\":", out->ok ? "true" : "false");
    sb_json_str(&b, out->request_id);
    if (out->ok) {
        char badge[96];
        snprintf(badge, sizeof badge, "Ответил: %s", out->model->label);
        sb_append(&b, ",\"text\":");
        sb_json_str(&b, out->text);
        sb_append(&b, ",\"badge\":");
        sb_json_str(&b, badge);
        sb_append(&b, ",\"model\":");
        model_json(&b, out->model);
        sb_printf(&b, ",\"switched\":%s,\"usage\":{\"input_tokens\":%ld,\"output_tokens\":%ld}", out->switched ? "true" : "false",
                  out->tokens_in, out->tokens_out);
    } else {
        sb_append(&b, ",\"error\":");
        sb_json_str(&b, out->error);
    }
    sb_append(&b, ",\"attempts\":[");
    for (int i = 0; i < out->nattempts; i++) {
        const attempt_info *a = &out->attempts[i];
        if (i) sb_appendc(&b, ',');
        sb_append(&b, "{\"model\":");
        model_json(&b, a->model);
        sb_append(&b, ",\"status\":");
        sb_json_str(&b, result_kind_code(a->kind));
        sb_printf(&b, ",\"limit\":%s,\"http_status\":%ld,\"latency_ms\":%ld,\"message\":", result_is_limit(a->kind) ? "true" : "false",
                  a->http_status, a->latency_ms);
        sb_json_str(&b, a->message);
        sb_append(&b, "}");
    }
    sb_printf(&b, "],\"latency_ms\":%ld}", out->latency_ms);
    return b.oom ? (sb_free(&b), NULL) : sb_take(&b);
}
