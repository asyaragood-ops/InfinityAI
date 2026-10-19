/* Локальный учёт лимитов в SQLite: счётчики запросов/токенов по окнам, паузы (cooldown) после 429,
 * журнал попыток каскада. Позволяет переключаться заранее, не дожидаясь ошибки от провайдера. */
#ifndef IAI_QUOTA_H
#define IAI_QUOTA_H

#include <stdint.h>

#include "config.h"

typedef struct quota_db quota_db;

typedef enum {
    QUOTA_OK = 0,
    QUOTA_LOCAL_LIMIT, /* исчерпан локально отслеживаемый лимит (rpm/rpd/rpmo/tpd) */
    QUOTA_COOLDOWN     /* модель на паузе после 429/ошибок */
} quota_state;

typedef struct {
    int     req_minute, req_day, req_month;
    long    tokens_day;
    int64_t cooldown_until; /* 0 — нет паузы */
    char    cooldown_reason[128];
} quota_status;

quota_db *quota_open(const char *path, char *err, size_t errlen);
void      quota_close(quota_db *q);

/* Можно ли сейчас вызвать модель. threshold_pct (50–100) — переключаться заранее, когда израсходовано
 * столько процентов локального лимита. reason — человекочитаемая причина отказа. */
quota_state quota_check(quota_db *q, const model_cfg *m, int64_t now, int threshold_pct, char *reason, size_t rlen);

/* Загруженность модели по локальным лимитам, 0–100 (максимум по минуте/суткам/месяцу/токенам).
 * -1 — лимиты не заданы. Нужна стратегии «Свободнее». */
int quota_load_pct(quota_db *q, const model_cfg *m, int64_t now);

/* Учесть успешный запрос. */
void quota_record(quota_db *q, const model_cfg *m, int64_t now, long tokens);

/* Поставить модель на паузу до until (unix-время). */
void quota_set_cooldown(quota_db *q, const char *model_id, int64_t until, const char *reason);

/* Снять паузу и обнулить счётчики модели (для админ-эндпоинта/тестов). */
void quota_reset(quota_db *q, const char *model_id);

/* Текущее состояние для /api/models. */
void quota_get_status(quota_db *q, const model_cfg *m, int64_t now, quota_status *st);

/* Запись в журнал попыток. */
void quota_log_attempt(quota_db *q, int64_t now, const char *request_id, const char *model_id, const char *kind,
                       long http_status, const char *message);

/* Снять только паузу (счётчики не трогаем) — например, после смены ключа. */
void quota_clear_cooldown(quota_db *q, const char *model_id);

/* ---- API-ключи из «Настроек» сайта (хранятся в той же БД) ---- */
/* Сохранить ключ провайдера; key == NULL или "" — удалить. 0 — успех. */
int  store_key_set(quota_db *q, const char *provider, const char *key);
/* Прочитать ключ. 1 — найден (записан в buf), 0 — нет. */
int  store_key_get(quota_db *q, const char *provider, char *buf, size_t len);
/* Имена провайдеров с сохранёнными ключами. Возвращает количество (не больше max). */
int  store_key_list(quota_db *q, char names[][64], int max);

/* Шифровать ли новые ключи. migrate=1 — перезаписать все сохранённые ключи в новом виде.
 * Возвращает число обработанных ключей или -1. */
int  quota_set_encryption(quota_db *q, int on, int migrate);
/* Прочитать сырое значение ключа как оно лежит в БД (для тестов и диагностики). */
int  store_key_get_raw(quota_db *q, const char *provider, char *buf, size_t len);

/* Простое хранилище «имя → значение» (настройки роутера). kv_get возвращает malloc-строку или NULL. */
char *quota_kv_get(quota_db *q, const char *name);
void  quota_kv_set(quota_db *q, const char *name, const char *value);

/* Начало следующих суток UTC после now — когда сбрасываются суточные лимиты. */
int64_t quota_next_day(int64_t now);

#endif
