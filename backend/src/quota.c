#include "quota.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/crypto.h>
#include <sqlite3.h>

#include "crypto.h"
#include "log.h"
#include "platform.h"
#include "strbuf.h"

struct quota_db {
    sqlite3        *db;
    pthread_mutex_t mu; /* одно соединение на процесс, доступ сериализуем */
    int             encrypt; /* шифровать новые ключи */
};

static const char *SCHEMA =
    "PRAGMA journal_mode=WAL;"
    "CREATE TABLE IF NOT EXISTS usage("
    "  model_id TEXT NOT NULL, period TEXT NOT NULL, bucket TEXT NOT NULL,"
    "  requests INTEGER NOT NULL DEFAULT 0, tokens INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY(model_id, period, bucket));"
    "CREATE TABLE IF NOT EXISTS cooldown("
    "  model_id TEXT PRIMARY KEY, until_ts INTEGER NOT NULL, reason TEXT);"
    "CREATE TABLE IF NOT EXISTS attempts("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL, request_id TEXT, model_id TEXT,"
    "  kind TEXT, http_status INTEGER, message TEXT);"
    "CREATE INDEX IF NOT EXISTS attempts_ts ON attempts(ts);"
    "CREATE TABLE IF NOT EXISTS api_keys("
    "  provider TEXT PRIMARY KEY, api_key TEXT NOT NULL, updated_ts INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS kv(name TEXT PRIMARY KEY, value TEXT NOT NULL);";

/* Ключи окон в UTC: минута, сутки, месяц. */
static void buckets(int64_t now, char *minute, char *day, char *month) {
    struct tm tm;
    plat_gmtime((time_t)now, &tm);
    snprintf(minute, 32, "%04d%02d%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    snprintf(day, 32, "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    snprintf(month, 32, "%04d%02d", tm.tm_year + 1900, tm.tm_mon + 1);
}

int64_t quota_next_day(int64_t now) { return (now / 86400 + 1) * 86400; }

quota_db *quota_open(const char *path, char *err, size_t errlen) {
    /* создаём каталог для файла БД */
    char dir[256];
    xstrlcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    char *bslash = strrchr(dir, '\\');
    if (bslash > slash) slash = bslash;
    if (slash && slash != dir) { *slash = '\0'; plat_mkdirs(dir); }

    quota_db *q = calloc(1, sizeof *q);
    if (!q) { snprintf(err, errlen, "нет памяти"); return NULL; }
    if (sqlite3_open(path, &q->db) != SQLITE_OK) {
        snprintf(err, errlen, "SQLite: не удалось открыть %s: %s", path, sqlite3_errmsg(q->db));
        sqlite3_close(q->db);
        free(q);
        return NULL;
    }
    sqlite3_busy_timeout(q->db, 3000);
    char *e = NULL;
    if (sqlite3_exec(q->db, SCHEMA, NULL, NULL, &e) != SQLITE_OK) {
        snprintf(err, errlen, "SQLite: %s", e ? e : "ошибка схемы");
        sqlite3_free(e);
        sqlite3_close(q->db);
        free(q);
        return NULL;
    }
    pthread_mutex_init(&q->mu, NULL);
    return q;
}

void quota_close(quota_db *q) {
    if (!q) return;
    sqlite3_close(q->db);
    pthread_mutex_destroy(&q->mu);
    free(q);
}

static void get_usage(quota_db *q, const char *id, const char *period, const char *bucket, int *req, long *tok) {
    *req = 0;
    if (tok) *tok = 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "SELECT requests, tokens FROM usage WHERE model_id=? AND period=? AND bucket=?", -1, &st,
                           NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, period, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, bucket, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *req = sqlite3_column_int(st, 0);
        if (tok) *tok = (long)sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
}

static int64_t get_cooldown(quota_db *q, const char *id, char *reason, size_t rlen) {
    int64_t until = 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "SELECT until_ts, reason FROM cooldown WHERE model_id=?", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        until = sqlite3_column_int64(st, 0);
        if (reason) xstrlcpy(reason, (const char *)sqlite3_column_text(st, 1), rlen);
    }
    sqlite3_finalize(st);
    return until;
}

/* Порог с учётом процента: 20 запросов при 90% → 18. Не меньше 1. */
static long eff_limit(long limit, int pct) {
    if (pct >= 100 || pct <= 0) return limit;
    long v = limit * pct / 100;
    return v < 1 ? 1 : v;
}

quota_state quota_check(quota_db *q, const model_cfg *m, int64_t now, int pct, char *reason, size_t rlen) {
    char mi[32], d[32], mo[32];
    buckets(now, mi, d, mo);
    quota_state res = QUOTA_OK;
    pthread_mutex_lock(&q->mu);
    char cr[128] = "";
    int64_t until = get_cooldown(q, m->id, cr, sizeof cr);
    if (until > now) {
        snprintf(reason, rlen, "пауза ещё %lld с: %s", (long long)(until - now), cr);
        res = QUOTA_COOLDOWN;
    } else {
        int r;
        long t;
        const limits_cfg *L = &m->limits;
        /* минутный лимит — всегда целиком (порог к нему не применяем: он сбрасывается через минуту) */
        if (L->rpm > 0) {
            get_usage(q, m->id, "m", mi, &r, NULL);
            if (r >= L->rpm) { snprintf(reason, rlen, "локальный лимит %d запросов/мин", L->rpm); res = QUOTA_LOCAL_LIMIT; }
        }
        if (res == QUOTA_OK && (L->rpd > 0 || L->tpd > 0)) {
            get_usage(q, m->id, "d", d, &r, &t);
            if (L->rpd > 0 && r >= eff_limit(L->rpd, pct)) {
                snprintf(reason, rlen, "израсходовано %d из %d запросов/сутки (порог %d%%)", r, L->rpd, pct);
                res = QUOTA_LOCAL_LIMIT;
            } else if (L->tpd > 0 && t >= eff_limit(L->tpd, pct)) {
                snprintf(reason, rlen, "израсходовано %ld из %ld токенов/сутки (порог %d%%)", t, L->tpd, pct);
                res = QUOTA_LOCAL_LIMIT;
            }
        }
        if (res == QUOTA_OK && L->rpmo > 0) {
            get_usage(q, m->id, "mo", mo, &r, NULL);
            if (r >= eff_limit(L->rpmo, pct)) {
                snprintf(reason, rlen, "израсходовано %d из %d запросов/месяц (порог %d%%)", r, L->rpmo, pct);
                res = QUOTA_LOCAL_LIMIT;
            }
        }
    }
    pthread_mutex_unlock(&q->mu);
    return res;
}

int quota_load_pct(quota_db *q, const model_cfg *m, int64_t now) {
    const limits_cfg *L = &m->limits;
    if (!L->rpm && !L->rpd && !L->rpmo && !L->tpd) return -1;
    char mi[32], d[32], mo[32];
    buckets(now, mi, d, mo);
    int r, best = 0;
    long t;
    pthread_mutex_lock(&q->mu);
    if (L->rpm) { get_usage(q, m->id, "m", mi, &r, NULL); if (r * 100 / L->rpm > best) best = r * 100 / L->rpm; }
    get_usage(q, m->id, "d", d, &r, &t);
    if (L->rpd && r * 100 / L->rpd > best) best = r * 100 / L->rpd;
    if (L->tpd && (int)(t * 100 / L->tpd) > best) best = (int)(t * 100 / L->tpd);
    if (L->rpmo) { get_usage(q, m->id, "mo", mo, &r, NULL); if (r * 100 / L->rpmo > best) best = r * 100 / L->rpmo; }
    pthread_mutex_unlock(&q->mu);
    return best > 100 ? 100 : best;
}

static void bump(quota_db *q, const char *id, const char *period, const char *bucket, long tokens) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db,
                           "INSERT INTO usage(model_id, period, bucket, requests, tokens) VALUES(?,?,?,1,?) "
                           "ON CONFLICT(model_id, period, bucket) DO UPDATE SET requests=requests+1, tokens=tokens+excluded.tokens",
                           -1, &st, NULL) != SQLITE_OK) {
        LOGE("quota", "SQLite: %s", sqlite3_errmsg(q->db));
        return;
    }
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, period, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, bucket, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, tokens);
    if (sqlite3_step(st) != SQLITE_DONE) LOGE("quota", "SQLite: %s", sqlite3_errmsg(q->db));
    sqlite3_finalize(st);
}

void quota_record(quota_db *q, const model_cfg *m, int64_t now, long tokens) {
    char mi[32], d[32], mo[32];
    buckets(now, mi, d, mo);
    pthread_mutex_lock(&q->mu);
    sqlite3_exec(q->db, "BEGIN", NULL, NULL, NULL);
    bump(q, m->id, "m", mi, tokens);
    bump(q, m->id, "d", d, tokens);
    bump(q, m->id, "mo", mo, tokens);
    /* старые минутные окна больше не нужны */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "DELETE FROM usage WHERE model_id=? AND period='m' AND bucket<>?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, m->id, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, mi, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_exec(q->db, "COMMIT", NULL, NULL, NULL);
    pthread_mutex_unlock(&q->mu);
}

void quota_set_cooldown(quota_db *q, const char *model_id, int64_t until, const char *reason) {
    pthread_mutex_lock(&q->mu);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db,
                           "INSERT INTO cooldown(model_id, until_ts, reason) VALUES(?,?,?) "
                           "ON CONFLICT(model_id) DO UPDATE SET until_ts=MAX(until_ts, excluded.until_ts), reason=excluded.reason",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, model_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, until);
        sqlite3_bind_text(st, 3, reason ? reason : "", -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&q->mu);
}

void quota_reset(quota_db *q, const char *model_id) {
    pthread_mutex_lock(&q->mu);
    const char *sql[] = { "DELETE FROM cooldown WHERE model_id=?", "DELETE FROM usage WHERE model_id=?" };
    for (int i = 0; i < 2; i++) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(q->db, sql[i], -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, model_id, -1, SQLITE_STATIC);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    pthread_mutex_unlock(&q->mu);
}

void quota_get_status(quota_db *q, const model_cfg *m, int64_t now, quota_status *s) {
    char mi[32], d[32], mo[32];
    buckets(now, mi, d, mo);
    memset(s, 0, sizeof *s);
    pthread_mutex_lock(&q->mu);
    get_usage(q, m->id, "m", mi, &s->req_minute, NULL);
    get_usage(q, m->id, "d", d, &s->req_day, &s->tokens_day);
    get_usage(q, m->id, "mo", mo, &s->req_month, NULL);
    int64_t until = get_cooldown(q, m->id, s->cooldown_reason, sizeof s->cooldown_reason);
    s->cooldown_until = until > now ? until : 0;
    if (!s->cooldown_until) s->cooldown_reason[0] = '\0';
    pthread_mutex_unlock(&q->mu);
}

void quota_log_attempt(quota_db *q, int64_t now, const char *request_id, const char *model_id, const char *kind,
                       long http_status, const char *message) {
    pthread_mutex_lock(&q->mu);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "INSERT INTO attempts(ts, request_id, model_id, kind, http_status, message) VALUES(?,?,?,?,?,?)",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, now);
        sqlite3_bind_text(st, 2, request_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, model_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, kind, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 5, http_status);
        sqlite3_bind_text(st, 6, message ? message : "", -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    /* журнал не растёт бесконечно: храним 30 дней */
    if (sqlite3_prepare_v2(q->db, "DELETE FROM attempts WHERE ts < ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, now - 30 * 86400);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&q->mu);
}

void quota_clear_cooldown(quota_db *q, const char *model_id) {
    pthread_mutex_lock(&q->mu);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "DELETE FROM cooldown WHERE model_id=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, model_id, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&q->mu);
}

/* Запись без блокировки (вызывающий держит q->mu). value — уже в нужном виде (открытый или enc:v1:). */
static int key_write_locked(quota_db *q, const char *provider, const char *value) {
    sqlite3_stmt *st;
    int rc = -1;
    const char *sql = (value && *value)
        ? "INSERT INTO api_keys(provider, api_key, updated_ts) VALUES(?,?,?) "
          "ON CONFLICT(provider) DO UPDATE SET api_key=excluded.api_key, updated_ts=excluded.updated_ts"
        : "DELETE FROM api_keys WHERE provider=?";
    if (sqlite3_prepare_v2(q->db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, provider, -1, SQLITE_STATIC);
        if (value && *value) {
            sqlite3_bind_text(st, 2, value, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 3, plat_now());
        }
        rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(st);
    }
    if (rc) LOGE("keys", "SQLite: %s", sqlite3_errmsg(q->db));
    return rc;
}

int store_key_set(quota_db *q, const char *provider, const char *key) {
    char enc[4096];
    const char *value = key;
    if (key && *key && q->encrypt) {
        if (crypto_encrypt(key, enc, sizeof enc) != 0) { LOGE("keys", "не удалось зашифровать ключ «%s»", provider); return -1; }
        value = enc;
    }
    pthread_mutex_lock(&q->mu);
    int rc = key_write_locked(q, provider, value);
    pthread_mutex_unlock(&q->mu);
    return rc;
}

int store_key_get_raw(quota_db *q, const char *provider, char *buf, size_t len) {
    int found = 0;
    pthread_mutex_lock(&q->mu);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "SELECT api_key FROM api_keys WHERE provider=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, provider, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *k = (const char *)sqlite3_column_text(st, 0);
            if (k && *k) { xstrlcpy(buf, k, len); found = 1; }
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&q->mu);
    return found;
}

int store_key_get(quota_db *q, const char *provider, char *buf, size_t len) {
    char raw[4096];
    if (!store_key_get_raw(q, provider, raw, sizeof raw)) return 0;
    if (crypto_is_encrypted(raw)) {
        if (crypto_decrypt(raw, buf, len) != 0) {
            LOGE("keys", "ключ «%s» не расшифровывается (сменился мастер-ключ?) — введите его заново в «Настройках»", provider);
            buf[0] = '\0';
            return 0;
        }
        return 1;
    }
    xstrlcpy(buf, raw, len);
    return 1;
}

int quota_set_encryption(quota_db *q, int on, int migrate) {
    if (on && !crypto_available()) return -1;
    q->encrypt = on;
    if (!migrate) return 0;
    char names[256][64];
    int n = store_key_list(q, names, 256), done = 0;
    for (int i = 0; i < n; i++) {
        char plain[2100];
        if (!store_key_get(q, names[i], plain, sizeof plain)) continue;
        if (store_key_set(q, names[i], plain) == 0) done++; /* перезапись в новом виде */
        OPENSSL_cleanse(plain, sizeof plain);
    }
    return done;
}

char *quota_kv_get(quota_db *q, const char *name) {
    char *out = NULL;
    pthread_mutex_lock(&q->mu);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "SELECT value FROM kv WHERE name=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) out = xstrdup((const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&q->mu);
    return out;
}

void quota_kv_set(quota_db *q, const char *name, const char *value) {
    pthread_mutex_lock(&q->mu);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "INSERT INTO kv(name, value) VALUES(?,?) ON CONFLICT(name) DO UPDATE SET value=excluded.value",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, value ? value : "", -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&q->mu);
}

int store_key_list(quota_db *q, char names[][64], int max) {
    int n = 0;
    pthread_mutex_lock(&q->mu);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "SELECT provider FROM api_keys ORDER BY provider", -1, &st, NULL) == SQLITE_OK) {
        while (n < max && sqlite3_step(st) == SQLITE_ROW)
            xstrlcpy(names[n++], (const char *)sqlite3_column_text(st, 0), 64);
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&q->mu);
    return n;
}
