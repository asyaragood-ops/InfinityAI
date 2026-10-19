/* Общий интерфейс провайдеров ИИ и классификация ошибок.
 *
 * В C нет try/catch, поэтому каждый вызов провайдера возвращает call_result с полем kind —
 * это «пойманное исключение» в явном виде. Роутер по kind решает, переключаться ли на следующий ИИ. */
#ifndef IAI_PROVIDER_H
#define IAI_PROVIDER_H

#include <stddef.h>

#include "../config.h"

typedef enum {
    RES_OK = 0,
    RES_RATE_LIMIT,     /* 429: слишком много запросов (временно) */
    RES_QUOTA,          /* квота/бесплатный лимит исчерпан (429 quota, RESOURCE_EXHAUSTED, insufficient_quota, 402) */
    RES_AUTH,           /* 401/403, неверный ключ */
    RES_BAD_REQUEST,    /* 400/404/422 — запрос не принят этой моделью */
    RES_SERVER,         /* 5xx, 529 overloaded */
    RES_NETWORK,        /* нет ответа: DNS, таймаут, TLS */
    RES_BAD_RESPONSE,   /* ответ пришёл, но разобрать его не удалось / пустой */
    RES_NOT_CONFIGURED, /* нет API-ключа в окружении */
    RES_LOCAL_LIMIT,    /* исчерпан локально отслеживаемый лимит */
    RES_COOLDOWN,       /* модель на паузе после прошлых ошибок */
    RES_DISABLED        /* модель выключена в конфиге */
} result_kind;

/* Короткий код для JSON/логов: "ok", "rate_limit", "quota", ... */
const char *result_kind_code(result_kind k);
/* Это ошибка лимита (повод для сообщения «Основной лимит исчерпан...»)? */
int result_is_limit(result_kind k);

typedef struct {
    const char *role;    /* "user" | "assistant" */
    const char *content;
} chat_msg;

typedef struct {
    const chat_msg *msgs;
    size_t          n;
    const char     *system; /* может быть NULL или "" */
} chat_request;

typedef struct {
    result_kind kind;
    long        http_status;
    long        retry_after_s; /* из Retry-After или retryDelay в теле; 0 — неизвестно */
    int         limit_daily;   /* признаки суточного лимита (PerDay / per day / daily) */
    int         limit_billing; /* закончились деньги/кредиты на платном аккаунте */
    char       *text;        /* malloc; ответ модели при RES_OK */
    long        tokens_in;
    long        tokens_out;
    char        error[512];  /* описание ошибки для лога и фронтенда */
} call_result;

void call_result_free(call_result *r);

/* Сигнатура адаптера провайдера. */
typedef void (*provider_call_fn)(const model_cfg *m, const char *api_key, const chat_request *req, long timeout_ms,
                                 call_result *out);

provider_call_fn provider_for(const model_cfg *m);

/* Адаптеры */
void provider_gemini(const model_cfg *m, const char *api_key, const chat_request *req, long timeout_ms, call_result *out);
void provider_openai(const model_cfg *m, const char *api_key, const chat_request *req, long timeout_ms, call_result *out);
void provider_anthropic(const model_cfg *m, const char *api_key, const chat_request *req, long timeout_ms, call_result *out);
void provider_mock(const model_cfg *m, const char *api_key, const chat_request *req, long timeout_ms, call_result *out);

/* Общая классификация HTTP-ответа с ошибкой. body — тело ответа (для поиска признаков квоты). */
result_kind classify_http_error(long status, const char *body);

/* Заполнить поля ошибки call_result из HTTP-ответа (kind, http_status, retry_after_s, limit_daily, error). */
void fill_http_error(call_result *out, long status, const char *body, long header_retry_after, const char *net_error);

/* Достать человекочитаемое сообщение об ошибке из типичного JSON провайдера. */
void extract_error_message(const char *body, char *out, size_t outlen);

#endif
