#include "provider.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../json.h"
#include "../strbuf.h"

const char *result_kind_code(result_kind k) {
    switch (k) {
        case RES_OK: return "ok";
        case RES_RATE_LIMIT: return "rate_limit";
        case RES_QUOTA: return "quota";
        case RES_AUTH: return "auth";
        case RES_BAD_REQUEST: return "bad_request";
        case RES_SERVER: return "server_error";
        case RES_NETWORK: return "network";
        case RES_BAD_RESPONSE: return "bad_response";
        case RES_NOT_CONFIGURED: return "no_api_key";
        case RES_LOCAL_LIMIT: return "local_limit";
        case RES_COOLDOWN: return "cooldown";
        case RES_DISABLED: return "disabled";
    }
    return "unknown";
}

int result_is_limit(result_kind k) { return k == RES_RATE_LIMIT || k == RES_QUOTA || k == RES_LOCAL_LIMIT || k == RES_COOLDOWN; }

void call_result_free(call_result *r) {
    free(r->text);
    r->text = NULL;
}

provider_call_fn provider_for(const model_cfg *m) {
    if (!strcmp(m->provider, "gemini")) return provider_gemini;
    if (!strcmp(m->provider, "openai")) return provider_openai;
    if (!strcmp(m->provider, "anthropic")) return provider_anthropic;
    if (!strcmp(m->provider, "mock")) return provider_mock;
    return NULL;
}

/* Поиск подстроки без учёта регистра (ASCII). */
static int contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle) return 0;
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

result_kind classify_http_error(long status, const char *body) {
    if (status == 0) return RES_NETWORK;
    if (status == 429) {
        /* Отличаем «исчерпана квота/бесплатный лимит» от кратковременного rate limit:
         *  Google: status RESOURCE_EXHAUSTED + "quota"; OpenAI: code insufficient_quota;
         *  Groq/OpenRouter: "per day"/"daily". */
        if (contains_ci(body, "insufficient_quota") || contains_ci(body, "quota") || contains_ci(body, "per day") ||
            contains_ci(body, "daily") || contains_ci(body, "billing"))
            return RES_QUOTA;
        return RES_RATE_LIMIT;
    }
    if (status == 402) return RES_QUOTA;                           /* payment required */
    if (status == 401 || status == 403) return RES_AUTH;
    if (status == 400 && (contains_ci(body, "api key") || contains_ci(body, "api_key"))) return RES_AUTH; /* Gemini */
    if (status == 400 && contains_ci(body, "credit balance")) return RES_QUOTA; /* Anthropic: нет средств */
    if (status == 408) return RES_NETWORK;
    if (status >= 500) return RES_SERVER;                          /* в т.ч. 529 overloaded */
    if (status >= 400) return RES_BAD_REQUEST;
    return RES_BAD_RESPONSE;
}

void extract_error_message(const char *body, char *out, size_t outlen) {
    out[0] = '\0';
    json *j = json_parse(body, NULL, 0);
    if (j) {
        const char *msg = json_str(json_path(j, "error.message"), NULL);  /* OpenAI/Gemini/Anthropic */
        if (!msg) msg = json_str(json_get(j, "message"), NULL);
        if (!msg) msg = json_str(json_get(j, "error"), NULL);
        if (!msg) msg = json_str(json_path(j, "0.error.message"), NULL);  /* Gemini иногда отдаёт массив */
        if (msg) xstrlcpy(out, msg, outlen);
        json_free(j);
    }
    if (!out[0] && body) {
        /* не JSON — берём начало тела, без переводов строк */
        xstrlcpy(out, body, outlen > 200 ? 200 : outlen);
        for (char *p = out; *p; p++)
            if (*p == '\n' || *p == '\r') *p = ' ';
    }
}

/* Google присылает задержку в теле: "retryDelay": "34s". */
static long parse_retry_delay(const char *body) {
    const char *p = body ? strstr(body, "\"retryDelay\"") : NULL;
    if (!p) return 0;
    p = strchr(p + 12, '"');
    if (!p) return 0;
    p++;
    long v = strtol(p, NULL, 10);
    return v > 0 ? v : 0;
}

void fill_http_error(call_result *out, long status, const char *body, long header_retry_after, const char *net_error) {
    out->http_status = status;
    out->kind = classify_http_error(status, body);
    out->retry_after_s = header_retry_after > 0 ? header_retry_after : parse_retry_delay(body);
    out->limit_daily = contains_ci(body, "PerDay") || contains_ci(body, "per day") || contains_ci(body, "daily");
    /* Google пишет «check your plan and billing details» даже на минутном лимите — поэтому «billing» не используем */
    out->limit_billing = status == 402 || contains_ci(body, "insufficient_quota") || contains_ci(body, "credit balance");
    if (status == 0) {
        snprintf(out->error, sizeof out->error, "нет ответа: %s", net_error && *net_error ? net_error : "сетевая ошибка");
        return;
    }
    char msg[400];
    extract_error_message(body, msg, sizeof msg);
    snprintf(out->error, sizeof out->error, "HTTP %ld: %s", status, msg[0] ? msg : "без описания");
}
