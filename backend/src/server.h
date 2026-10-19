/* Минимальный HTTP/1.1-сервер для API фронтенда. */
#ifndef IAI_SERVER_H
#define IAI_SERVER_H

#include "config.h"
#include "quota.h"
#include "router.h"

typedef struct {
    const app_config *cfg;
    router           *rt;
    quota_db         *quota;
} server_ctx;

/* Запустить сервер (блокирует до server_stop / Ctrl+C). 0 — штатная остановка. */
int  server_run(server_ctx *ctx);
void server_stop(void);

/* Обработка одного запроса — вынесена для тестов без сети.
 * is_local — запрос пришёл с этого же компьютера (только так можно менять API-ключи).
 * Возвращает HTTP-код, тело ответа (malloc) пишет в *resp. */
int  server_handle(server_ctx *ctx, const char *method, const char *path, const char *body, int is_local, char **resp);

#endif
