#pragma once

/* Small browser DOM binding for libjs.
 *
 * This deliberately exposes the useful, interoperable core of the DOM rather
 * than inventing Kestrel-specific page APIs.  Mutations are applied directly
 * to libweb's DOM and are reported through jsdom_dirty(), allowing the browser
 * to recompute style and layout after a script or event handler completes.
 */

#include "dom.h"
#include "../libjs/js.h"

struct jsdom;

struct jsdom_fetch_response {
    int status;
    char *status_text;
    char *url;
    char *content_type;
    char *body;
    unsigned long body_len;
};

struct jsdom_config {
    const char *url;       /* document.URL */
    const char *base_url;  /* URL resolution base; NULL means url */
    void (*print)(void *user, const char *text);
    const char *(*cookie_get)(void *user);
    int (*cookie_set)(void *user, const char *value);
    /* Runs at an event-loop checkpoint, never recursively inside fetch().
     * On success ownership of every allocated response field passes to the
     * DOM binding. */
    int (*fetch)(void *user, const char *url, const char *method,
                 const void *body, unsigned long body_len,
                 struct jsdom_fetch_response *out,
                 char *err, unsigned long errsz);
    void *user;
    unsigned long max_heap;
    unsigned long max_steps;
};

struct jsdom *jsdom_new(struct dom_document *doc,
                        const struct jsdom_config *cfg);
void jsdom_free(struct jsdom *j);

/* Run one classic script.  A thrown exception is copied into err. */
int jsdom_eval(struct jsdom *j, const char *source, const char *name,
               char *err, unsigned long errsz);

/* Dispatch an Event-compatible handler.  Returns 1 when the default action
 * is allowed, 0 when preventDefault() or `return false` cancelled it. */
int jsdom_dispatch(struct jsdom *j, struct dom_node *target,
                   const char *type, char *err, unsigned long errsz);
int jsdom_dispatch_document(struct jsdom *j, const char *type,
                            char *err, unsigned long errsz);
int jsdom_pump(struct jsdom *j, char *err, unsigned long errsz);

int jsdom_dirty(const struct jsdom *j);
void jsdom_clear_dirty(struct jsdom *j);

/* A location assignment is queued rather than recursively navigating from
 * inside the interpreter.  The browser consumes it after the script returns. */
const char *jsdom_pending_navigation(const struct jsdom *j);
void jsdom_clear_navigation(struct jsdom *j);
