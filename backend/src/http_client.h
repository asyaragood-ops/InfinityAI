/* HTTPS-клиент на libcurl для запросов к провайдерам ИИ. */
#ifndef IAI_HTTP_CLIENT_H
#define IAI_HTTP_CLIENT_H

#include "strbuf.h"

typedef struct {
    long   status;          /* HTTP-код; 0 — запрос не дошёл (сеть/таймаут) */
    strbuf body;
    long   retry_after_s;   /* из заголовка Retry-After (секунды), 0 — нет */
    char   error[256];      /* описание сетевой ошибки */
} http_response;

int  http_global_init(void);
void http_global_cleanup(void);

/* POST с JSON-телом. headers — массив строк "Name: value". Возвращает 0, если получен любой HTTP-ответ. */
int  http_post_json(const char *url, const char *const *headers, int nheaders, const char *body, long timeout_ms,
                    http_response *out);
void http_response_free(http_response *r);

#endif
