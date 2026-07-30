/* storage.c - bounded, origin-partitioned Web Storage data.
 *
 * All hot operations are procedural scans over one contiguous entry array.
 * Entries own their strings; the store owns the array.  This keeps ownership
 * explicit and makes quota accounting a simple sum rather than a graph walk.
 */

#include "storage.h"
#include "url.h"

#include <stdlib.h>
#include <string.h>

#define STORAGE_MAGIC "KWS1\n"
#define STORAGE_FIELD_MAX (1024UL * 1024UL)

struct storage_entry {
    char *origin;
    char *key;
    char *value;
};

struct web_storage {
    struct storage_entry *entries;
    unsigned int count;
    unsigned int capacity;
    unsigned int max_entries;
    unsigned long quota;
    int dirty;
};

static char *copy_n(const char *p, unsigned long n)
{
    char *out;

    if (n > STORAGE_FIELD_MAX)
        return 0;
    out = (char *)malloc(n + 1);
    if (!out)
        return 0;
    if (n)
        memcpy(out, p, n);
    out[n] = 0;
    return out;
}

static char *copy_z(const char *p)
{
    return p ? copy_n(p, strlen(p)) : 0;
}

static int find_entry(const struct web_storage *s, const char *origin,
                      const char *key)
{
    unsigned int i;

    if (!s || !origin || !key)
        return -1;
    for (i = 0; i < s->count; i++)
        if (!strcmp(s->entries[i].origin, origin) &&
            !strcmp(s->entries[i].key, key))
            return (int)i;
    return -1;
}

static void entry_free(struct storage_entry *e)
{
    if (!e)
        return;
    free(e->origin);
    free(e->key);
    free(e->value);
    memset(e, 0, sizeof(*e));
}

struct web_storage *web_storage_new(unsigned long quota,
                                    unsigned int max_entries)
{
    struct web_storage *s;

    if (!quota || !max_entries)
        return 0;
    s = (struct web_storage *)calloc(1, sizeof(*s));
    if (!s)
        return 0;
    s->quota = quota;
    s->max_entries = max_entries;
    return s;
}

void web_storage_free(struct web_storage *s)
{
    unsigned int i;

    if (!s)
        return;
    for (i = 0; i < s->count; i++)
        entry_free(&s->entries[i]);
    free(s->entries);
    free(s);
}

const char *web_storage_get(const struct web_storage *s, const char *origin,
                            const char *key)
{
    int at = find_entry(s, origin, key);

    return at >= 0 ? s->entries[at].value : 0;
}

unsigned long web_storage_usage(const struct web_storage *s,
                                const char *origin)
{
    unsigned long used = 0;
    unsigned int i;

    if (!s || !origin)
        return 0;
    for (i = 0; i < s->count; i++)
        if (!strcmp(s->entries[i].origin, origin))
            used += strlen(s->entries[i].key) +
                    strlen(s->entries[i].value);
    return used;
}

static int reserve_one(struct web_storage *s)
{
    struct storage_entry *grown;
    unsigned int cap;

    if (s->count < s->capacity)
        return WEB_STORAGE_OK;
    if (s->count >= s->max_entries)
        return WEB_STORAGE_FULL;
    cap = s->capacity ? s->capacity * 2 : 16;
    if (cap > s->max_entries)
        cap = s->max_entries;
    grown = (struct storage_entry *)realloc(
        s->entries, (unsigned long)cap * sizeof(*grown));
    if (!grown)
        return WEB_STORAGE_NOMEM;
    memset(grown + s->capacity, 0,
           (unsigned long)(cap - s->capacity) * sizeof(*grown));
    s->entries = grown;
    s->capacity = cap;
    return WEB_STORAGE_OK;
}

int web_storage_set(struct web_storage *s, const char *origin,
                    const char *key, const char *value)
{
    unsigned long old_size = 0, new_size, used;
    char *new_value;
    int at, rc;

    if (!s || !origin || !*origin || !key || !value)
        return WEB_STORAGE_BAD_DATA;
    if (strlen(origin) > STORAGE_FIELD_MAX ||
        strlen(key) > STORAGE_FIELD_MAX ||
        strlen(value) > STORAGE_FIELD_MAX)
        return WEB_STORAGE_QUOTA;
    at = find_entry(s, origin, key);
    if (at >= 0)
        old_size = strlen(s->entries[at].key) +
                   strlen(s->entries[at].value);
    new_size = strlen(key) + strlen(value);
    used = web_storage_usage(s, origin);
    if (new_size > s->quota || used - old_size > s->quota - new_size)
        return WEB_STORAGE_QUOTA;

    new_value = copy_z(value);
    if (!new_value)
        return WEB_STORAGE_NOMEM;
    if (at >= 0) {
        if (!strcmp(s->entries[at].value, value)) {
            free(new_value);
            return WEB_STORAGE_OK;
        }
        free(s->entries[at].value);
        s->entries[at].value = new_value;
        s->dirty = 1;
        return WEB_STORAGE_OK;
    }

    rc = reserve_one(s);
    if (rc != WEB_STORAGE_OK) {
        free(new_value);
        return rc;
    }
    s->entries[s->count].origin = copy_z(origin);
    s->entries[s->count].key = copy_z(key);
    s->entries[s->count].value = new_value;
    if (!s->entries[s->count].origin || !s->entries[s->count].key) {
        entry_free(&s->entries[s->count]);
        return WEB_STORAGE_NOMEM;
    }
    s->count++;
    s->dirty = 1;
    return WEB_STORAGE_OK;
}

int web_storage_remove(struct web_storage *s, const char *origin,
                       const char *key)
{
    int at;

    if (!s || !origin || !key)
        return 0;
    at = find_entry(s, origin, key);
    if (at < 0)
        return 0;
    entry_free(&s->entries[at]);
    s->count--;
    if ((unsigned int)at != s->count)
        s->entries[at] = s->entries[s->count];
    memset(&s->entries[s->count], 0, sizeof(s->entries[s->count]));
    s->dirty = 1;
    return 1;
}

unsigned int web_storage_clear(struct web_storage *s, const char *origin)
{
    unsigned int i = 0, removed = 0;

    if (!s || !origin)
        return 0;
    while (i < s->count) {
        if (strcmp(s->entries[i].origin, origin)) {
            i++;
            continue;
        }
        entry_free(&s->entries[i]);
        s->count--;
        if (i != s->count)
            s->entries[i] = s->entries[s->count];
        memset(&s->entries[s->count], 0, sizeof(s->entries[s->count]));
        removed++;
    }
    if (removed)
        s->dirty = 1;
    return removed;
}

unsigned int web_storage_length(const struct web_storage *s,
                                const char *origin)
{
    unsigned int i, n = 0;

    if (!s || !origin)
        return 0;
    for (i = 0; i < s->count; i++)
        if (!strcmp(s->entries[i].origin, origin))
            n++;
    return n;
}

const char *web_storage_key(const struct web_storage *s, const char *origin,
                            unsigned int index)
{
    unsigned int i, n = 0;

    if (!s || !origin)
        return 0;
    for (i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].origin, origin))
            continue;
        if (n++ == index)
            return s->entries[i].key;
    }
    return 0;
}

static int encoded_size(const char *s, unsigned long *total)
{
    unsigned long n = strlen(s);

    if (n > (~0UL - *total) / 3)
        return 0;
    *total += n * 3;
    return 1;
}

char *web_storage_export(const struct web_storage *s, unsigned long *out_len)
{
    unsigned long cap = sizeof(STORAGE_MAGIC), used;
    unsigned int i;
    char *out;

    if (out_len)
        *out_len = 0;
    if (!s)
        return 0;
    for (i = 0; i < s->count; i++) {
        if (!encoded_size(s->entries[i].origin, &cap) ||
            !encoded_size(s->entries[i].key, &cap) ||
            !encoded_size(s->entries[i].value, &cap) ||
            cap > ~0UL - 4)
            return 0;
        cap += 4;
    }
    out = (char *)malloc(cap);
    if (!out)
        return 0;
    memcpy(out, STORAGE_MAGIC, sizeof(STORAGE_MAGIC) - 1);
    used = sizeof(STORAGE_MAGIC) - 1;
    for (i = 0; i < s->count; i++) {
        const char *fields[3] = {
            s->entries[i].origin, s->entries[i].key, s->entries[i].value
        };
        int f;

        for (f = 0; f < 3; f++) {
            long n = url_pct_encode(fields[f], ~0UL, URL_COMP_FORM,
                                    out + used, cap - used);
            if (n < 0) {
                free(out);
                return 0;
            }
            used += (unsigned long)n;
            out[used++] = f == 2 ? '\n' : '\t';
        }
    }
    out[used] = 0;
    if (out_len)
        *out_len = used;
    return out;
}

static int decode_field(const char *p, unsigned long n, char **out)
{
    char *decoded;
    long used;

    if (n > STORAGE_FIELD_MAX * 3)
        return 0;
    decoded = (char *)malloc(n + 1);
    if (!decoded)
        return -1;
    used = url_pct_decode(p, n, 1, decoded, n + 1);
    if (used < 0 || (unsigned long)used > STORAGE_FIELD_MAX) {
        free(decoded);
        return 0;
    }
    *out = decoded;
    return 1;
}

int web_storage_import(struct web_storage *s, const char *data,
                       unsigned long len)
{
    struct web_storage *tmp;
    unsigned long at = sizeof(STORAGE_MAGIC) - 1;
    int rc = WEB_STORAGE_OK;

    if (!s || !data || len < at ||
        memcmp(data, STORAGE_MAGIC, at))
        return WEB_STORAGE_BAD_DATA;
    tmp = web_storage_new(s->quota, s->max_entries);
    if (!tmp)
        return WEB_STORAGE_NOMEM;
    while (at < len) {
        unsigned long start[3], size[3];
        char *field[3] = { 0, 0, 0 };
        int f;

        for (f = 0; f < 3; f++) {
            unsigned long end;
            char separator = f == 2 ? '\n' : '\t';

            start[f] = at;
            for (end = at; end < len && data[end] != separator; end++)
                ;
            if (end >= len) {
                rc = WEB_STORAGE_BAD_DATA;
                break;
            }
            size[f] = end - at;
            at = end + 1;
        }
        if (rc != WEB_STORAGE_OK)
            break;
        for (f = 0; f < 3; f++) {
            int decoded = decode_field(data + start[f], size[f], &field[f]);
            if (decoded <= 0) {
                rc = decoded < 0 ? WEB_STORAGE_NOMEM : WEB_STORAGE_BAD_DATA;
                break;
            }
        }
        if (rc == WEB_STORAGE_OK)
            rc = web_storage_set(tmp, field[0], field[1], field[2]);
        for (f = 0; f < 3; f++)
            free(field[f]);
        if (rc != WEB_STORAGE_OK)
            break;
    }
    if (rc == WEB_STORAGE_OK) {
        unsigned int i;

        for (i = 0; i < s->count; i++)
            entry_free(&s->entries[i]);
        free(s->entries);
        s->entries = tmp->entries;
        s->count = tmp->count;
        s->capacity = tmp->capacity;
        s->dirty = 0;
        tmp->entries = 0;
        tmp->count = tmp->capacity = 0;
    }
    web_storage_free(tmp);
    return rc;
}

int web_storage_dirty(const struct web_storage *s)
{
    return s ? s->dirty : 0;
}

void web_storage_clear_dirty(struct web_storage *s)
{
    if (s)
        s->dirty = 0;
}

const char *web_storage_error(int rc)
{
    switch (rc) {
    case WEB_STORAGE_OK:       return "ok";
    case WEB_STORAGE_QUOTA:    return "origin quota exceeded";
    case WEB_STORAGE_FULL:     return "entry limit reached";
    case WEB_STORAGE_NOMEM:    return "out of memory";
    case WEB_STORAGE_BAD_DATA: return "invalid storage data";
    default:                   return "storage error";
    }
}
