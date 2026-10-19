/* Тестовый провайдер без сети: позволяет проверить каскад без ключей.
 * mock_behavior: ok | rate_limit | quota | daily_quota | server_error | auth_error | network */
#include <stdio.h>
#include <string.h>

#include "../strbuf.h"
#include "provider.h"

void provider_mock(const model_cfg *m, const char *api_key, const chat_request *req, long timeout_ms, call_result *out) {
    (void)api_key;
    (void)timeout_ms;
    memset(out, 0, sizeof *out);
    const char *b = m->mock_behavior;
    if (!strcmp(b, "rate_limit")) {
        fill_http_error(out, 429, "{\"error\":{\"message\":\"Rate limit reached (mock)\"}}", 7, NULL);
        return;
    }
    if (!strcmp(b, "quota")) {
        fill_http_error(out, 429,
                        "{\"error\":{\"code\":429,\"message\":\"You exceeded your current quota (mock)\",\"status\":\"RESOURCE_EXHAUSTED\"}}",
                        0, NULL);
        return;
    }
    if (!strcmp(b, "daily_quota")) {
        fill_http_error(out, 429,
                        "{\"error\":{\"message\":\"Quota exceeded: GenerateRequestsPerDayPerProjectPerModel-FreeTier (mock)\"}}", 0,
                        NULL);
        return;
    }
    if (!strcmp(b, "server_error")) { fill_http_error(out, 503, "{\"error\":{\"message\":\"Service unavailable (mock)\"}}", 0, NULL); return; }
    if (!strcmp(b, "auth_error")) { fill_http_error(out, 401, "{\"error\":{\"message\":\"Invalid API key (mock)\"}}", 0, NULL); return; }
    if (!strcmp(b, "network")) { fill_http_error(out, 0, "", 0, "connection refused (mock)"); return; }

    const char *last = req->n ? req->msgs[req->n - 1].content : "";
    strbuf sb;
    sb_init(&sb);
    sb_printf(&sb, "[%s] Тестовый ответ на: «%.200s» (сообщений: %zu)", m->label, last, req->n);
    out->kind = RES_OK;
    out->http_status = 200;
    out->text = sb_take(&sb);
    out->tokens_in = (long)strlen(last) / 4 + 1;
    out->tokens_out = 12;
}
