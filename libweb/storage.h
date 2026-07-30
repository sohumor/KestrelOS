#pragma once

/* Bounded Web Storage backing store.
 *
 * The store is deliberately a flat, packed entry array.  Web Storage lookups
 * are small enough that a procedural linear scan is predictable, cache
 * friendly, and substantially simpler than exposing ownership through a
 * hierarchy of per-origin objects.  A browser runtime composes two instances:
 * one persistent local store and one process-lifetime session store.
 */

#define WEB_STORAGE_OK          0
#define WEB_STORAGE_QUOTA      -1
#define WEB_STORAGE_FULL       -2
#define WEB_STORAGE_NOMEM      -3
#define WEB_STORAGE_BAD_DATA   -4

struct web_storage;

/* quota is enforced independently for each origin. max_entries bounds the
 * complete store across all origins. */
struct web_storage *web_storage_new(unsigned long quota,
                                    unsigned int max_entries);
void web_storage_free(struct web_storage *s);

const char *web_storage_get(const struct web_storage *s, const char *origin,
                            const char *key);
int web_storage_set(struct web_storage *s, const char *origin,
                    const char *key, const char *value);
int web_storage_remove(struct web_storage *s, const char *origin,
                       const char *key);
unsigned int web_storage_clear(struct web_storage *s, const char *origin);

unsigned int web_storage_length(const struct web_storage *s,
                                const char *origin);
const char *web_storage_key(const struct web_storage *s, const char *origin,
                            unsigned int index);
unsigned long web_storage_usage(const struct web_storage *s,
                                const char *origin);

/* Persistence format is a versioned, percent-escaped line stream.  Exported
 * memory belongs to the caller. Import is transactional: malformed or
 * over-limit input leaves the destination unchanged. */
char *web_storage_export(const struct web_storage *s, unsigned long *out_len);
int web_storage_import(struct web_storage *s, const char *data,
                       unsigned long len);

int web_storage_dirty(const struct web_storage *s);
void web_storage_clear_dirty(struct web_storage *s);
const char *web_storage_error(int rc);
