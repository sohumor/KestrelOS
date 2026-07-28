/* Host regression tests for the JavaScript <-> DOM compatibility layer. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../libweb/dom.h"
#include "../libweb/jsdom.h"

static int checks;
static int failures;
static char console_text[512];
static int fetch_calls;

#define CHECK(expr, name) do {                                               \
    checks++;                                                                 \
    if (!(expr)) {                                                            \
        failures++;                                                           \
        printf("FAIL %s\n", name);                                            \
    }                                                                         \
} while (0)

static void print_sink(void *user, const char *text)
{
    unsigned long n = strlen(console_text);
    (void)user;
    if (n && n + 1 < sizeof(console_text))
        console_text[n++] = '\n';
    snprintf(console_text + n, sizeof(console_text) - n, "%s", text);
}

static char *copy_text(const char *s)
{
    unsigned long n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int fetch_sink(void *user, const char *url, const char *method,
                      const void *body, unsigned long body_len,
                      struct jsdom_fetch_response *out,
                      char *err, unsigned long errsz)
{
    static const char payload[] =
        "{\"message\":\"FETCH-OK\",\"count\":2}";
    static const unsigned char wasm[] = {
        0,97,115,109,1,0,0,0,
        1,7,1,96,2,127,127,1,127,
        3,2,1,0,7,7,1,3,97,100,100,0,0,
        10,9,1,7,0,32,0,32,1,106,11
    };
    (void)user; (void)err; (void)errsz;

    fetch_calls++;
    if (!strcmp(url, "https://example.test/module.wasm") &&
        !strcmp(method, "GET") && !body_len) {
        memset(out, 0, sizeof(*out));
        out->status = 200;
        out->status_text = copy_text("OK");
        out->url = copy_text(url);
        out->content_type = copy_text("application/wasm");
        out->body = (char *)malloc(sizeof(wasm));
        if (out->body)
            memcpy(out->body, wasm, sizeof(wasm));
        out->body_len = sizeof(wasm);
        return out->status_text && out->url && out->content_type && out->body
            ? 0 : -1;
    }
    if (strcmp(url, "https://example.test/api/data") ||
        strcmp(method, "POST") || body_len != 3 ||
        memcmp(body, "q=1", 3))
        return -1;
    memset(out, 0, sizeof(*out));
    out->status = 201;
    out->status_text = copy_text("Created");
    out->url = copy_text(url);
    out->content_type = copy_text("application/json");
    out->body = copy_text(payload);
    out->body_len = sizeof(payload) - 1;
    return out->status_text && out->url && out->content_type && out->body
        ? 0 : -1;
}

int main(void)
{
    static const char html[] =
        "<!doctype html><html><head><title>Before</title></head><body>"
        "<main id=app><p class='note old'>hello</p>"
        "<button id=go onclick=\"this.classList.add('inline')\">Go</button>"
        "</main></body></html>";
    static const char script[] =
        "var p = document.querySelector('main .note');"
        "if (!p || p.tagName !== 'P') throw new Error('query failed');"
        "p.textContent = 'changed';"
        "p.classList.remove('old');"
        "p.classList.add('ready');"
        "p.style.backgroundColor = '#123456';"
        "p.setAttribute('data-state', 'ok');"
        "var em = document.createElement('em');"
        "em.textContent = ' child';"
        "p.appendChild(em);"
        "document.title = 'After';"
        "var b = document.getElementById('go');"
        "b.addEventListener('click', function(e) {"
        "  p.textContent = p.textContent + '!';"
        "  console.log('clicked', p.textContent);"
        "});"
        "location.href = '/next?q=1';";
    struct dom_document *doc;
    struct jsdom_config cfg;
    struct jsdom *j;
    struct dom_node *p, *button;
    char err[256];
    char *text;
    unsigned long text_len;
    int allow;

    doc = html_parse_document(html, sizeof(html) - 1);
    CHECK(doc != 0 && !doc->oom, "parse fixture");
    if (!doc || doc->oom)
        return 1;

    memset(&cfg, 0, sizeof(cfg));
    cfg.url = "https://example.test/a/index.html";
    cfg.print = print_sink;
    cfg.fetch = fetch_sink;
    j = jsdom_new(doc, &cfg);
    CHECK(j != 0, "create binding");
    if (!j) {
        dom_document_free(doc);
        return 1;
    }

    err[0] = 0;
    CHECK(jsdom_eval(j, script, "fixture.js", err, sizeof(err)) == 0,
          err[0] ? err : "evaluate script");
    p = dom_find_tag(doc->body, "p");
    button = dom_get_element_by_id(doc, "go");
    CHECK(p != 0 && button != 0, "fixture nodes remain");
    CHECK(p && dom_has_class(p, "ready") && !dom_has_class(p, "old"),
          "classList mutations");
    CHECK(p && !strcmp(dom_get_attr(p, "data-state"), "ok"),
          "attribute mutation");
    CHECK(p && strstr(dom_get_attr(p, "style"), "background-color:#123456"),
          "style property mutation");
    CHECK(!strcmp(doc->title, "After"), "document title mutation");
    CHECK(jsdom_dirty(j), "dirty flag");
    CHECK(!strcmp(jsdom_pending_navigation(j),
                  "https://example.test/next?q=1"),
          "location resolution");

    jsdom_clear_dirty(j);
    err[0] = 0;
    allow = jsdom_dispatch(j, button, "click", err, sizeof(err));
    CHECK(allow == 1, "click default allowed");
    CHECK(dom_has_class(button, "inline"), "inline click handler");
    text = dom_text_content(p, &text_len);
    CHECK(text && !strcmp(text, "changed child!"), "listener changed DOM");
    free(text);
    CHECK(strstr(console_text, "clicked changed child!") != 0,
          "console sink");
    CHECK(jsdom_dirty(j), "event mutation dirty");

    CHECK(jsdom_eval(j,
          "document.getElementById('go').addEventListener('submit',"
          "function(e){e.preventDefault();});",
          "cancel.js", err, sizeof(err)) == 0, "install cancelling listener");
    CHECK(jsdom_dispatch(j, button, "submit", err, sizeof(err)) == 0,
          "preventDefault cancels");

    CHECK(jsdom_eval(j,
          "document.addEventListener('DOMContentLoaded', function(){"
          " document.body.setAttribute('data-ready','yes'); });"
          "setTimeout(function(){"
          " document.getElementById('go').innerHTML='<b>Timer</b>';"
          "}, 1);",
          "events.js", err, sizeof(err)) == 0, "register document event/timer");
    CHECK(jsdom_eval(j,
          "if (atob(btoa('Kestrel')) !== 'Kestrel') throw Error('base64');"
          "if (navigator.platform.indexOf('KestrelOS') < 0)"
          " throw Error('navigator');",
          "web-globals.js", err, sizeof(err)) == 0,
          "browser utility globals");
    CHECK(jsdom_dispatch_document(j, "DOMContentLoaded",
                                  err, sizeof(err)) == 1,
          "dispatch DOMContentLoaded");
    CHECK(!strcmp(dom_get_attr(doc->body, "data-ready"), "yes"),
          "document event listener ran");
    CHECK(jsdom_pump(j, err, sizeof(err)) == 1, "timer pump ran callback");
    CHECK(button->first_child && button->first_child->type == DOM_ELEMENT &&
          button->first_child->tag_id == HTAG_B,
          "innerHTML parsed a fragment");
    text = dom_text_content(button, &text_len);
    CHECK(text && !strcmp(text, "Timer"), "innerHTML content visible");
    free(text);

    CHECK(jsdom_eval(j,
          "let order='sync'; const expected=7;"
          "Promise.resolve(3).then(function(v){"
          " order='promise-'+v;"
          " return v+1;"
          "}).then(function(v){"
          " document.body.setAttribute('data-chain',order+'-'+v);"
          "});"
          "new Promise(function(resolve){resolve(expected);})"
          ".then(function(v){document.body.setAttribute('data-new',''+v);});"
          "Promise.reject('handled').catch(function(e){"
          " document.body.setAttribute('data-catch',e);"
          "});"
          "Promise.all([Promise.resolve(1),2,Promise.resolve(3)])"
          ".then(function(values){"
          " document.body.setAttribute('data-all',values.join('-'));"
          "});"
          "Promise.race([Promise.resolve('winner'),"
          " Promise.resolve('later')]).then(function(value){"
          " document.body.setAttribute('data-race',value);"
          "});"
          "Promise.resolve('kept').finally(function(){"
          " document.body.setAttribute('data-finally','ran');"
          " return Promise.resolve('cleanup');"
          "}).then(function(value){"
          " document.body.setAttribute('data-final-value',value);"
          "});"
          "Promise.reject('original').finally(function(){return 9;})"
          ".catch(function(reason){"
          " document.body.setAttribute('data-final-reject',reason);"
          "});"
          "var wasmBytes=[0,97,115,109,1,0,0,0,"
          "1,7,1,96,2,127,127,1,127,"
          "3,2,1,0,7,7,1,3,97,100,100,0,0,"
          "10,9,1,7,0,32,0,32,1,106,11];"
          "if(!WebAssembly.validate(wasmBytes) ||"
          " WebAssembly.validate([0,1,2])) throw Error('wasm validate');"
          "var wasmModule=new WebAssembly.Module(wasmBytes);"
          "var wasmInstance=new WebAssembly.Instance(wasmModule,{});"
          "if(typeof Module!=='undefined' || typeof Instance!=='undefined')"
          " throw Error('wasm constructor leaked');"
          "if(!(wasmModule instanceof WebAssembly.Module) ||"
          " !(wasmInstance instanceof WebAssembly.Instance))"
          " throw Error('wasm prototype');"
          "if(wasmInstance.exports.add(20,22)!==42)"
          " throw Error('wasm execute');"
          "WebAssembly.instantiate(wasmModule).then(function(instance){"
          " document.body.setAttribute('data-wasm',"
          "   ''+instance.exports.add(7,8));"
          "});"
          "var wasmResponse;"
          "fetch('/module.wasm').then(function(response){"
          " wasmResponse=response;"
          " return response.arrayBuffer();"
          "}).then(function(buffer){"
          " document.body.setAttribute('data-buffer',''+buffer.byteLength);"
          " return WebAssembly.instantiate(buffer);"
          "}).then(function(pair){"
          " document.body.setAttribute('data-wasm-fetch',"
          "   ''+pair.instance.exports.add(9,10));"
          " if(!wasmResponse.bodyUsed) throw Error('bodyUsed');"
          "});"
          "fetch('/api/data',{method:'POST',body:'q=1'})"
          ".then(function(response){"
          " if (!response.ok || response.status!==201) throw Error('status');"
          " if (response.headers.get('content-type')!=='application/json')"
          "   throw Error('headers');"
          " return response.json();"
          "}).then(function(data){"
          " document.body.setAttribute('data-fetch',"
          "   data.message+'-'+data.count);"
          "});"
          "if(order!=='sync') throw Error('promise ran synchronously');",
          "async.js", err, sizeof(err)) == 0,
          err[0] ? err : "register promises and fetch");
    CHECK(!dom_get_attr(doc->body, "data-chain"),
          "promise callbacks wait for checkpoint");
    CHECK(jsdom_pump(j, err, sizeof(err)) > 0,
          err[0] ? err : "promise/fetch checkpoint");
    CHECK(dom_get_attr(doc->body, "data-chain") &&
          !strcmp(dom_get_attr(doc->body, "data-chain"), "promise-3-4"),
          "Promise chain and microtask order");
    CHECK(dom_get_attr(doc->body, "data-new") &&
          !strcmp(dom_get_attr(doc->body, "data-new"), "7"),
          "Promise constructor resolver");
    CHECK(dom_get_attr(doc->body, "data-catch") &&
          !strcmp(dom_get_attr(doc->body, "data-catch"), "handled"),
          "Promise rejection catch");
    CHECK(dom_get_attr(doc->body, "data-all") &&
          !strcmp(dom_get_attr(doc->body, "data-all"), "1-2-3"),
          "Promise.all preserves input order");
    CHECK(dom_get_attr(doc->body, "data-race") &&
          !strcmp(dom_get_attr(doc->body, "data-race"), "winner"),
          "Promise.race settles from the first reaction");
    CHECK(dom_get_attr(doc->body, "data-finally") &&
          !strcmp(dom_get_attr(doc->body, "data-finally"), "ran") &&
          dom_get_attr(doc->body, "data-final-value") &&
          !strcmp(dom_get_attr(doc->body, "data-final-value"), "kept"),
          "Promise.finally waits and preserves fulfillment");
    CHECK(dom_get_attr(doc->body, "data-final-reject") &&
          !strcmp(dom_get_attr(doc->body, "data-final-reject"), "original"),
          "Promise.finally preserves rejection");
    CHECK(dom_get_attr(doc->body, "data-wasm") &&
          !strcmp(dom_get_attr(doc->body, "data-wasm"), "15"),
          "WebAssembly validates, instantiates, and executes i32 MVP code");
    CHECK(dom_get_attr(doc->body, "data-buffer") &&
          !strcmp(dom_get_attr(doc->body, "data-buffer"), "41") &&
          dom_get_attr(doc->body, "data-wasm-fetch") &&
          !strcmp(dom_get_attr(doc->body, "data-wasm-fetch"), "19"),
          "fetch arrayBuffer feeds WebAssembly.instantiate");
    CHECK(fetch_calls == 2, "fetch host called twice");
    CHECK(dom_get_attr(doc->body, "data-fetch") &&
          !strcmp(dom_get_attr(doc->body, "data-fetch"), "FETCH-OK-2"),
          "fetch Response.json Promise chain");

    jsdom_free(j);
    dom_document_free(doc);
    printf("%d passed, %d failed\n", checks - failures, failures);
    return failures ? 1 : 0;
}
