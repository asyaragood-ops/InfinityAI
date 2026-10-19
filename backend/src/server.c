#include "server.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "json.h"
#include "log.h"
#include "crypto.h"
#include "platform.h"
#include "settings.h"
#include "strbuf.h"

#define MAX_HEADER   (16 * 1024)
#define MAX_BODY     (4 * 1024 * 1024)
#define MAX_MESSAGES 200
#define MAX_MSG_LEN  (200 * 1024)
#define MAX_CLIENTS  64
#define IO_TIMEOUT_MS 15000

static volatile sock_t g_listen = SOCK_INVALID;
static volatile int    g_running = 0;
static volatile int    g_clients = 0;

/* ------------------------------------------------------------------ ответы ---- */

static char *json_error(const char *msg) {
    strbuf b;
    sb_init(&b);
    sb_append(&b, "{\"ok\":false,\"error\":");
    sb_json_str(&b, msg);
    sb_append(&b, "}");
    return sb_take(&b);
}

static int handle_health(server_ctx *ctx, char **resp) {
    int configured = 0;
    for (size_t i = 0; i < ctx->cfg->nmodels; i++)
    {
        char kb[600];
        if (ctx->cfg->models[i].enabled && router_api_key(ctx->rt, &ctx->cfg->models[i], kb, sizeof kb, NULL)) configured++;
    }
    strbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"ok\":true,\"service\":\"infinityAI-router\",\"version\":\"0.1.0\",\"models\":%zu,\"configured\":%d}",
              ctx->cfg->nmodels, configured);
    *resp = sb_take(&b);
    return 200;
}

static int handle_models(server_ctx *ctx, char **resp) {
    int64_t now = plat_now();
    strbuf b;
    sb_init(&b);
    sb_append(&b, "{\"ok\":true,\"models\":[");
    router_settings *rs = malloc(sizeof *rs);
    if (!rs) { *resp = json_error("нет памяти"); return 500; }
    settings_get(rs);
    int first = 1;
    for (size_t i = 0; i < ctx->cfg->nmodels; i++) {
        const model_cfg *m = &ctx->cfg->models[i];
        quota_status st;
        quota_get_status(ctx->quota, m, now, &st);
        char reason[200];
        char kb[600];
        const char *src = NULL;
        int has_key = router_api_key(ctx->rt, m, kb, sizeof kb, &src) != NULL;
        if (m->auto_added && !has_key) continue; /* модели каталога показываем, только когда для них есть ключ */
        int disabled = settings_is_disabled(rs, m->id);
        int available = m->enabled && has_key && quota_check(ctx->quota, m, now, 100, reason, sizeof reason) == QUOTA_OK;
        if (!first) sb_appendc(&b, ',');
        first = 0;
        sb_append(&b, "{\"id\":");
        sb_json_str(&b, m->id);
        sb_append(&b, ",\"label\":");
        sb_json_str(&b, m->label);
        sb_append(&b, ",\"provider\":");
        sb_json_str(&b, m->provider);
        sb_append(&b, ",\"model\":");
        sb_json_str(&b, m->model);
        sb_append(&b, ",\"tier\":");
        sb_json_str(&b, m->tier);
        sb_append(&b, ",\"key_name\":");
        sb_json_str(&b, m->key_name);
        sb_append(&b, ",\"key_source\":");
        sb_json_str(&b, src);
        sb_printf(&b, ",\"order\":%zu,\"auto\":%s,\"priority\":%d,\"enabled\":%s,\"configured\":%s,\"available\":%s,\"skills\":[", i,
                  m->auto_added ? "true" : "false", m->priority, m->enabled && !disabled ? "true" : "false",
                  has_key ? "true" : "false", available && !disabled ? "true" : "false");
        for (int s = 0; s < m->nskills; s++) {
            if (s) sb_appendc(&b, ',');
            sb_json_str(&b, m->skills[s]);
        }
        sb_printf(&b, "],\"limits\":{\"rpm\":%d,\"rpd\":%d,\"rpmo\":%d,\"tpd\":%ld}", m->limits.rpm, m->limits.rpd,
                  m->limits.rpmo, m->limits.tpd);
        sb_printf(&b, ",\"usage\":{\"minute\":%d,\"day\":%d,\"month\":%d,\"tokens_day\":%ld}", st.req_minute, st.req_day,
                  st.req_month, st.tokens_day);
        sb_printf(&b, ",\"cooldown_until\":%lld,\"cooldown_reason\":", (long long)st.cooldown_until);
        sb_json_str(&b, st.cooldown_reason[0] ? st.cooldown_reason : NULL);
        sb_append(&b, "}");
    }
    free(rs);
    sb_printf(&b, "],\"day_resets_at\":%lld}", (long long)quota_next_day(now));
    *resp = sb_take(&b);
    return 200;
}

static int handle_chat(server_ctx *ctx, const char *body, char **resp) {
    char err[160];
    json *req = json_parse(body ? body : "", err, sizeof err);
    if (!req || req->type != JSON_OBJECT) {
        json_free(req);
        char msg[300];
        snprintf(msg, sizeof msg, "тело запроса должно быть JSON-объектом: %s", err);
        *resp = json_error(msg);
        return 400;
    }
    const json *arr = json_get(req, "messages");
    size_t n = json_len(arr);
    if (!arr || arr->type != JSON_ARRAY || n == 0 || n > MAX_MESSAGES) {
        json_free(req);
        *resp = json_error("поле messages: нужен массив из 1–200 сообщений");
        return 400;
    }
    chat_msg *msgs = calloc(n, sizeof *msgs);
    if (!msgs) { json_free(req); *resp = json_error("нет памяти"); return 500; }
    for (size_t i = 0; i < n; i++) {
        const json *m = json_at(arr, i);
        const char *role = json_str(json_get(m, "role"), NULL);
        const char *content = json_str(json_get(m, "content"), NULL);
        if (!role || !content || (strcmp(role, "user") && strcmp(role, "assistant"))) {
            free(msgs);
            json_free(req);
            *resp = json_error("каждое сообщение: {\"role\": \"user\"|\"assistant\", \"content\": строка}");
            return 400;
        }
        if (strlen(content) > MAX_MSG_LEN) {
            free(msgs);
            json_free(req);
            *resp = json_error("сообщение слишком длинное (максимум 200 КБ)");
            return 413;
        }
        msgs[i].role = role;
        msgs[i].content = content;
    }
    if (strcmp(msgs[n - 1].role, "user")) {
        free(msgs);
        json_free(req);
        *resp = json_error("последнее сообщение должно быть от пользователя");
        return 400;
    }
    const char *prefer = json_str(json_get(req, "model"), "auto");
    const char *skill = json_str(json_get(req, "skill"), NULL);

    router_reply rep;
    router_chat(ctx->rt, msgs, n, prefer, skill, &rep);
    *resp = router_reply_json(&rep);
    int code = rep.ok ? 200 : 503;
    router_reply_free(&rep);
    free(msgs);
    json_free(req);
    if (!*resp) { *resp = json_error("нет памяти"); return 500; }
    return code;
}

static int handle_reset(server_ctx *ctx, const char *path, char **resp) {
    /* /api/models/{id}/reset */
    const char *id = path + strlen("/api/models/");
    const char *end = strstr(id, "/reset");
    char buf[64];
    size_t len = end ? (size_t)(end - id) : 0;
    if (!end || !len || len >= sizeof buf) { *resp = json_error("не найдено"); return 404; }
    memcpy(buf, id, len);
    buf[len] = '\0';
    for (size_t i = 0; i < ctx->cfg->nmodels; i++)
        if (!strcmp(ctx->cfg->models[i].id, buf)) {
            quota_reset(ctx->quota, buf);
            LOGI("server", "счётчики и пауза модели %s сброшены", buf);
            *resp = xstrdup("{\"ok\":true}");
            return 200;
        }
    *resp = json_error("модель не найдена в каскаде");
    return 404;
}

/* ---------------------------------------------------------------- ключи ---- */

/* Ключ попадает в HTTP-заголовок — допускаем только видимые ASCII-символы (защита от подмены заголовков). */
static int key_is_valid(const char *k) {
    size_t n = strlen(k);
    if (n == 0 || n > 512) return 0;
    for (const unsigned char *p = (const unsigned char *)k; *p; p++)
        if (*p < 0x21 || *p > 0x7E) return 0;
    return 1;
}

static int provider_is_valid(const char *p) {
    size_t n = strlen(p);
    if (n == 0 || n >= 64) return 0;
    for (const unsigned char *c = (const unsigned char *)p; *c; c++)
        if (*c < 0x20) return 0;
    return 1;
}

/* Подсказка вида «…a1B2» — сам ключ наружу не отдаём никогда. */
static void key_hint(const char *k, char *out, size_t len) {
    size_t n = strlen(k);
    snprintf(out, len, "…%s", n > 4 ? k + n - 4 : "");
}

/* После смены ключа снимаем паузы моделей этого провайдера (например, после ошибки «неверный ключ»). */
static void on_key_changed(server_ctx *ctx, const char *provider) {
    for (size_t i = 0; i < ctx->cfg->nmodels; i++)
        if (!strcmp(ctx->cfg->models[i].key_name, provider)) quota_clear_cooldown(ctx->quota, ctx->cfg->models[i].id);
}

static int handle_keys_get(server_ctx *ctx, char **resp) {
    /* все провайдеры из каскада + сохранённые, которых в каскаде нет */
    static char names[256][64];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    int n = 0;
    for (size_t i = 0; i < ctx->cfg->nmodels && n < 256; i++) {
        const char *kn = ctx->cfg->models[i].key_name;
        if (!strcmp(ctx->cfg->models[i].provider, "mock")) continue;
        int dup = 0;
        for (int k = 0; k < n; k++) if (!strcmp(names[k], kn)) { dup = 1; break; }
        if (!dup) xstrlcpy(names[n++], kn, 64);
    }
    char stored[128][64];
    int ns = store_key_list(ctx->quota, stored, 128);
    for (int s = 0; s < ns && n < 256; s++) {
        int dup = 0;
        for (int k = 0; k < n; k++) if (!strcmp(names[k], stored[s])) { dup = 1; break; }
        if (!dup) xstrlcpy(names[n++], stored[s], 64);
    }

    strbuf b;
    sb_init(&b);
    sb_append(&b, "{\"ok\":true,\"keys\":[");
    for (int k = 0; k < n; k++) {
        char kb[600], hint[16] = "";
        const char *source = NULL;
        if (store_key_get(ctx->quota, names[k], kb, sizeof kb)) source = "settings";
        else {
            for (size_t i = 0; i < ctx->cfg->nmodels; i++) {
                const model_cfg *m = &ctx->cfg->models[i];
                const char *e;
                if (!strcmp(m->key_name, names[k]) && (e = config_api_key(m)) != NULL) { xstrlcpy(kb, e, sizeof kb); source = "env"; break; }
            }
        }
        if (source) key_hint(kb, hint, sizeof hint);
        if (k) sb_appendc(&b, ',');
        sb_append(&b, "{\"provider\":");
        sb_json_str(&b, names[k]);
        sb_printf(&b, ",\"set\":%s,\"source\":", source ? "true" : "false");
        sb_json_str(&b, source);
        sb_append(&b, ",\"hint\":");
        sb_json_str(&b, source ? hint : NULL);
        sb_append(&b, ",\"models\":[");
        int first = 1;
        for (size_t i = 0; i < ctx->cfg->nmodels; i++)
            if (!strcmp(ctx->cfg->models[i].key_name, names[k])) {
                if (!first) sb_appendc(&b, ',');
                first = 0;
                sb_json_str(&b, ctx->cfg->models[i].label);
            }
        sb_append(&b, "]}");
    }
    sb_append(&b, "]}");
    pthread_mutex_unlock(&mu);
    *resp = sb_take(&b);
    return 200;
}

static int handle_key_set(server_ctx *ctx, const char *body, char **resp) {
    json *j = json_parse(body ? body : "", NULL, 0);
    const char *provider = json_str(json_get(j, "provider"), NULL);
    const json *kj = json_get(j, "key");
    const char *key = json_str(kj, NULL);
    if (!provider || !provider_is_valid(provider) || (kj && kj->type != JSON_STRING && kj->type != JSON_NULL)) {
        json_free(j);
        *resp = json_error("нужно {\"provider\": \"Groq\", \"key\": \"...\"}; пустой key удаляет ключ");
        return 400;
    }
    if (key && *key && !key_is_valid(key)) {
        json_free(j);
        *resp = json_error("ключ содержит недопустимые символы (пробелы, переводы строк) или длиннее 512 символов");
        return 400;
    }
    int rc = store_key_set(ctx->quota, provider, key);
    if (rc == 0) on_key_changed(ctx, provider);
    strbuf b;
    sb_init(&b);
    if (rc == 0) {
        char hint[16] = "";
        if (key && *key) key_hint(key, hint, sizeof hint);
        sb_append(&b, "{\"ok\":true,\"provider\":");
        sb_json_str(&b, provider);
        sb_printf(&b, ",\"set\":%s,\"hint\":", key && *key ? "true" : "false");
        sb_json_str(&b, key && *key ? hint : NULL);
        sb_append(&b, "}");
        LOGI("keys", "ключ «%s» %s через «Настройки»", provider, key && *key ? "сохранён" : "удалён");
    }
    json_free(j);
    if (rc != 0) { sb_free(&b); *resp = json_error("не удалось сохранить ключ"); return 500; }
    *resp = sb_take(&b);
    return 200;
}

static int handle_keys_bulk(server_ctx *ctx, const char *body, char **resp) {
    json *j = json_parse(body ? body : "", NULL, 0);
    const json *keys = json_get(j, "keys");
    if (!keys || keys->type != JSON_OBJECT) {
        json_free(j);
        *resp = json_error("нужно {\"keys\": {\"Groq\": \"...\", ...}}");
        return 400;
    }
    int saved = 0, skipped = 0;
    for (const json *c = keys->child; c; c = c->next) {
        const char *k = json_str(c, NULL);
        if (!c->key || !provider_is_valid(c->key) || !k || !key_is_valid(k)) { skipped++; continue; }
        char cur[600];
        if (store_key_get(ctx->quota, c->key, cur, sizeof cur) && !strcmp(cur, k)) continue; /* без изменений */
        if (store_key_set(ctx->quota, c->key, k) == 0) { saved++; on_key_changed(ctx, c->key); }
    }
    json_free(j);
    if (saved) LOGI("keys", "синхронизировано ключей из «Настроек»: %d", saved);
    strbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"ok\":true,\"saved\":%d,\"skipped\":%d}", saved, skipped);
    *resp = sb_take(&b);
    return 200;
}

static int handle_settings(server_ctx *ctx, const char *method, const char *body, char **resp) {
    if (!strcmp(method, "POST") || !strcmp(method, "PUT")) {
        char err[200];
        if (settings_apply_json(ctx->quota, body, err, sizeof err) != 0) { *resp = json_error(err); return 400; }
    } else if (strcmp(method, "GET")) {
        *resp = json_error("используйте GET или POST");
        return 405;
    }
    strbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"ok\":true,\"encryption_available\":%s,\"settings\":", crypto_available() ? "true" : "false");
    settings_to_json(&b);
    sb_append(&b, "}");
    *resp = sb_take(&b);
    return 200;
}

int server_handle(server_ctx *ctx, const char *method, const char *path, const char *body, int is_local, char **resp) {
    *resp = NULL;
    char clean[256];
    xstrlcpy(clean, path, sizeof clean);
    char *qm = strchr(clean, '?');
    if (qm) *qm = '\0';

    if (!strcmp(clean, "/api/health") && !strcmp(method, "GET")) return handle_health(ctx, resp);
    if (!strcmp(clean, "/api/models") && !strcmp(method, "GET")) return handle_models(ctx, resp);
    if (!strcmp(clean, "/api/chat")) {
        if (strcmp(method, "POST")) { *resp = json_error("используйте POST"); return 405; }
        return handle_chat(ctx, body, resp);
    }
    if (!strncmp(clean, "/api/models/", 12) && !strcmp(method, "POST")) return handle_reset(ctx, clean, resp);
    if (!strcmp(clean, "/api/settings")) {
        if (!is_local) { *resp = json_error("менять настройки можно только с этого компьютера"); return 403; }
        return handle_settings(ctx, method, body, resp);
    }
    if (!strncmp(clean, "/api/keys", 9)) {
        if (!is_local) { *resp = json_error("управлять ключами можно только с этого компьютера"); return 403; }
        if (!strcmp(clean, "/api/keys") && !strcmp(method, "GET")) return handle_keys_get(ctx, resp);
        if (!strcmp(clean, "/api/keys") && (!strcmp(method, "POST") || !strcmp(method, "PUT"))) return handle_key_set(ctx, body, resp);
        if (!strcmp(clean, "/api/keys/bulk") && !strcmp(method, "POST")) return handle_keys_bulk(ctx, body, resp);
        *resp = json_error("не найдено");
        return 404;
    }
    *resp = json_error("не найдено");
    return 404;
}

/* ------------------------------------------------------------ HTTP/сокеты ---- */

static const char *status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
    }
    return "OK";
}

static int send_all(sock_t s, const char *data, size_t len) {
    while (len) {
        int n = send(s, data, (int)(len > 65536 ? 65536 : len), SEND_FLAGS);
        if (n <= 0) return -1;
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

static void send_response(sock_t s, const app_config *cfg, int code, const char *body) {
    strbuf h;
    sb_init(&h);
    size_t blen = body ? strlen(body) : 0;
    sb_printf(&h, "HTTP/1.1 %d %s\r\n", code, status_text(code));
    if (blen) sb_append(&h, "Content-Type: application/json; charset=utf-8\r\n");
    sb_printf(&h, "Content-Length: %zu\r\n", blen);
    sb_printf(&h, "Access-Control-Allow-Origin: %s\r\n", cfg->cors_origin);
    sb_append(&h, "Access-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\n"
                   "Access-Control-Allow-Headers: Content-Type\r\n"
                   "Access-Control-Max-Age: 600\r\n"
                   "Cache-Control: no-store\r\n"
                   "X-Content-Type-Options: nosniff\r\n"
                   "Connection: close\r\n\r\n");
    if (!h.oom) {
        send_all(s, h.data, h.len);
        if (blen) send_all(s, body, blen);
    }
    sb_free(&h);
}

/* Значение заголовка (без учёта регистра имени). 1 — найден. */
static int header_value(const char *headers, const char *name, char *out, size_t len) {
    size_t nl = strlen(name);
    const char *p = headers;
    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t n = strcspn(v, "\r\n");
            if (n >= len) n = len - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return 1;
        }
    }
    return 0;
}

/* Изменяющие запросы принимаем только от своего фронтенда: защищает от сайтов, которые пытаются
 * из браузера пользователя тратить лимиты или подменять ключи на 127.0.0.1. */
static int origin_allowed(const app_config *cfg, const char *origin) {
    if (!origin || !*origin) return 1; /* не браузер (curl, тесты) */
    if (!strcmp(cfg->cors_origin, "*") || !strcmp(origin, cfg->cors_origin)) return 1;
    char self1[64], self2[64];
    snprintf(self1, sizeof self1, "http://localhost:%d", cfg->port);
    snprintf(self2, sizeof self2, "http://127.0.0.1:%d", cfg->port);
    return !strcmp(origin, self1) || !strcmp(origin, self2);
}

static long find_content_length(const char *headers) {
    const char *p = headers;
    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        if (!strncasecmp(p, "content-length:", 15)) return strtol(p + 15, NULL, 10);
    }
    return 0;
}

typedef struct {
    server_ctx *ctx;
    sock_t      sock;
    int         is_local; /* клиент с 127.0.0.0/8 */
} conn_arg;

static void *conn_thread(void *argp) {
    conn_arg *arg = argp;
    sock_t s = arg->sock;
    server_ctx *ctx = arg->ctx;
    int arg_is_local = arg->is_local;
    free(arg);
    plat_sock_timeouts(s, IO_TIMEOUT_MS);

    strbuf in;
    sb_init(&in);
    char buf[8192];
    char *hdr_end = NULL;
    int code = 0;
    char *resp = NULL;

    /* заголовки */
    while (!hdr_end) {
        int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) goto done;
        sb_append_n(&in, buf, (size_t)n);
        hdr_end = strstr(in.data, "\r\n\r\n");
        if (!hdr_end && in.len > MAX_HEADER) { code = 431; resp = json_error("слишком большие заголовки"); goto reply; }
    }
    size_t hdr_len = (size_t)(hdr_end - in.data) + 4;
    long clen = find_content_length(in.data);
    if (clen < 0 || clen > MAX_BODY) { code = 413; resp = json_error("слишком большое тело запроса"); goto reply; }
    while (in.len < hdr_len + (size_t)clen) {
        int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) goto done;
        sb_append_n(&in, buf, (size_t)n);
    }
    if (in.oom) { code = 500; resp = json_error("нет памяти"); goto reply; }

    {
        char method[8] = "", path[256] = "";
        if (sscanf(in.data, "%7s %255s", method, path) != 2) { code = 400; resp = json_error("некорректный запрос"); goto reply; }
        in.data[hdr_len + (size_t)clen] = '\0';
        const char *body = in.data + hdr_len;
        if (!strcmp(method, "OPTIONS")) { code = 204; goto reply; } /* CORS preflight */
        char origin[256] = "";
        in.data[hdr_len - 2] = '\0'; /* временно ограничиваем поиск заголовками */
        header_value(in.data, "origin", origin, sizeof origin);
        in.data[hdr_len - 2] = '\r';
        if (strcmp(method, "GET") && !origin_allowed(ctx->cfg, origin)) {
            LOGW("http", "отклонён %s %s с чужого Origin: %s", method, path, origin);
            code = 403;
            resp = json_error("запрос с чужого сайта отклонён");
            goto reply;
        }
        int64_t t0 = plat_now_ms();
        code = server_handle(ctx, method, path, body, arg_is_local, &resp);
        LOGD("http", "%s %s → %d (%lld мс)", method, path, code, (long long)(plat_now_ms() - t0));
    }

reply:
    send_response(s, ctx->cfg, code, resp);
done:
    free(resp);
    sb_free(&in);
    sock_close(s);
    __sync_fetch_and_sub(&g_clients, 1);
    return NULL;
}

void server_stop(void) {
    g_running = 0;
    sock_t l = g_listen;
    if (l != SOCK_INVALID) {
        g_listen = SOCK_INVALID;
#ifdef _WIN32
        closesocket(l);
#else
        shutdown(l, SHUT_RDWR);
        close(l);
#endif
    }
}

#ifdef _WIN32
static BOOL WINAPI on_ctrl(DWORD type) { (void)type; server_stop(); return TRUE; }
#else
static void on_signal(int sig) { (void)sig; server_stop(); }
#endif

int server_run(server_ctx *ctx) {
    sock_t l = socket(AF_INET, SOCK_STREAM, 0);
    if (l == SOCK_INVALID) { LOGE("server", "socket() не удался"); return -1; }
    int yes = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)ctx->cfg->port);
    if (inet_pton(AF_INET, ctx->cfg->host, &addr.sin_addr) != 1) {
        LOGE("server", "некорректный host \"%s\" (нужен IPv4, например 127.0.0.1 или 0.0.0.0)", ctx->cfg->host);
        sock_close(l);
        return -1;
    }
    if (bind(l, (struct sockaddr *)&addr, sizeof addr) != 0) {
        LOGE("server", "не удалось занять %s:%d — порт занят?", ctx->cfg->host, ctx->cfg->port);
        sock_close(l);
        return -1;
    }
    if (listen(l, 64) != 0) { LOGE("server", "listen() не удался"); sock_close(l); return -1; }
    g_listen = l;
    g_running = 1;

#ifdef _WIN32
    SetConsoleCtrlHandler(on_ctrl, TRUE);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; /* без SA_RESTART, чтобы accept() прервался */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#endif

    LOGI("server", "Слушаю http://%s:%d  (Ctrl+C — остановить)", ctx->cfg->host, ctx->cfg->port);
    while (g_running) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof cli;
        sock_t c = accept(l, (struct sockaddr *)&cli, &cl);
        if (c == SOCK_INVALID) {
            if (!g_running) break;
            continue;
        }
        if (g_clients >= MAX_CLIENTS) {
            char *busy = json_error("сервер перегружен, повторите позже");
            send_response(c, ctx->cfg, 503, busy);
            free(busy);
            sock_close(c);
            continue;
        }
        conn_arg *arg = malloc(sizeof *arg);
        if (!arg) { sock_close(c); continue; }
        arg->ctx = ctx;
        arg->sock = c;
        arg->is_local = (ntohl(cli.sin_addr.s_addr) >> 24) == 127;
        __sync_fetch_and_add(&g_clients, 1);
        pthread_t th;
        if (pthread_create(&th, NULL, conn_thread, arg) != 0) {
            __sync_fetch_and_sub(&g_clients, 1);
            sock_close(c);
            free(arg);
            continue;
        }
        pthread_detach(th);
    }
    /* даём активным запросам завершиться (до 5 секунд) */
    for (int i = 0; i < 50 && g_clients > 0; i++) {
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
    }
    LOGI("server", "Остановлен");
    return 0;
}
