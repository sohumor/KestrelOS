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
static int beacon_calls;

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
                      const char *content_type, const char *accept,
                      const char *extra_headers,
                      const char *mode, const char *credentials,
                      const char *redirect,
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

    if (!strcmp(url, "https://example.test/beacon") &&
        !strcmp(method, "POST") && body_len == 6 &&
        !memcmp(body, "BEACON", 6) && content_type &&
        !strcmp(content_type, "text/plain;charset=UTF-8") &&
        !strcmp(mode, "no-cors") &&
        !strcmp(credentials, "include") &&
        !strcmp(redirect, "follow")) {
        beacon_calls++;
        memset(out, 0, sizeof(*out));
        out->status = 204;
        out->status_text = copy_text("No Content");
        out->url = copy_text(url);
        out->content_type = copy_text("text/plain");
        out->body = copy_text("");
        return out->status_text && out->url && out->content_type && out->body
            ? 0 : -1;
    }
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
        memcmp(body, "q=1", 3) ||
        !content_type ||
        strcmp(content_type, "text/plain;charset=UTF-8") ||
        !accept || strcmp(accept, "application/json") ||
        !extra_headers || strcmp(extra_headers,
                                 "x-kestrel: runtime\r\n") ||
        strcmp(mode, "cors") ||
        strcmp(credentials, "same-origin") ||
        strcmp(redirect, "follow"))
        return -1;
    memset(out, 0, sizeof(*out));
    out->status = 201;
    out->status_text = copy_text("Created");
    out->url = copy_text(url);
    out->content_type = copy_text("application/json");
    out->body = copy_text(payload);
    out->body_len = sizeof(payload) - 1;
    out->redirected = 1;
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
        "  if(e instanceof MouseEvent && e.isTrusted)"
        "    b.setAttribute('data-native-event','mouse');"
        "  p.textContent = p.textContent + '!';"
        "  console.log('clicked', p.textContent);"
        "});"
        "location.href = '/next?q=1';";
    struct dom_document *doc;
    struct jsdom_config cfg;
    struct jsdom *j;
    struct web_storage *local_store, *session_store;
    struct web_storage *roundtrip_store;
    char *storage_blob;
    unsigned long storage_blob_len;
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
    local_store = web_storage_new(4096, 64);
    session_store = web_storage_new(4096, 64);
    CHECK(local_store != 0 && session_store != 0, "create storage services");
    cfg.url = "https://example.test/a/index.html";
    cfg.print = print_sink;
    cfg.fetch = fetch_sink;
    cfg.local_storage = local_store;
    cfg.session_storage = session_store;
    cfg.hardware_concurrency = 4;
    cfg.viewport_width = 800;
    cfg.viewport_height = 600;
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
    jsdom_clear_navigation(j);
    CHECK(jsdom_eval(j,
          "if(location.href!=='https://example.test/a/index.html' ||"
          " location.origin!=='https://example.test' ||"
          " location.protocol!=='https:' ||"
          " location.host!=='example.test' ||"
          " location.hostname!=='example.test' || location.port!=='' ||"
          " location.pathname!=='/a/index.html' ||"
          " location.search!=='' || location.hash!=='' ||"
          " location.toString()!==location.href ||"
          " document.URL!==location.href ||"
          " document.documentURI!==location.href ||"
          " document.baseURI!==location.href || document.referrer!=='' ||"
          " document.characterSet!=='UTF-8' ||"
          " document.charset!=='UTF-8' ||"
          " document.inputEncoding!=='UTF-8' ||"
          " document.contentType!=='text/html' ||"
          " document.compatMode!=='CSS1Compat' ||"
          " document.hidden || document.visibilityState!=='visible' ||"
          " document.defaultView!==window)"
          " throw Error('document URL surface');",
          "location-shape.js", err, sizeof(err)) == 0,
          err[0] ? err : "Location and document URL component shape");
    CHECK(jsdom_eval(j, "location.search='?q=hello world';",
                     "location-search.js", err, sizeof(err)) == 0 &&
          jsdom_pending_navigation(j) &&
          !strcmp(jsdom_pending_navigation(j),
                  "https://example.test/a/index.html?q=hello%20world"),
          "Location search setter queues normalized navigation");
    jsdom_clear_navigation(j);
    CHECK(jsdom_eval(j, "location.pathname='/next/../final page';",
                     "location-path.js", err, sizeof(err)) == 0 &&
          jsdom_pending_navigation(j) &&
          !strcmp(jsdom_pending_navigation(j),
                  "https://example.test/final%20page"),
          "Location pathname setter encodes and normalizes");
    jsdom_clear_navigation(j);
    CHECK(jsdom_eval(j, "location.reload();",
                     "location-reload.js", err, sizeof(err)) == 0 &&
          jsdom_pending_navigation(j) &&
          !strcmp(jsdom_pending_navigation(j),
                  "https://example.test/a/index.html"),
          "Location reload queues the current document");
    jsdom_clear_navigation(j);
    CHECK(jsdom_eval(j,
          "if(history.length!==1 || history.state!==null ||"
          " history.scrollRestoration!=='auto')"
          " throw Error('history defaults');"
          "history.scrollRestoration='manual';"
          "var popCount=0;var popSteps='';"
          "addEventListener('popstate',function(event){"
          " popCount++;popSteps+=event.state.step;});"
          "history.pushState({step:1},'',"
          " '/route?x=1#one');"
          "history.replaceState({step:2},'', '#two');"
          "history.pushState({step:3},'', '#three');"
          "history.back();history.forward();"
          "var crossOriginBlocked=false;"
          "try{history.pushState({},'',"
          " 'https://other.test/');}catch(error){crossOriginBlocked=true;}"
          "if(history.length!==3 || history.state.step!==3 ||"
          " history.scrollRestoration!=='manual' ||"
          " location.pathname!=='/route' || location.search!=='?x=1' ||"
          " location.hash!=='#three' || popCount!==2 ||"
          " popSteps!=='23' || !crossOriginBlocked)"
          " throw Error('history traversal');",
          "history.js", err, sizeof(err)) == 0,
          err[0] ? err : "bounded same-document History traversal");
    CHECK(jsdom_document_url(j) &&
          !strcmp(jsdom_document_url(j),
                  "https://example.test/route?x=1#three"),
          "History updates the embedding document URL");
    CHECK(jsdom_eval(j, "history.go(0);",
                     "history-reload.js", err, sizeof(err)) == 0 &&
          jsdom_pending_navigation(j) &&
          !strcmp(jsdom_pending_navigation(j),
                  "https://example.test/route?x=1#three"),
          "history.go(0) queues a reload");
    jsdom_clear_navigation(j);

    jsdom_clear_dirty(j);
    err[0] = 0;
    allow = jsdom_dispatch(j, button, "click", err, sizeof(err));
    CHECK(allow == 1, "click default allowed");
    CHECK(dom_has_class(button, "inline"), "inline click handler");
    CHECK(dom_get_attr(button, "data-native-event") &&
          !strcmp(dom_get_attr(button, "data-native-event"), "mouse"),
          "host click dispatch uses trusted MouseEvent identity");
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
          "var eventTarget=new EventTarget();"
          "var eventCalls=0;"
          "var removedEvent=()=>{eventCalls+=100;};"
          "var customListener=e=>{"
          " if(!(e instanceof Event) || !(e instanceof CustomEvent) ||"
          "    e.target!==eventTarget || e.currentTarget!==eventTarget ||"
          "    e.eventPhase!==2 || e.detail.answer!==42)"
          "   throw Error('custom event shape');"
          " eventCalls++;e.preventDefault();"
          "};"
          "eventTarget.addEventListener('ready',removedEvent);"
          "eventTarget.removeEventListener('ready',removedEvent);"
          "eventTarget.addEventListener('ready',customListener);"
          "eventTarget.addEventListener('ready',customListener);"
          "var customEvent=new CustomEvent('ready',{"
          " detail:{answer:42},cancelable:true});"
          "if(eventTarget.dispatchEvent(customEvent)!==false ||"
          " !customEvent.defaultPrevented || customEvent.currentTarget!==null ||"
          " customEvent.eventPhase!==0 || eventCalls!==1)"
          " throw Error('custom dispatch');"
          "var plainEvent=new Event('plain');plainEvent.preventDefault();"
          "if(plainEvent.defaultPrevented ||"
          " eventTarget.dispatchEvent(plainEvent)!==true)"
          " throw Error('noncancelable');"
          "var bubbleParent=document.createElement('div');"
          "var bubbleChild=document.createElement('button');"
          "bubbleParent.appendChild(bubbleChild);"
          "document.body.appendChild(bubbleParent);"
          "if(!(bubbleChild instanceof EventTarget))"
          " throw Error('EventTarget prototype');"
          "var bubbleOrder='';"
          "bubbleChild.addEventListener('kestrel',e=>{"
          " bubbleOrder+='child';"
          " var nested=false;"
          " try{bubbleChild.dispatchEvent(e);}catch(error){nested=true;}"
          " if(!nested) throw Error('reentrant dispatch');"
          "});"
          "bubbleParent.addEventListener('kestrel',e=>{"
          " bubbleOrder+='-parent';"
          " if(e.eventPhase!==3 || e.target!==bubbleChild ||"
          "    e.currentTarget!==bubbleParent) throw Error('bubble state');"
          "});"
          "if(!bubbleChild.dispatchEvent(new Event('kestrel',{bubbles:true}))"
          " || bubbleOrder!=='child-parent') throw Error('bubbling');"
          "var stoppedParent=0;"
          "bubbleParent.addEventListener('stopped',()=>stoppedParent++);"
          "bubbleChild.addEventListener('stopped',e=>e.stopPropagation());"
          "var stoppedEvent=new Event('stopped',"
          " {bubbles:true,cancelable:true});"
          "if(!bubbleChild.dispatchEvent(stoppedEvent) ||"
          " stoppedEvent.defaultPrevented || stoppedParent!==0)"
          " throw Error('propagation versus cancellation');"
          "var immediate=0;"
          "eventTarget.addEventListener('immediate',e=>{"
          " immediate++;e.stopImmediatePropagation();"
          "});"
          "eventTarget.addEventListener('immediate',()=>immediate++);"
          "eventTarget.dispatchEvent(new Event('immediate'));"
          "if(immediate!==1) throw Error('immediate propagation');",
          "event-api.js", err, sizeof(err)) == 0,
          err[0] ? err : "EventTarget, CustomEvent, and propagation");

    CHECK(jsdom_eval(j,
          "var optionRoot=document.createElement('div');"
          "var optionChild=document.createElement('button');"
          "optionRoot.appendChild(optionChild);"
          "document.body.appendChild(optionRoot);"
          "var optionOrder='';"
          "var observedEventPath=null;"
          "addEventListener('listener-options',function(e){"
          " if(e.eventPhase!==Event.CAPTURING_PHASE)"
          "  throw Error('window capture phase');"
          " optionOrder+='wc>';"
          "},{capture:true});"
          "document.addEventListener('listener-options',function(e){"
          " if(e.eventPhase!==Event.CAPTURING_PHASE)"
          "  throw Error('document capture phase');"
          " optionOrder+='dc>';"
          "},true);"
          "optionRoot.addEventListener('listener-options',function(){"
          " optionOrder+='rc>';},{capture:true});"
          "optionChild.addEventListener('listener-options',function(e){"
          " if(e.eventPhase!==Event.AT_TARGET)"
          "  throw Error('target capture phase');"
          " observedEventPath=e.composedPath();"
          " if(e.srcElement!==optionChild)"
          "  throw Error('srcElement alias');"
          " optionOrder+='tc>';},{capture:true});"
          "optionChild.addEventListener('listener-options',function(){"
          " optionOrder+='tb>';});"
          "optionRoot.addEventListener('listener-options',function(){"
          " optionOrder+='rb>';});"
          "document.addEventListener('listener-options',function(){"
          " optionOrder+='db>';});"
          "addEventListener('listener-options',function(){"
          " optionOrder+='wb';});"
          "var phaseEvent=new Event('listener-options',{bubbles:true});"
          "optionChild.dispatchEvent(phaseEvent);"
          "if(optionOrder!=='wc>dc>rc>tc>tb>rb>db>wb' ||"
          " observedEventPath[0]!==optionChild ||"
          " observedEventPath[1]!==optionRoot ||"
          " !observedEventPath.includes(document) ||"
          " observedEventPath[observedEventPath.length-1]!==window ||"
          " phaseEvent.composedPath().length!==0)"
          " throw Error('capture bubble order '+optionOrder);"
          "var onceCalls=0;"
          "optionChild.addEventListener('listener-once',function(){"
          " onceCalls++;"
          " optionChild.dispatchEvent(new Event('listener-once'));"
          "},{once:true});"
          "optionChild.dispatchEvent(new Event('listener-once'));"
          "optionChild.dispatchEvent(new Event('listener-once'));"
          "if(onceCalls!==1) throw Error('once reentrancy');"
          "var passiveEvent=new Event('listener-passive',{cancelable:true});"
          "optionChild.addEventListener('listener-passive',function(e){"
          " e.preventDefault();"
          "},{passive:true});"
          "if(!optionChild.dispatchEvent(passiveEvent) ||"
          " passiveEvent.defaultPrevented)"
          " throw Error('passive cancellation');"
          "var cancelBubbleParentCalls=0;"
          "optionRoot.addEventListener('cancel-bubble-alias',function(){"
          " cancelBubbleParentCalls++;});"
          "var cancelBubbleEvent=new Event('cancel-bubble-alias',"
          " {bubbles:true});"
          "optionChild.addEventListener('cancel-bubble-alias',function(e){"
          " e.cancelBubble=true;"
          " if(!e.cancelBubble) throw Error('cancelBubble getter');"
          "});"
          "optionChild.dispatchEvent(cancelBubbleEvent);"
          "if(cancelBubbleParentCalls!==0)"
          " throw Error('cancelBubble propagation');"
          "var returnValueEvent=new Event('return-value-alias',"
          " {cancelable:true});"
          "optionChild.addEventListener('return-value-alias',function(e){"
          " e.returnValue=false;"
          "});"
          "if(optionChild.dispatchEvent(returnValueEvent)!==false ||"
          " returnValueEvent.returnValue!==false)"
          " throw Error('returnValue cancellation');"
          "var linkedController=new AbortController();"
          "var linkedCalls=0;"
          "optionChild.addEventListener('listener-signal',function(){"
          " linkedCalls++;"
          "},{signal:linkedController.signal});"
          "linkedController.abort();"
          "optionChild.dispatchEvent(new Event('listener-signal'));"
          "optionChild.addEventListener('listener-signal',function(){"
          " linkedCalls+=10;"
          "},{signal:linkedController.signal});"
          "optionChild.dispatchEvent(new Event('listener-signal'));"
          "if(linkedCalls!==0) throw Error('signal removal');"
          "var objectCalls=0;"
          "var listenerObject={handleEvent:function(e){"
          " if(this!==listenerObject || e.currentTarget!==optionChild)"
          "  throw Error('handleEvent receiver');"
          " objectCalls++;"
          "}};"
          "optionChild.addEventListener('listener-object',listenerObject);"
          "optionChild.dispatchEvent(new Event('listener-object'));"
          "optionChild.removeEventListener('listener-object',listenerObject);"
          "optionChild.dispatchEvent(new Event('listener-object'));"
          "if(objectCalls!==1) throw Error('listener object removal');"
          "var dualCalls='';"
          "function dualListener(e){dualCalls+=e.eventPhase+';';}"
          "optionChild.addEventListener('listener-dual',dualListener,true);"
          "optionChild.addEventListener('listener-dual',dualListener,false);"
          "optionChild.removeEventListener('listener-dual',dualListener,true);"
          "optionChild.dispatchEvent(new Event('listener-dual'));"
          "if(dualCalls!=='2;') throw Error('capture-keyed removal');"
          "var detachedWindowCalls=0;"
          "addEventListener('listener-detached',function(){"
          " detachedWindowCalls++;},{capture:true});"
          "document.createElement('div').dispatchEvent("
          " new Event('listener-detached',{bubbles:true}));"
          "if(detachedWindowCalls!==0) throw Error('detached event path');"
          "var abortObjectCalls=0;"
          "var abortListenerObject={handleEvent:function(e){"
          " if(this!==abortListenerObject || e.type!=='abort')"
          "  throw Error('abort handleEvent receiver');"
          " abortObjectCalls++;"
          "}};"
          "var abortOptionOwner=new AbortController();"
          "var abortRemovalOwner=new AbortController();"
          "abortOptionOwner.signal.addEventListener('abort',"
          " abortListenerObject,{capture:true,"
          " signal:abortRemovalOwner.signal});"
          "abortRemovalOwner.abort();abortOptionOwner.abort();"
          "if(abortObjectCalls!==0)"
          " throw Error('AbortSignal linked removal');"
          "var abortKeptOwner=new AbortController();"
          "abortKeptOwner.signal.addEventListener('abort',"
          " abortListenerObject,true);"
          "abortKeptOwner.signal.removeEventListener('abort',"
          " abortListenerObject,false);"
          "abortKeptOwner.abort();"
          "if(abortObjectCalls!==1)"
          " throw Error('AbortSignal capture identity');",
          "event-listener-options.js", err, sizeof(err)) == 0,
          err[0] ? err :
          "capture, once, passive, signal, and listener-object options");

    CHECK(jsdom_eval(j,
          "var uiEvent=new UIEvent('scroll',{"
          " bubbles:true,view:window,detail:2});"
          "var mouseEvent=new MouseEvent('click',{"
          " bubbles:true,cancelable:true,clientX:12,clientY:34,"
          " pageX:15,pageY:37,button:1,buttons:4,ctrlKey:true,"
          " relatedTarget:document.body});"
          "var keyboardEvent=new KeyboardEvent('keydown',{"
          " key:'K',code:'KeyK',location:KeyboardEvent.DOM_KEY_LOCATION_LEFT,"
          " ctrlKey:true,shiftKey:true,repeat:true,isComposing:true,"
          " keyCode:75,charCode:0,which:75});"
          "var focusEvent=new FocusEvent('focusin',{"
          " bubbles:true,relatedTarget:document.body});"
          "var inputEvent=new InputEvent('beforeinput',{"
          " bubbles:true,cancelable:true,data:'x',"
          " inputType:'insertText',isComposing:true});"
          "var pointerEvent=new PointerEvent('pointerdown',{"
          " bubbles:true,clientX:8,clientY:9,pointerId:7,width:3,height:4,"
          " pressure:0.5,tiltX:10,tiltY:-10,twist:90,"
          " pointerType:'pen',isPrimary:true,buttons:1});"
          "if(!(uiEvent instanceof UIEvent) || !(uiEvent instanceof Event) ||"
          " uiEvent.view!==window || uiEvent.detail!==2 || !uiEvent.bubbles ||"
          " !(mouseEvent instanceof MouseEvent) ||"
          " !(mouseEvent instanceof UIEvent) ||"
          " !(mouseEvent instanceof Event) ||"
          " mouseEvent.clientX!==12 || mouseEvent.clientY!==34 ||"
          " mouseEvent.pageX!==15 || mouseEvent.pageY!==37 ||"
          " mouseEvent.button!==1 || mouseEvent.buttons!==4 ||"
          " mouseEvent.relatedTarget!==document.body ||"
          " !mouseEvent.getModifierState('Control') ||"
          " mouseEvent.getModifierState('Shift') ||"
          " !(keyboardEvent instanceof KeyboardEvent) ||"
          " !(keyboardEvent instanceof UIEvent) ||"
          " keyboardEvent.key!=='K' || keyboardEvent.code!=='KeyK' ||"
          " keyboardEvent.location!==1 || !keyboardEvent.repeat ||"
          " !keyboardEvent.isComposing || keyboardEvent.keyCode!==75 ||"
          " keyboardEvent.which!==75 ||"
          " !keyboardEvent.getModifierState('Control') ||"
          " !keyboardEvent.getModifierState('Shift') ||"
          " keyboardEvent.getModifierState('CapsLock') ||"
          " !(focusEvent instanceof FocusEvent) ||"
          " focusEvent.relatedTarget!==document.body ||"
          " !(inputEvent instanceof InputEvent) || inputEvent.data!=='x' ||"
          " inputEvent.inputType!=='insertText' || !inputEvent.isComposing ||"
          " inputEvent.dataTransfer!==null ||"
          " !(pointerEvent instanceof PointerEvent) ||"
          " !(pointerEvent instanceof MouseEvent) ||"
          " pointerEvent.pointerId!==7 || pointerEvent.width!==3 ||"
          " pointerEvent.height!==4 || pointerEvent.pressure!==0.5 ||"
          " pointerEvent.tiltX!==10 || pointerEvent.tiltY!==-10 ||"
          " pointerEvent.twist!==90 || pointerEvent.pointerType!=='pen' ||"
          " !pointerEvent.isPrimary || pointerEvent.clientX!==8)"
          " throw Error('derived event shape');"
          "var derivedTarget=new EventTarget();"
          "var derivedSeen=false;"
          "derivedTarget.addEventListener('pointerdown',function(e){"
          " derivedSeen=e===pointerEvent && e.currentTarget===derivedTarget;"
          "});"
          "derivedTarget.dispatchEvent(pointerEvent);"
          "var mouseNeedsNew=false;"
          "try{MouseEvent('click');}catch(e){"
          " mouseNeedsNew=e instanceof TypeError;"
          "}"
          "if(!derivedSeen || !mouseNeedsNew)"
          " throw Error('derived event dispatch');",
          "derived-events.js", err, sizeof(err)) == 0,
          err[0] ? err :
          "UI, mouse, keyboard, focus, input, and pointer event interfaces");

    CHECK(jsdom_eval(j,
          "var rangeHost=document.createElement('section');"
          "rangeHost.innerHTML="
          " '<p>alpha <b>beta</b> gamma</p><p>delta</p>';"
          "document.body.appendChild(rangeHost);"
          "var firstRangeParagraph=rangeHost.children[0];"
          "var secondRangeParagraph=rangeHost.children[1];"
          "var range=document.createRange();"
          "if(!(range instanceof Range) || range.startContainer!==document ||"
          " range.endContainer!==document || range.startOffset!==0 ||"
          " range.endOffset!==0 || !range.collapsed)"
          " throw Error('Range defaults');"
          "range.selectNodeContents(firstRangeParagraph);"
          "var rangeClone=range.cloneRange();"
          "var clonedContents=range.cloneContents();"
          "var contextual=range.createContextualFragment("
          " '<i data-range=\"yes\">context</i><u>tail</u>');"
          "if(range.toString()!=='alpha beta gamma' || range.collapsed ||"
          " range.commonAncestorContainer!==firstRangeParagraph ||"
          " !(rangeClone instanceof Range) || rangeClone===range ||"
          " rangeClone.toString()!=='alpha beta gamma' ||"
          " range.compareBoundaryPoints(Range.START_TO_START,rangeClone)!==0 ||"
          " range.compareBoundaryPoints(Range.START_TO_END,rangeClone)!==1 ||"
          " range.compareBoundaryPoints(Range.END_TO_START,rangeClone)!==-1 ||"
          " !range.intersectsNode(firstRangeParagraph.querySelector('b')) ||"
          " range.intersectsNode(secondRangeParagraph) ||"
          " !(clonedContents instanceof DocumentFragment) ||"
          " clonedContents.textContent!=='alpha beta gamma' ||"
          " contextual.children.length!==2 ||"
          " contextual.firstElementChild.localName!=='i' ||"
          " contextual.firstElementChild.dataset.range!=='yes' ||"
          " contextual.textContent!=='contexttail' ||"
          " !range.isPointInRange(firstRangeParagraph,1) ||"
          " range.comparePoint(firstRangeParagraph,0)!==0)"
          " throw Error('Range contents');"
          "range.selectNode(secondRangeParagraph);"
          "if(range.toString()!=='delta' ||"
          " range.cloneContents().firstElementChild.localName!=='p')"
          " throw Error('Range selectNode');"
          "var textRangeNode=document.createTextNode('abcdef');"
          "rangeHost.appendChild(textRangeNode);"
          "var textRange=new Range();"
          "textRange.setStart(textRangeNode,1);"
          "textRange.setEnd(textRangeNode,4);"
          "if(textRange.toString()!=='bcd' ||"
          " textRange.cloneContents().textContent!=='bcd' ||"
          " textRange.comparePoint(textRangeNode,0)!==-1 ||"
          " textRange.comparePoint(textRangeNode,2)!==0 ||"
          " textRange.comparePoint(textRangeNode,5)!==1)"
          " throw Error('Range text boundaries');"
          "var extractedText=textRange.extractContents();"
          "if(extractedText.textContent!=='bcd' ||"
          " textRangeNode.data!=='aef' || !textRange.collapsed ||"
          " textRange.startContainer!==textRangeNode ||"
          " textRange.startOffset!==1)"
          " throw Error('Range text extraction');"
          "var insertionHost=document.createElement('div');"
          "var insertionTail=document.createElement('span');"
          "insertionTail.textContent='tail';"
          "insertionHost.appendChild(insertionTail);"
          "rangeHost.appendChild(insertionHost);"
          "var insertionRange=document.createRange();"
          "insertionRange.selectNodeContents(insertionHost);"
          "insertionRange.collapse(true);"
          "var insertionHead=document.createElement('i');"
          "insertionHead.textContent='head';"
          "insertionRange.insertNode(insertionHead);"
          "if(insertionHost.firstElementChild!==insertionHead ||"
          " insertionHost.textContent!=='headtail')"
          " throw Error('Range insertion');"
          "var surroundHost=document.createElement('div');"
          "surroundHost.innerHTML='<b>x</b><i>y</i>';"
          "rangeHost.appendChild(surroundHost);"
          "var surroundRange=document.createRange();"
          "surroundRange.selectNodeContents(surroundHost);"
          "var surroundMark=document.createElement('mark');"
          "surroundRange.surroundContents(surroundMark);"
          "if(surroundHost.children.length!==1 ||"
          " surroundHost.firstElementChild!==surroundMark ||"
          " surroundMark.children.length!==2 ||"
          " surroundMark.textContent!=='xy' ||"
          " surroundRange.toString()!=='xy')"
          " throw Error('Range surroundContents');"
          "var selection=getSelection();"
          "if(selection!==document.getSelection() ||"
          " !(selection instanceof Selection) || selection.rangeCount!==0 ||"
          " selection.type!=='None' || !selection.isCollapsed ||"
          " selection.anchorNode!==null || selection.toString()!=='')"
          " throw Error('Selection defaults');"
          "selection.addRange(rangeClone);"
          "if(selection.rangeCount!==1 || selection.getRangeAt(0)!==rangeClone ||"
          " selection.anchorNode!==firstRangeParagraph ||"
          " selection.anchorOffset!==0 ||"
          " selection.focusNode!==firstRangeParagraph ||"
          " selection.focusOffset!==firstRangeParagraph.childNodes.length ||"
          " selection.type!=='Range' || selection.isCollapsed ||"
          " selection.direction!=='forward' ||"
          " selection.toString()!=='alpha beta gamma' ||"
          " !selection.containsNode(firstRangeParagraph,true) ||"
          " selection.containsNode(secondRangeParagraph,true))"
          " throw Error('Selection range');"
          "selection.collapseToStart();"
          "if(!selection.isCollapsed || selection.anchorOffset!==0)"
          " throw Error('Selection collapseToStart');"
          "selection.removeRange(rangeClone);"
          "if(selection.rangeCount!==0)"
          " throw Error('Selection removeRange');"
          "rangeClone.selectNodeContents(firstRangeParagraph);"
          "selection.addRange(rangeClone);"
          "selection.collapseToEnd();"
          "if(!selection.isCollapsed ||"
          " selection.anchorOffset!==firstRangeParagraph.childNodes.length)"
          " throw Error('Selection collapseToEnd');"
          "selection.collapse(textRangeNode,1);"
          "if(selection.type!=='Caret' || !selection.isCollapsed ||"
          " selection.anchorNode!==textRangeNode ||"
          " selection.anchorOffset!==1)"
          " throw Error('Selection collapse');"
          "selection.selectAllChildren(secondRangeParagraph);"
          "if(selection.toString()!=='delta')"
          " throw Error('Selection selectAllChildren');"
          "selection.removeAllRanges();"
          "var emptySelectionError=false;"
          "try{selection.getRangeAt(0);}catch(e){"
          " emptySelectionError=e instanceof DOMException &&"
          " e.name==='IndexSizeError';"
          "}"
          "if(selection.rangeCount!==0 || !emptySelectionError)"
          " throw Error('Selection removal');",
          "range-selection.js", err, sizeof(err)) == 0,
          err[0] ? err :
          "bounded Range editing and singleton Selection state");

    CHECK(jsdom_eval(j,
          "var modernRoot=document.createElement('div');"
          "modernRoot.id='dom-modern';modernRoot.className='panel';"
          "modernRoot.classList.add('ready','interactive');"
          "if(modernRoot.classList.length!==3 ||"
          " modernRoot.classList.item(1)!=='ready' ||"
          " !modernRoot.classList.replace('ready','mounted') ||"
          " modernRoot.classList.replace('missing','unused') ||"
          " !modernRoot.classList.toggle('forced',true) ||"
          " modernRoot.classList.toggle('forced',false) ||"
          " modernRoot.classList.contains('forced') ||"
          " modernRoot.classList.value.indexOf('mounted')<0 ||"
          " modernRoot.classList.toString()!==modernRoot.className)"
          " throw Error('DOMTokenList');"
          "var modernStyle=modernRoot.style;"
          "modernStyle.setProperty('border-color','#123456');"
          "modernStyle.setProperty('--accent','teal','important');"
          "modernStyle.setProperty('ignored','value','invalid');"
          "if(modernStyle.length!==2 ||"
          " modernStyle.item(0)!=='border-color' ||"
          " modernStyle.getPropertyValue('border-color')!=='#123456' ||"
          " modernStyle.getPropertyValue('--accent')!=='teal' ||"
          " modernStyle.getPropertyPriority('--accent')!=='important' ||"
          " modernStyle.getPropertyValue('ignored')!=='' ||"
          " modernStyle.removeProperty('border-color')!=='#123456' ||"
          " modernStyle.getPropertyValue('border-color')!=='' ||"
          " modernStyle.removeProperty('missing')!=='')"
          " throw Error('CSSStyleDeclaration methods');"
          "modernStyle.width='20px';"
          "if(modernStyle.width!=='20px' || modernStyle.length!==2 ||"
          " modernStyle.cssText.indexOf('--accent:teal !important;')<0)"
          " throw Error('CSSStyleDeclaration property');"
          "document.body.appendChild(modernRoot);"
          "var modernA=document.createElement('p');"
          "modernA.className='item';modernA.setAttribute('data-kind','x');"
          "modernA.setAttribute('data-user-id','7');"
          "var modernDataset=modernA.dataset;"
          "if(modernDataset!==modernA.dataset ||"
          " modernDataset.kind!=='x' || modernDataset.userId!=='7' ||"
          " Object.keys(modernDataset).join(',').indexOf('userId')<0)"
          " throw Error('dataset read');"
          "modernDataset.userId='8';"
          "if(modernA.getAttribute('data-user-id')!=='8')"
          " throw Error('dataset write');"
          "modernA.setAttribute('data-new-key','fresh');"
          "if(modernA.dataset.newKey!=='fresh')"
          " throw Error('dataset refresh');"
          "var attributeMap=modernA.attributes;"
          "var dataAttribute=attributeMap.getNamedItem('DATA-NEW-KEY');"
          "if(!(attributeMap instanceof NamedNodeMap) ||"
          " attributeMap.length!==modernA.getAttributeNames().length ||"
          " !(attributeMap.item(0) instanceof Attr) ||"
          " !(attributeMap[0] instanceof Attr) ||"
          " attributeMap['data-new-key'].value!=='fresh' ||"
          " attributeMap.item(999)!==null ||"
          " !(dataAttribute instanceof Attr) ||"
          " !(dataAttribute instanceof Node) ||"
          " dataAttribute.name!=='data-new-key' ||"
          " dataAttribute.nodeName!=='data-new-key' ||"
          " dataAttribute.localName!=='data-new-key' ||"
          " dataAttribute.nodeType!==Node.ATTRIBUTE_NODE ||"
          " dataAttribute.ownerElement!==modernA ||"
          " dataAttribute.ownerDocument!==document ||"
          " !dataAttribute.specified || dataAttribute.namespaceURI!==null ||"
          " attributeMap.getNamedItem('missing')!==null ||"
          " attributeMap.getNamedItemNS(null,'data-new-key').value!=='fresh' ||"
          " modernA.getAttributeNode('data-new-key').value!=='fresh')"
          " throw Error('Attr NamedNodeMap');"
          "dataAttribute.value='updated';"
          "if(modernA.getAttribute('data-new-key')!=='updated' ||"
          " dataAttribute.nodeValue!=='updated')"
          " throw Error('Attr live value');"
          "modernA.textContent='A';"
          "modernA.name='shared';"
          "var modernB=document.createElement('p');modernB.textContent='B';"
          "modernB.className='item secondary';modernB.name='shared';"
          "modernRoot.append('lead',modernA,modernB);"
          "var modernHead=document.createElement('span');"
          "modernHead.textContent='head';"
          "modernRoot.prepend(modernHead,'middle');"
          "var identityText=document.createTextNode('identity');"
          "if(modernA.nodeType!==Node.ELEMENT_NODE ||"
          " identityText.nodeType!==Node.TEXT_NODE ||"
          " document.nodeType!==Node.DOCUMENT_NODE ||"
          " modernA.nodeName!=='P' || modernA.localName!=='p' ||"
          " modernA.namespaceURI!=='http://www.w3.org/1999/xhtml' ||"
          " modernA.prefix!==null || modernA.ownerDocument!==document ||"
          " document.ownerDocument!==null || document.nodeName!=='#document' ||"
          " identityText.nodeName!=='#text' ||"
          " identityText.nodeValue!=='identity' ||"
          " !(modernA instanceof Node) || !(modernA instanceof Element) ||"
          " !(modernA instanceof HTMLElement) ||"
          " !(document instanceof Node) || document instanceof Element)"
          " throw Error('Node identity');"
          "identityText.nodeValue='updated';"
          "if(identityText.nodeValue!=='updated' ||"
          " identityText.textContent!=='updated')"
          " throw Error('nodeValue mutation');"
          "if(modernRoot.firstChild.nodeType!==1 ||"
          " modernRoot.firstElementChild!==modernHead ||"
          " modernRoot.lastElementChild!==modernB ||"
          " modernRoot.childElementCount!==3 ||"
          " modernA.previousElementSibling!==modernHead ||"
          " modernA.nextElementSibling!==modernB ||"
          " !modernRoot.isConnected || !modernRoot.contains(modernA) ||"
          " modernRoot.contains(null) ||"
          " !modernRoot.matches('div#dom-modern.panel') ||"
          " !modernA.matches('.item[data-kind=x]') ||"
          " modernA.closest('#dom-modern')!==modernRoot ||"
          " modernRoot.getElementsByClassName('item').length!==2 ||"
          " modernRoot.getElementsByClassName('item secondary')[0]!==modernB ||"
          " document.getElementsByName('shared').length!==2 ||"
          " !document.contains(modernA) ||"
          " !modernA.isSameNode(modernA) || modernA.isSameNode(modernB) ||"
          " !(modernA.compareDocumentPosition(modernB) &"
          "   Node.DOCUMENT_POSITION_FOLLOWING) ||"
          " !(modernRoot.compareDocumentPosition(modernA) &"
          "   Node.DOCUMENT_POSITION_CONTAINED_BY) ||"
          " !(modernA.compareDocumentPosition(modernRoot) &"
          "   Node.DOCUMENT_POSITION_CONTAINS) ||"
          " !modernA.hasAttributes() ||"
          " !modernA.getAttributeNames().includes('data-user-id'))"
          " throw Error('modern traversal');"
          "if(!modernA.toggleAttribute('hidden') ||"
          " !modernA.hasAttribute('hidden') ||"
          " modernA.toggleAttribute('hidden',false) ||"
          " modernA.hasAttribute('hidden'))"
          " throw Error('toggleAttribute');"
          "var modernClone=modernA.cloneNode(true);"
          "if(modernClone===modernA || modernClone.textContent!=='A' ||"
          " modernClone.isConnected || !modernClone.isEqualNode(modernA) ||"
          " modernClone.isSameNode(modernA)) throw Error('cloneNode');"
          "var normalizeNode=document.createElement('div');"
          "normalizeNode.append(new Text('a'),new Text(''),new Text('b'));"
          "normalizeNode.normalize();"
          "if(normalizeNode.childNodes.length!==1 ||"
          " normalizeNode.firstChild.data!=='ab')"
          " throw Error('normalize');"
          "modernRoot.insertBefore(modernClone,modernB);"
          "var modernReplacement=document.createElement('em');"
          "modernReplacement.textContent='replacement';"
          "if(modernRoot.replaceChild(modernReplacement,modernClone)!=="
          "modernClone || modernClone.isConnected)"
          " throw Error('replaceChild');"
          "modernReplacement.remove();modernB.remove();"
          "if(modernReplacement.isConnected || modernB.isConnected)"
          " throw Error('remove');"
          "var finalStrong=document.createElement('strong');"
          "finalStrong.textContent='final';"
          "modernRoot.replaceChildren('done-',finalStrong);"
          "if(modernRoot.textContent!=='done-final' ||"
          " modernRoot.childElementCount!==1 ||"
          " modernRoot.getElementsByTagName('strong')[0]!==finalStrong)"
          " throw Error('replaceChildren');"
          "finalStrong.insertAdjacentHTML('afterbegin',"
          " '<b class=\"adjacent\">bold</b>');"
          "finalStrong.insertAdjacentText('beforeend','-tail');"
          "var adjacentB=finalStrong.querySelector('.adjacent');"
          "adjacentB.outerHTML='<u id=\"outer-replacement\">under</u>';"
          "var adjacentU=document.getElementById('outer-replacement');"
          "var adjacentI=document.createElement('i');"
          "adjacentI.textContent='italic';"
          "finalStrong.insertAdjacentElement('beforebegin',adjacentI);"
          "adjacentI.before('left-');adjacentI.after('-right');"
          "var adjacentEm=document.createElement('em');"
          "adjacentEm.textContent='em';"
          "adjacentI.replaceWith(adjacentEm);"
          "if(!adjacentU || adjacentB.isConnected ||"
          " adjacentEm.previousSibling.textContent!=='left-' ||"
          " adjacentEm.nextSibling.textContent!=='-right' ||"
          " finalStrong.textContent!=='underfinal-tail')"
          " throw Error('adjacent insertion');"
          "var constructedText=new Text('made');"
          "var constructedComment=new Comment();"
          "var characterOps=new Text('abcdef');"
          "constructedText.data='changed';"
          "if(characterOps.substringData(1,3)!=='bcd')"
          " throw Error('substringData');"
          "characterOps.appendData('g');"
          "characterOps.insertData(1,'X');"
          "characterOps.deleteData(2,2);"
          "characterOps.replaceData(2,3,'YZ');"
          "var domError=new DOMException('outside','IndexSizeError');"
          "var caughtDomError=false;"
          "try{characterOps.substringData(99,1);}catch(e){"
          " caughtDomError=e instanceof DOMException &&"
          " e.name==='IndexSizeError' && e.code===DOMException.INDEX_SIZE_ERR;"
          "}"
          "if(!(document instanceof HTMLDocument) ||"
          " !(document instanceof Document) ||"
          " !(constructedText instanceof Text) ||"
          " !(constructedText instanceof CharacterData) ||"
          " !(constructedText instanceof Node) ||"
          " constructedText.data!=='changed' ||"
          " constructedText.nodeValue!=='changed' ||"
          " constructedText.textContent!=='changed' ||"
          " constructedText.length!==7 ||"
          " characterOps.data!=='aXYZg' || characterOps.length!==5 ||"
          " !(domError instanceof DOMException) ||"
          " domError.name!=='IndexSizeError' || domError.message!=='outside' ||"
          " domError.code!==1 ||"
          " domError.toString()!=='IndexSizeError: outside' ||"
          " !caughtDomError ||"
          " !(constructedComment instanceof Comment) ||"
          " !(constructedComment instanceof CharacterData) ||"
          " constructedComment.data!=='' || constructedComment.length!==0)"
          " throw Error('character data identity');"
          "var fragment=document.createDocumentFragment();"
          "var fragmentComment=document.createComment('marker');"
          "var fragmentElement=document.createElement('b');"
          "fragmentElement.textContent='fragment';"
          "fragment.append(fragmentComment,fragmentElement,'-tail');"
          "if(!(fragment instanceof DocumentFragment) ||"
          " !(fragment instanceof Node) ||"
          " fragment.nodeType!==Node.DOCUMENT_FRAGMENT_NODE ||"
          " fragment.nodeName!=='#document-fragment' ||"
          " fragment.ownerDocument!==document || fragment.isConnected ||"
          " fragment.getRootNode()!==fragment ||"
          " !fragment.hasChildNodes() || fragment.childNodes.length!==3 ||"
          " fragment.children.length!==1 ||"
          " fragmentComment.nodeType!==Node.COMMENT_NODE ||"
          " fragmentComment.nodeName!=='#comment' ||"
          " !(fragmentComment instanceof Comment) ||"
          " !(fragmentComment instanceof CharacterData) ||"
          " fragmentComment.nodeValue!=='marker')"
          " throw Error('DocumentFragment identity');"
          "fragmentComment.nodeValue='updated-marker';"
          "var fragmentClone=fragment.cloneNode(true);"
          "if(fragmentClone.childNodes.length!==3 ||"
          " fragmentClone.textContent!=='fragment-tail')"
          " throw Error('DocumentFragment clone');"
          "var fragmentHost=document.createElement('section');"
          "document.body.appendChild(fragmentHost);"
          "fragmentHost.appendChild(fragment);"
          "if(fragment.hasChildNodes() || fragmentHost.childNodes.length!==3 ||"
          " fragmentHost.children.length!==1 ||"
          " fragmentComment.getRootNode()!==document ||"
          " fragmentHost.textContent!=='fragment-tail' ||"
          " document.getRootNode()!==document)"
          " throw Error('DocumentFragment insertion');",
          "modern-dom.js", err, sizeof(err)) == 0,
          err[0] ? err : "modern DOM traversal and mutation suite");

    CHECK(jsdom_eval(j,
          "var svgNS='http://www.w3.org/2000/svg';"
          "var svg=document.createElementNS(svgNS,'svg');"
          "var svgPath=document.createElementNS(svgNS,'path');"
          "svg.appendChild(svgPath);"
          "var custom=document.createElementNS('urn:kestrel:test',"
          " 'k:widget');"
          "var bare=document.createElementNS(null,'plain');"
          "var htmlDiv=document.createElement('div');"
          "var parsedHost=document.createElement('div');"
          "parsedHost.innerHTML='<svg><g><path></path></g></svg>';"
          "var parsedSvg=parsedHost.firstElementChild;"
          "var parsedPath=parsedHost.querySelector('path');"
          "var invalidNamespace=false;"
          "try{document.createElementNS(null,'x:y');}catch(e){"
          " invalidNamespace=e instanceof DOMException &&"
          " e.name==='NamespaceError';"
          "}"
          "if(!(svg instanceof SVGElement) || svg instanceof HTMLElement ||"
          " !(svg instanceof Element) || !(svgPath instanceof SVGElement) ||"
          " svg.namespaceURI!==svgNS || svgPath.namespaceURI!==svgNS ||"
          " svg.tagName!=='svg' || svgPath.localName!=='path' ||"
          " custom.namespaceURI!=='urn:kestrel:test' ||"
          " custom.prefix!=='k' || custom.localName!=='widget' ||"
          " custom instanceof HTMLElement || custom instanceof SVGElement ||"
          " bare.namespaceURI!==null || !(htmlDiv instanceof HTMLElement) ||"
          " !(parsedSvg instanceof SVGElement) ||"
          " !(parsedPath instanceof SVGElement) ||"
          " parsedPath.namespaceURI!==svgNS ||"
          " svg.getElementsByTagNameNS(svgNS,'path')[0]!==svgPath ||"
          " parsedHost.getElementsByTagNameNS('*','path').length!==1 ||"
          " parsedHost.getElementsByTagNameNS(null,'path').length!==0 ||"
          " svg.isEqualNode(document.createElement('svg')) ||"
          " !invalidNamespace)"
          " throw Error('namespace identity');",
          "dom-namespaces.js", err, sizeof(err)) == 0,
          err[0] ? err :
          "HTML, SVG, and custom namespace identity and propagation");

    CHECK(jsdom_eval(j,
          "var traversalRoot=document.createElement('div');"
          "traversalRoot.innerHTML="
          " '<p>A<span>S</span></p><p>B</p><!--C-->';"
          "var iteratorFilter=function(node){"
          " return node.tagName==='SPAN' ? NodeFilter.FILTER_REJECT :"
          " NodeFilter.FILTER_ACCEPT;"
          "};"
          "var iterator=document.createNodeIterator(traversalRoot,"
          " NodeFilter.SHOW_ELEMENT,iteratorFilter);"
          "if(!(iterator instanceof NodeIterator) ||"
          " iterator.root!==traversalRoot ||"
          " iterator.whatToShow!==NodeFilter.SHOW_ELEMENT ||"
          " iterator.filter!==iteratorFilter ||"
          " !iterator.pointerBeforeReferenceNode)"
          " throw Error('NodeIterator shape');"
          "var traversed='',iteratedNode;"
          "while((iteratedNode=iterator.nextNode())!==null)"
          " traversed+=iteratedNode.tagName+';';"
          "if(traversed!=='DIV;P;P;' ||"
          " iterator.referenceNode.textContent!=='B' ||"
          " iterator.previousNode().textContent!=='B' ||"
          " !iterator.pointerBeforeReferenceNode)"
          " throw Error('NodeIterator traversal');"
          "iterator.detach();"
          "var walkerFilter={acceptNode:function(node){"
          " return node.textContent==='AS' ? NodeFilter.FILTER_REJECT :"
          " NodeFilter.FILTER_ACCEPT;"
          "}};"
          "var walker=document.createTreeWalker(traversalRoot,"
          " NodeFilter.SHOW_ELEMENT,walkerFilter);"
          "if(!(walker instanceof TreeWalker) ||"
          " walker.currentNode!==traversalRoot ||"
          " walker.filter!==walkerFilter ||"
          " walker.nextNode().textContent!=='B' ||"
          " walker.parentNode()!==traversalRoot ||"
          " walker.firstChild().textContent!=='B')"
          " throw Error('TreeWalker traversal');"
          "var outsideWalker=false;"
          "try{walker.currentNode=document.body;}catch(e){"
          " outsideWalker=e instanceof DOMException &&"
          " e.name==='NotFoundError';"
          "}"
          "if(!outsideWalker) throw Error('TreeWalker root guard');",
          "dom-traversal.js", err, sizeof(err)) == 0,
          err[0] ? err :
          "NodeIterator, TreeWalker, and NodeFilter traversal");

    CHECK(jsdom_eval(j,
          "var mutationTarget=document.createElement('div');"
          "var mutationText=document.createTextNode('before');"
          "mutationTarget.appendChild(mutationText);"
          "document.body.appendChild(mutationTarget);"
          "var mutationCalls=0,mutationContext=false;"
          "var mutationObserver=new MutationObserver(function(records,same){"
          " mutationCalls++;"
          " mutationContext=this===mutationObserver && same===mutationObserver;"
          " if(records.length!==6) throw Error('mutation record count');"
          " if(!(records[0] instanceof MutationRecord) ||"
          "    records[0].type!=='attributes' ||"
          "    records[0].target!==mutationTarget ||"
          "    records[0].attributeName!=='data-watch' ||"
          "    records[0].oldValue!==null ||"
          "    records[0].addedNodes.length!==0 ||"
          "    records[0].removedNodes.length!==0)"
          "   throw Error('first attribute record');"
          " if(records[1].oldValue!=='first' ||"
          "    records[2].attributeName!=='class' ||"
          "    records[3].attributeName!=='style')"
          "   throw Error('attribute old values');"
          " if(records[4].type!=='childList' ||"
          "    records[4].addedNodes[0].textContent!=='child' ||"
          "    records[4].removedNodes.length!==0 ||"
          "    records[4].target!==mutationTarget)"
          "   throw Error('child record');"
          " if(records[5].type!=='characterData' ||"
          "    records[5].target!==mutationText ||"
          "    records[5].oldValue!=='before' ||"
          "    mutationText.nodeValue!=='after')"
          "   throw Error('character record');"
          " document.body.setAttribute('data-mutation-observer','delivered');"
          "});"
          "if(!(mutationObserver instanceof MutationObserver))"
          " throw Error('observer identity');"
          "mutationObserver.observe(mutationTarget,{attributes:true,"
          " attributeOldValue:true,childList:true,subtree:true,"
          " characterData:true,characterDataOldValue:true});"
          "mutationTarget.setAttribute('data-watch','first');"
          "mutationTarget.setAttribute('data-watch','second');"
          "mutationTarget.classList.add('watching');"
          "mutationTarget.style.color='red';"
          "var mutationChild=document.createElement('span');"
          "mutationChild.textContent='child';"
          "mutationTarget.appendChild(mutationChild);"
          "mutationText.nodeValue='after';"
          "if(mutationCalls!==0) throw Error('observer ran synchronously');"
          "var drainedCalls=0;"
          "var drainedTarget=document.createElement('div');"
          "var drainedSecond=document.createElement('div');"
          "var drainedObserver=new MutationObserver(function(){"
          " drainedCalls++;"
          "});"
          "drainedObserver.observe(drainedTarget,{attributeOldValue:true,"
          " attributeFilter:['DATA-PENDING']});"
          "drainedObserver.observe(drainedSecond,{attributes:true});"
          "drainedTarget.setAttribute('data-ignored','no');"
          "drainedTarget.setAttribute('data-pending','yes');"
          "drainedSecond.setAttribute('data-second','yes');"
          "var drainedRecords=drainedObserver.takeRecords();"
          "if(drainedRecords.length!==2 ||"
          " drainedRecords[0].attributeName!=='data-pending' ||"
          " drainedRecords[1].attributeName!=='data-second' ||"
          " drainedObserver.takeRecords().length!==0)"
          " throw Error('takeRecords drain');"
          "drainedTarget.setAttribute('data-pending','again');"
          "drainedObserver.disconnect();"
          "var observerNeedsNew=false,observerNeedsType=false;"
          "try{MutationObserver(function(){});}catch(e){"
          " observerNeedsNew=e instanceof TypeError;"
          "}"
          "try{new MutationObserver(function(){}).observe(drainedTarget,{});}"
          "catch(e){observerNeedsType=e instanceof TypeError;}"
          "if(!observerNeedsNew || !observerNeedsType)"
          " throw Error('observer validation');",
          "mutation-observer.js", err, sizeof(err)) == 0,
          err[0] ? err : "queue bounded MutationObserver records");
    CHECK(!dom_get_attr(doc->body, "data-mutation-observer"),
          "MutationObserver callback waits for a checkpoint");
    CHECK(jsdom_pump(j, err, sizeof(err)) >= 1,
          err[0] ? err : "MutationObserver checkpoint delivery");
    CHECK(jsdom_eval(j,
          "if(mutationCalls!==1 || !mutationContext || drainedCalls!==0 ||"
          " mutationObserver.takeRecords().length!==0)"
          " throw Error('observer delivery state');"
          "mutationObserver.disconnect();",
          "mutation-observer-after.js", err, sizeof(err)) == 0 &&
          dom_get_attr(doc->body, "data-mutation-observer") &&
          !strcmp(dom_get_attr(doc->body, "data-mutation-observer"),
                  "delivered"),
          err[0] ? err :
          "MutationObserver records, context, drain, and disconnect");

    CHECK(jsdom_eval(j,
          "document.addEventListener('DOMContentLoaded', function(){"
          " document.body.setAttribute('data-ready','yes'); });"
          "setTimeout(function(){"
          " document.getElementById('go').innerHTML='<b>Timer</b>';"
          "}, 1);",
          "events.js", err, sizeof(err)) == 0, "register document event/timer");
    CHECK(jsdom_eval(j,
          "if (atob(btoa('Kestrel')) !== 'Kestrel') throw Error('base64');"
          "if (navigator.platform.indexOf('KestrelOS') < 0 ||"
          " navigator.oscpu.indexOf('KestrelOS') < 0 ||"
          " navigator.appCodeName!=='Mozilla' ||"
          " navigator.appName!=='Netscape' ||"
          " navigator.product!=='Gecko' || navigator.vendor!=='' ||"
          " navigator.language!=='en-US' ||"
          " navigator.languages.join(',')!=='en-US,en' ||"
          " navigator.hardwareConcurrency!==4 || !navigator.onLine ||"
          " !navigator.cookieEnabled || navigator.maxTouchPoints!==0 ||"
          " navigator.webdriver || navigator.pdfViewerEnabled ||"
          " navigator.doNotTrack!==null ||"
          " typeof navigator.sendBeacon!=='function' ||"
          " innerWidth!==800 || innerHeight!==600 ||"
          " outerWidth!==800 || outerHeight!==600 ||"
          " devicePixelRatio!==1 || screen.width!==800 ||"
          " screen.height!==600 || screen.availWidth!==800 ||"
          " screen.availHeight!==600 || screen.colorDepth!==24 ||"
          " screen.pixelDepth!==24 ||"
          " screen.orientation.type!=='landscape-primary')"
          " throw Error('navigator');",
          "web-globals.js", err, sizeof(err)) == 0,
          "browser utility globals");
    CHECK(jsdom_eval(j,
          "var wide=matchMedia('screen and (min-width: 700px) and "
          "(orientation: landscape)');"
          "var narrow=matchMedia('(max-width: 799px)');"
          "var printQuery=matchMedia('print');"
          "if(!(wide instanceof EventTarget) || !wide.matches ||"
          " wide.media.indexOf('min-width')<0 || wide.onchange!==null ||"
          " narrow.matches || printQuery.matches)"
          " throw Error('media query result');"
          "var mediaChanges=0;"
          "function onMediaChange(){mediaChanges++;}"
          "wide.addListener(onMediaChange);"
          "wide.dispatchEvent(new Event('change'));"
          "wide.removeListener(onMediaChange);"
          "wide.dispatchEvent(new Event('change'));"
          "if(mediaChanges!==1) throw Error('media listener aliases');",
          "media-query.js", err, sizeof(err)) == 0,
          err[0] ? err : "matchMedia shares CSS media evaluation");
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
          "var intervalRuns=0;"
          "var intervalId=setInterval(function(){"
          " intervalRuns++;"
          " document.body.setAttribute('data-interval',''+intervalRuns);"
          " if(intervalRuns===2) clearInterval(intervalId);"
          "},1);"
          "queueMicrotask(function(){"
          " document.body.setAttribute('data-microtask','queued');"
          "});",
          "event-loop.js", err, sizeof(err)) == 0,
          err[0] ? err : "queue event-loop tasks");
    CHECK(!dom_get_attr(doc->body, "data-microtask"),
          "microtask waits for checkpoint");
    CHECK(jsdom_pump(j, err, sizeof(err)) >= 2,
          err[0] ? err : "first event-loop turn");
    CHECK(!strcmp(dom_get_attr(doc->body, "data-microtask"), "queued") &&
          !strcmp(dom_get_attr(doc->body, "data-interval"), "1"),
          "microtask and interval dispatch at checkpoint");
    CHECK(jsdom_pump(j, err, sizeof(err)) >= 1 &&
          !strcmp(dom_get_attr(doc->body, "data-interval"), "2"),
          "repeating timer advances through explicit states");
    CHECK(jsdom_pump(j, err, sizeof(err)) == 0,
          "clearInterval cancels future task turns");

    CHECK(jsdom_eval(j,
          "var timerArguments='',animationTimestamp=-1,idleRemaining=-1;"
          "var cancelledAnimation=0,cancelledIdle=0;"
          "setTimeout(function(a,b){timerArguments=a+b;},1,'left','right');"
          "var cancelledAnimationId=requestAnimationFrame(function(){"
          " cancelledAnimation++;});"
          "cancelAnimationFrame(cancelledAnimationId);"
          "requestAnimationFrame(function(timestamp){"
          " animationTimestamp=timestamp;});"
          "var cancelledIdleId=requestIdleCallback(function(){"
          " cancelledIdle++;});"
          "cancelIdleCallback(cancelledIdleId);"
          "requestIdleCallback(function(deadline){"
          " if(deadline.didTimeout) throw Error('idle timeout');"
          " idleRemaining=deadline.timeRemaining();});"
          "if(typeof performance.timeOrigin!=='number')"
          " throw Error('performance timeOrigin');",
          "timing-compat.js", err, sizeof(err)) == 0,
          err[0] ? err : "queue timer arguments, animation, and idle work");
    CHECK(jsdom_eval(j,
          "if(timerArguments!=='' || animationTimestamp!==-1 ||"
          " idleRemaining!==-1) throw Error('timing ran synchronously');",
          "timing-before-pump.js", err, sizeof(err)) == 0,
          err[0] ? err : "timing callbacks wait for checkpoint");
    CHECK(jsdom_pump(j, err, sizeof(err)) >= 3,
          err[0] ? err : "timing compatibility callbacks ran");
    CHECK(jsdom_eval(j,
          "if(timerArguments!=='leftright' || animationTimestamp<0 ||"
          " idleRemaining<=0 || cancelledAnimation!==0 || cancelledIdle!==0)"
          " throw Error('timing callback state');",
          "timing-after-pump.js", err, sizeof(err)) == 0,
          err[0] ? err : "timing arguments, timestamps, deadlines, and cancellation");

    CHECK(jsdom_eval(j,
          "var bytes=new Uint8Array([1,258,-1,4]);"
          "if(bytes.length!==4 || bytes.byteLength!==4 ||"
          " bytes[0]!==1 || bytes[1]!==2 || bytes[2]!==255)"
          " throw Error('construct');"
          "if(!ArrayBuffer.isView(bytes) ||"
          " ArrayBuffer.isView(bytes.buffer)) throw Error('isView');"
          "var shared=bytes.subarray(1,3);"
          "if(shared.byteOffset!==1 || shared.buffer!==bytes.buffer)"
          " throw Error('subarray');"
          "shared[0]=9;"
          "if(bytes[1]!==9) throw Error('shared write');"
          "bytes.set(bytes.subarray(0,3),1);"
          "if(bytes[0]!==1 || bytes[1]!==1 || bytes[2]!==9 ||"
          " bytes[3]!==255) throw Error('overlap set');"
          "var copied=bytes.slice(1,3);copied.fill(7);"
          "if(copied[0]!==7 || copied[1]!==7 || bytes[1]!==1)"
          " throw Error('slice/fill');"
          "if(!WebAssembly.validate(new Uint8Array([0,97,115,109,1,0,0,0])))"
          " throw Error('wasm typed bytes');",
          "uint8array.js", err, sizeof(err)) == 0,
          err[0] ? err : "Uint8Array views and byte operations");

    CHECK(jsdom_eval(j,
          "var codec=new TextEncoder();"
          "var encoded=codec.encode('Kestrel-é');"
          "if(encoded.length!==10 ||"
          " new TextDecoder().decode(encoded)!=='Kestrel-é')"
          " throw Error('text codec');"
          "var params=new URLSearchParams({q:'hello world',page:2});"
          "params.append('tag','os');params.append('tag','browser');"
          "if(params.getAll('tag').join(',')!=='os,browser' ||"
          " params.toString()!=='q=hello+world&page=2&tag=os&tag=browser')"
          " throw Error('url params');",
          "modern-webapi.js", err, sizeof(err)) == 0,
          err[0] ? err : "text codecs and URLSearchParams");

    CHECK(jsdom_eval(j,
          "var parsed=new URL('../api?q=hello%20world#part',"
          " 'https://user:pw@example.test:8443/a/b');"
          "if(parsed.origin!=='https://example.test:8443' ||"
          " parsed.protocol!=='https:' || parsed.username!=='user' ||"
          " parsed.password!=='pw' ||"
          " parsed.host!=='example.test:8443' ||"
          " parsed.hostname!=='example.test' || parsed.port!=='8443' ||"
          " parsed.pathname!=='/api' || parsed.search!=='?q=hello%20world' ||"
          " parsed.hash!=='#part' ||"
          " parsed.searchParams.get('q')!=='hello world')"
          " throw Error('parse properties');"
          "var live=parsed.searchParams;"
          "live.append('tag','a b');"
          "if(parsed.href!=="
          " 'https://user:pw@example.test:8443/api?"
          "q=hello+world&tag=a+b#part') throw Error('live append');"
          "parsed.search='?x=1&x=2';"
          "if(live!==parsed.searchParams || live.getAll('x').join(',')!=='1,2')"
          " throw Error('live replace');"
          "live.set('x','3');parsed.pathname='/v 1';parsed.hash='done';"
          "parsed.hostname='other.test';parsed.port='443';"
          "if(parsed.href!=="
          " 'https://user:pw@other.test/v%201?x=3#done')"
          " throw Error('mutations '+parsed.href);"
          "if(!URL.canParse('/ok','https://example.test/base') ||"
          " URL.canParse('relative-only')) throw Error('canParse');"
          "var noNew=false;try{URL('https://example.test/');}"
          "catch(e){noNew=e instanceof TypeError;}"
          "if(!noNew) throw Error('construct');",
          "url-api.js", err, sizeof(err)) == 0,
          err[0] ? err : "URL parsing, mutation, and live searchParams");

    CHECK(jsdom_eval(j,
          "var headers=new Headers({'X-Test':' one ','X-Count':2});"
          "if(headers.get('x-test')!=='one' ||"
          " headers.get('X-COUNT')!=='2' || !headers.has('x-test'))"
          " throw Error('record initializer');"
          "headers.append('X-Test','two');"
          "if(headers.get('x-test')!=='one, two') throw Error('append');"
          "headers.set('X-Test',' replacement ');"
          "headers.delete('x-count');"
          "if(headers.get('x-test')!=='replacement' ||"
          " headers.has('x-count')) throw Error('set delete');"
          "var pairs=new Headers([['A','1'],['a','2'],['B','3']]);"
          "if(pairs.get('a')!=='1, 2') throw Error('pair initializer');"
          "var clone=new Headers(pairs);pairs.set('a','changed');"
          "if(clone.get('a')!=='1, 2') throw Error('clone isolation');"
          "var visited='';"
          "clone.forEach((value,name,parent)=>{"
          " if(parent!==clone) throw Error('forEach parent');"
          " visited+=name+'='+value+';';"
          "});"
          "if(visited!=='a=1, 2;b=3;') throw Error('forEach order');"
          "var headerEntries=clone.entries();"
          "var headerEntryOne=headerEntries.next();"
          "var headerEntryTwo=headerEntries.next();"
          "var headerEntryDone=headerEntries.next();"
          "if(headerEntryOne.done || headerEntryOne.value.join('=')!=="
          " 'a=1, 2' || headerEntryTwo.value.join('=')!=='b=3' ||"
          " !headerEntryDone.done || headerEntryDone.value!==undefined)"
          " throw Error('Headers entries');"
          "if(clone.keys().next().value!=='a' ||"
          " clone.values().next().value!=='1, 2')"
          " throw Error('Headers keys values');"
          "var invalidHeader=false;"
          "try{headers.set('bad name','x');}"
          "catch(e){invalidHeader=e instanceof TypeError;}"
          "var headersNeedNew=false;"
          "try{Headers();}catch(e){headersNeedNew=e instanceof TypeError;}"
          "var forbiddenFetchHeader=false;"
          "try{fetch('/blocked',{headers:{Cookie:'secret'}});}"
          "catch(e){forbiddenFetchHeader=e instanceof TypeError;}"
          "if(!invalidHeader || !headersNeedNew || !forbiddenFetchHeader)"
          " throw Error('Headers validation');",
          "headers.js", err, sizeof(err)) == 0,
          err[0] ? err : "constructible bounded Headers collection");

    CHECK(jsdom_eval(j,
          "var randomA=new Uint8Array(32);"
          "var randomB=new Uint8Array(32);"
          "if(crypto.getRandomValues(randomA)!==randomA)"
          " throw Error('random identity');"
          "crypto.getRandomValues(randomB);"
          "var differs=false,nonzero=false;"
          "for(var ri=0;ri<randomA.length;ri++){"
          " if(randomA[ri]!==randomB[ri]) differs=true;"
          " if(randomA[ri]!==0) nonzero=true;"
          "}"
          "var uuid=crypto.randomUUID();"
          "if(!differs || !nonzero || uuid.length!==36 ||"
          " uuid[8]!=='-' || uuid[13]!=='-' || uuid[18]!=='-' ||"
          " uuid[23]!=='-' || uuid[14]!=='4' ||"
          " '89ab'.indexOf(uuid[19])<0) throw Error('secure random shape');"
          "var randomTypeFailed=false;"
          "try{crypto.getRandomValues([]);}"
          "catch(e){randomTypeFailed=e instanceof TypeError;}"
          "var randomQuotaFailed=false;"
          "try{crypto.getRandomValues(new Uint8Array(65537));}"
          "catch(e){randomQuotaFailed=e instanceof RangeError;}"
          "if(!randomTypeFailed || !randomQuotaFailed)"
          " throw Error('secure random validation');"
          "crypto.subtle.digest({name:'sha-256'},"
          " new TextEncoder().encode('abc')).then(buffer=>{"
          " var bytes=new Uint8Array(buffer),hex='';"
          " for(var di=0;di<bytes.length;di++){"
          "  var part=bytes[di].toString(16);"
          "  hex+=(part.length===1?'0':'')+part;"
          " }"
          " document.body.setAttribute('data-digest',hex);"
          "});"
          "Promise.all(["
          " crypto.subtle.digest('SHA-1',"
          "  new TextEncoder().encode('abc')),"
          " crypto.subtle.digest('SHA-384',"
          "  new TextEncoder().encode('abc')),"
          " crypto.subtle.digest('SHA-512',"
          "  new TextEncoder().encode('abc'))"
          "]).then(buffers=>{"
          " var joined='';"
          " for(var bi=0;bi<buffers.length;bi++){"
          "  var wideBytes=new Uint8Array(buffers[bi]);"
          "  for(var wi=0;wi<wideBytes.length;wi++){"
          "   var widePart=wideBytes[wi].toString(16);"
          "   joined+=(widePart.length===1?'0':'')+widePart;"
          "  }"
          "  joined+='|';"
          " }"
          " document.body.setAttribute('data-wide-digests',joined);"
          "});"
          "crypto.subtle.digest('MD5',new Uint8Array([1]))"
          ".catch(reason=>{"
          " document.body.setAttribute('data-digest-reject',reason.name);"
          "});",
          "crypto-api.js", err, sizeof(err)) == 0,
          err[0] ? err : "kernel-backed Web Crypto randomness");
    CHECK(jsdom_pump(j, err, sizeof(err)) > 0,
          err[0] ? err : "Web Crypto digest Promise checkpoint");
    CHECK(dom_get_attr(doc->body, "data-digest") &&
          !strcmp(dom_get_attr(doc->body, "data-digest"),
                  "ba7816bf8f01cfea414140de5dae2223"
                  "b00361a396177a9cb410ff61f20015ad") &&
          dom_get_attr(doc->body, "data-digest-reject") &&
          !strcmp(dom_get_attr(doc->body, "data-digest-reject"),
                  "NotSupportedError") &&
          dom_get_attr(doc->body, "data-wide-digests") &&
          !strcmp(dom_get_attr(doc->body, "data-wide-digests"),
                  "a9993e364706816aba3e25717850c26c9cd0d89d|"
                  "cb00753f45a35e8bb5a03d699ac65007"
                  "272c32ab0eded1631a8b605a43ff5bed"
                  "8086072ba1e7cc2358baeca134c825a7|"
                  "ddaf35a193617abacc417349ae204131"
                  "12e6fa4e89a97ea20a9eeee64b55d39"
                  "a2192992a274fc1a836ba3c23a3feeb"
                  "bd454d4423643ce80e2a9ac94fa54ca49f|"),
          "Web Crypto SHA-2 digests and rejection");

    CHECK(jsdom_eval(j,
          "var nestedBlob=new Blob(['!']);"
          "var madeBlob=new Blob(['A',new Uint8Array([66,67]),nestedBlob],"
          " {type:'Text/Plain'});"
          "if(!(madeBlob instanceof Blob) || madeBlob.size!==4 ||"
          " madeBlob.type!=='text/plain') throw Error('Blob shape');"
          "var blobSlice=madeBlob.slice(1,-1,'Text/Custom');"
          "if(blobSlice.size!==2 || blobSlice.type!=='text/custom')"
          " throw Error('Blob slice shape');"
          "madeBlob.text().then(text=>{"
          " document.body.setAttribute('data-blob-text',text);"
          "});"
          "blobSlice.arrayBuffer().then(buffer=>{"
          " document.body.setAttribute('data-blob-slice',"
          " new TextDecoder().decode(buffer));"
          "});"
          "madeBlob.bytes().then(bytes=>{"
          " document.body.setAttribute('data-blob-bytes',"
          " bytes.length+'-'+bytes[0]+'-'+bytes[3]);"
          "});"
          "var blobResponse=new Response(madeBlob);"
          "if(blobResponse.headers.get('content-type')!=='text/plain')"
          " throw Error('Blob Response type');"
          "blobResponse.text().then(text=>{"
          " document.body.setAttribute('data-blob-response',text);"
          "});"
          "var blobRequest=new Request('/blob',{method:'POST',body:madeBlob});"
          "if(blobRequest.headers.get('content-type')!=='text/plain')"
          " throw Error('Blob Request type');"
          "var madeFile=new File(['DATA'],'report.txt',"
          " {type:'Text/CSV',lastModified:1234});"
          "if(!(madeFile instanceof File) || !(madeFile instanceof Blob) ||"
          " madeFile.name!=='report.txt' || madeFile.size!==4 ||"
          " madeFile.type!=='text/csv' || madeFile.lastModified!==1234 ||"
          " madeFile.webkitRelativePath!=='') throw Error('File shape');"
          "var fileSlice=madeFile.slice(1,3);"
          "if(!(fileSlice instanceof Blob) || fileSlice instanceof File ||"
          " fileSlice.size!==2) throw Error('File slice');"
          "madeFile.text().then(text=>{"
          " document.body.setAttribute('data-file-text',text);"
          "});"
          "var textReader=new FileReader();"
          "var readerEvents='';"
          "textReader.onloadstart=()=>{readerEvents+='start;';};"
          "textReader.onprogress=event=>{"
          " if(!event.lengthComputable || event.loaded!==4 || event.total!==4)"
          "  throw Error('FileReader progress');"
          " readerEvents+='progress;';"
          "};"
          "textReader.onload=()=>{"
          " readerEvents+='load;';"
          " document.body.setAttribute('data-reader-text',textReader.result);"
          "};"
          "textReader.onloadend=()=>{"
          " readerEvents+='end;';"
          " document.body.setAttribute('data-reader-events',readerEvents);"
          "};"
          "textReader.readAsText(madeFile);"
          "if(!(textReader instanceof FileReader) ||"
          " !(textReader instanceof EventTarget) ||"
          " textReader.readyState!==FileReader.LOADING ||"
          " textReader.result!==null || textReader.error!==null ||"
          " textReader.EMPTY!==0 || textReader.DONE!==2)"
          " throw Error('FileReader loading state');"
          "var bufferReader=new FileReader();"
          "bufferReader.onload=()=>{"
          " document.body.setAttribute('data-reader-buffer',"
          " new TextDecoder().decode(bufferReader.result));"
          "};"
          "bufferReader.readAsArrayBuffer(madeFile);"
          "var binaryReader=new FileReader();"
          "binaryReader.onload=()=>{"
          " document.body.setAttribute('data-reader-binary',"
          " binaryReader.result);"
          "};"
          "binaryReader.readAsBinaryString(madeFile);"
          "var dataReader=new FileReader();"
          "dataReader.onload=()=>{"
          " document.body.setAttribute('data-reader-url',dataReader.result);"
          "};"
          "dataReader.readAsDataURL(madeFile);"
          "var abortReader=new FileReader();"
          "var abortEvents='';"
          "abortReader.onabort=()=>{abortEvents+='abort;';};"
          "abortReader.onloadend=()=>{"
          " abortEvents+='end;';"
          " document.body.setAttribute('data-reader-abort',"
          " abortReader.error.name+'-'+abortEvents);"
          "};"
          "abortReader.readAsText(madeFile);abortReader.abort();"
          "if(abortReader.readyState!==FileReader.DONE ||"
          " abortReader.result!==null || abortReader.error.name!=='AbortError')"
          " throw Error('FileReader abort state');"
          "var blobNeedsNew=false;"
          "try{Blob();}catch(e){blobNeedsNew=e instanceof TypeError;}"
          "var fileNeedsNew=false;"
          "try{File([], 'x');}catch(e){fileNeedsNew=e instanceof TypeError;}"
          "var readerNeedsNew=false;"
          "try{FileReader();}catch(e){readerNeedsNew=e instanceof TypeError;}"
          "if(!blobNeedsNew || !fileNeedsNew || !readerNeedsNew)"
          " throw Error('Blob/File constructor');",
          "blob-api.js", err, sizeof(err)) == 0,
          err[0] ? err : "bounded Blob construction, slicing, and readers");
    CHECK(jsdom_pump(j, err, sizeof(err)) > 0,
          err[0] ? err : "Blob reader Promise checkpoint");
    CHECK(dom_get_attr(doc->body, "data-blob-text") &&
          !strcmp(dom_get_attr(doc->body, "data-blob-text"), "ABC!") &&
          dom_get_attr(doc->body, "data-blob-slice") &&
          !strcmp(dom_get_attr(doc->body, "data-blob-slice"), "BC") &&
          dom_get_attr(doc->body, "data-blob-bytes") &&
          !strcmp(dom_get_attr(doc->body, "data-blob-bytes"), "4-65-33") &&
          dom_get_attr(doc->body, "data-blob-response") &&
          !strcmp(dom_get_attr(doc->body, "data-blob-response"), "ABC!") &&
          dom_get_attr(doc->body, "data-file-text") &&
          !strcmp(dom_get_attr(doc->body, "data-file-text"), "DATA") &&
          dom_get_attr(doc->body, "data-reader-text") &&
          !strcmp(dom_get_attr(doc->body, "data-reader-text"), "DATA") &&
          dom_get_attr(doc->body, "data-reader-events") &&
          !strcmp(dom_get_attr(doc->body, "data-reader-events"),
                  "start;progress;load;end;") &&
          dom_get_attr(doc->body, "data-reader-buffer") &&
          !strcmp(dom_get_attr(doc->body, "data-reader-buffer"), "DATA") &&
          dom_get_attr(doc->body, "data-reader-binary") &&
          !strcmp(dom_get_attr(doc->body, "data-reader-binary"), "DATA") &&
          dom_get_attr(doc->body, "data-reader-url") &&
          !strcmp(dom_get_attr(doc->body, "data-reader-url"),
                  "data:text/csv;base64,REFUQQ==") &&
          dom_get_attr(doc->body, "data-reader-abort") &&
          !strcmp(dom_get_attr(doc->body, "data-reader-abort"),
                  "AbortError-abort;end;"),
          "Blob, File, and asynchronous FileReader paths");

    CHECK(jsdom_eval(j,
          "var objectUrl=URL.createObjectURL("
          " new Blob(['OBJECT-DATA'],{type:'Text/Custom'}));"
          "if(objectUrl.indexOf('blob:https://example.test/')!==0)"
          " throw Error('object URL shape');"
          "fetch(objectUrl).then(response=>{"
          " if(response.status!==200 || response.url!==objectUrl ||"
          "    response.headers.get('content-type')!=='text/custom')"
          "  throw Error('object URL response');"
          " return response.blob();"
          "}).then(blob=>blob.text().then(text=>{"
          " document.body.setAttribute('data-object-url',"
          " blob.type+'-'+text);"
          " URL.revokeObjectURL(objectUrl);"
          " return fetch(objectUrl);"
          "})).then(()=>{"
          " document.body.setAttribute('data-object-revoked','bad');"
          "},()=>{"
          " document.body.setAttribute('data-object-revoked','ok');"
          "});",
          "object-url.js", err, sizeof(err)) == 0,
          err[0] ? err : "bounded Blob object URL creation");
    CHECK(jsdom_pump(j, err, sizeof(err)) > 0,
          err[0] ? err : "Blob object URL fetch/revoke checkpoints");
    CHECK(dom_get_attr(doc->body, "data-object-url") &&
          !strcmp(dom_get_attr(doc->body, "data-object-url"),
                  "text/custom-OBJECT-DATA") &&
          dom_get_attr(doc->body, "data-object-revoked") &&
          !strcmp(dom_get_attr(doc->body, "data-object-revoked"), "ok"),
          "Blob object URLs preserve bytes, MIME type, and revocation");

    CHECK(jsdom_eval(j,
          "var multipart=new FormData();"
          "multipart.append('tag','one');multipart.append('tag','two');"
          "var uploadBlob=new Blob(['FILE'],{type:'text/plain'});"
          "multipart.append('upload',uploadBlob,'note.txt');"
          "var uploadFile=new File(['CSV'],'table.csv',{type:'text/csv'});"
          "multipart.append('nativeFile',uploadFile);"
          "if(!(multipart instanceof FormData) ||"
          " multipart.get('tag')!=='one' ||"
          " multipart.getAll('tag').join(',')!=='one,two' ||"
          " multipart.get('upload')!==uploadBlob ||"
          " !multipart.has('upload')) throw Error('FormData values');"
          "multipart.set('tag','replacement');"
          "multipart.delete('missing');"
          "if(multipart.getAll('tag').join(',')!=='replacement')"
          " throw Error('FormData set');"
          "var formOrder='';"
          "multipart.forEach((value,name,parent)=>{"
          " if(parent!==multipart) throw Error('FormData parent');"
          " formOrder+=name+';';"
          "});"
          "if(formOrder!=='upload;nativeFile;tag;')"
          " throw Error('FormData order');"
          "var formEntries=multipart.entries();"
          "var formEntryOne=formEntries.next();"
          "var formEntryTwo=formEntries.next();"
          "if(formEntryOne.value[0]!=='upload' ||"
          " formEntryOne.value[1]!==uploadBlob ||"
          " formEntryTwo.value[0]!=='nativeFile' ||"
          " formEntryTwo.value[1]!==uploadFile ||"
          " multipart.keys().next().value!=='upload' ||"
          " multipart.values().next().value!==uploadBlob)"
          " throw Error('FormData iterators');"
          "var multipartRequest=new Request('/multipart',{"
          " method:'POST',body:multipart});"
          "var multipartType=multipartRequest.headers.get('content-type');"
          "if(multipartType.indexOf('multipart/form-data; boundary=')!==0)"
          " throw Error('FormData type');"
          "multipartRequest.text().then(body=>{"
          " var ok=body.indexOf('name=\"tag\"')>=0 &&"
          " body.indexOf('replacement')>=0 &&"
          " body.indexOf('name=\"upload\"; filename=\"note.txt\"')>=0 &&"
          " body.indexOf('Content-Type: text/plain')>=0 &&"
          " body.indexOf('FILE')>=0 &&"
          " body.indexOf('name=\"nativeFile\"; filename=\"table.csv\"')>=0 &&"
          " body.indexOf('Content-Type: text/csv')>=0 &&"
          " body.indexOf('CSV')>=0;"
          " document.body.setAttribute('data-formdata',ok?'ok':'bad');"
          "});"
          "new Request('/multipart-reader',{method:'POST',body:multipart})"
          ".formData().then(decoded=>{"
          " var decodedUpload=decoded.get('upload');"
          " var decodedNative=decoded.get('nativeFile');"
          " Promise.all([decodedUpload.text(),decodedNative.text()])"
          " .then(texts=>{"
          "  var ok=decoded.get('tag')==='replacement' &&"
          "  decodedUpload instanceof File &&"
          "  decodedUpload.name==='note.txt' &&"
          "  decodedUpload.type==='text/plain' && texts[0]==='FILE' &&"
          "  decodedNative instanceof File &&"
          "  decodedNative.name==='table.csv' &&"
          "  decodedNative.type==='text/csv' && texts[1]==='CSV';"
          "  document.body.setAttribute('data-formdata-read',"
          "  ok?'ok':'bad');"
          " });"
          "});"
          "var formNeedsNew=false;"
          "try{FormData();}catch(e){formNeedsNew=e instanceof TypeError;}"
          "if(!formNeedsNew) throw Error('FormData constructor');",
          "formdata-api.js", err, sizeof(err)) == 0,
          err[0] ? err : "FormData collection and multipart encoding");
    CHECK(jsdom_pump(j, err, sizeof(err)) > 0,
          err[0] ? err : "FormData body Promise checkpoint");
    CHECK(dom_get_attr(doc->body, "data-formdata") &&
          !strcmp(dom_get_attr(doc->body, "data-formdata"), "ok") &&
          dom_get_attr(doc->body, "data-formdata-read") &&
          !strcmp(dom_get_attr(doc->body, "data-formdata-read"), "ok"),
          "FormData serializes and decodes fields and File values");

    CHECK(jsdom_eval(j,
          "var madeResponse=new Response('hello',{status:202,"
          " statusText:'Accepted',headers:{'X-Made':'yes'}});"
          "if(!(madeResponse instanceof Response) ||"
          " madeResponse.status!==202 || madeResponse.statusText!=='Accepted' ||"
          " madeResponse.url!=='' || madeResponse.type!=='default' ||"
          " madeResponse.redirected ||"
          " madeResponse.headers.get('content-type')!=="
          " 'text/plain;charset=UTF-8' ||"
          " madeResponse.headers.get('x-made')!=='yes')"
          " throw Error('Response init');"
          "var responseClone=madeResponse.clone();"
          "madeResponse.text().then(text=>{"
          " document.body.setAttribute('data-response-text',text);"
          "});"
          "responseClone.arrayBuffer().then(buffer=>{"
          " document.body.setAttribute('data-response-clone',"
          " new TextDecoder().decode(buffer));"
          "});"
          "var usedCloneFailed=false;"
          "try{madeResponse.clone();}catch(e){"
          " usedCloneFailed=e instanceof TypeError;"
          "}"
          "if(!usedCloneFailed) throw Error('used clone');"
          "Response.json({ok:true,count:3},{status:201,"
          " headers:{'X-JSON':'yes'}}).json().then(data=>{"
          " document.body.setAttribute('data-response-json',"
          " data.ok+'-'+data.count);"
          "});"
          "new Response(new Uint8Array([68,65,84,65]),{"
          " headers:{'Content-Type':'Application/Octet-Stream'}})"
          ".blob().then(blob=>blob.text().then(text=>{"
          " document.body.setAttribute('data-response-blob',"
          " blob.type+'-'+text);"
          "}));"
          "new Response(new Uint8Array([7,8,9])).bytes().then(bytes=>{"
          " document.body.setAttribute('data-response-bytes',"
          " bytes.length+'-'+bytes[0]+'-'+bytes[2]);"
          "});"
          "new Response('a=1&a=two+words&encoded=%21',{headers:{"
          " 'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'}})"
          ".formData().then(form=>{"
          " document.body.setAttribute('data-response-formdata',"
          " form.getAll('a').join('|')+'-'+form.get('encoded'));"
          "});"
          "new Response('not-form').formData().catch(reason=>{"
          " document.body.setAttribute('data-response-formdata-reject',"
          " reason instanceof TypeError?'yes':'no');"
          "});"
          "var redirectResponse=Response.redirect('../moved',307);"
          "if(redirectResponse.status!==307 || redirectResponse.ok ||"
          " redirectResponse.type!=='default' || redirectResponse.redirected ||"
          " redirectResponse.headers.get('location')!=="
          " 'https://example.test/moved') throw Error('Response redirect');"
          "var errorResponse=Response.error();"
          "if(errorResponse.status!==0 || errorResponse.ok ||"
          " errorResponse.type!=='error' || errorResponse.url!=='' ||"
          " errorResponse.headers.has('content-type'))"
          " throw Error('Response error');"
          "var redirectStatusFailed=false;"
          "try{Response.redirect('/bad',200);}"
          "catch(e){redirectStatusFailed=e instanceof RangeError;}"
          "var bodyStatusFailed=false;"
          "try{new Response('body',{status:204});}"
          "catch(e){bodyStatusFailed=e instanceof TypeError;}"
          "var responseNeedsNew=false;"
          "try{Response();}catch(e){responseNeedsNew=e instanceof TypeError;}"
          "if(!bodyStatusFailed || !responseNeedsNew ||"
          " !redirectStatusFailed)"
          " throw Error('Response validation');",
          "response-api.js", err, sizeof(err)) == 0,
          err[0] ? err : "constructible Response, clone, and JSON factory");
    CHECK(jsdom_pump(j, err, sizeof(err)) > 0,
          err[0] ? err : "script-created Response Promise checkpoint");
    CHECK(dom_get_attr(doc->body, "data-response-text") &&
          !strcmp(dom_get_attr(doc->body, "data-response-text"), "hello") &&
          dom_get_attr(doc->body, "data-response-clone") &&
          !strcmp(dom_get_attr(doc->body, "data-response-clone"), "hello") &&
          dom_get_attr(doc->body, "data-response-json") &&
          !strcmp(dom_get_attr(doc->body, "data-response-json"), "true-3") &&
          dom_get_attr(doc->body, "data-response-blob") &&
          !strcmp(dom_get_attr(doc->body, "data-response-blob"),
                  "application/octet-stream-DATA") &&
          dom_get_attr(doc->body, "data-response-bytes") &&
          !strcmp(dom_get_attr(doc->body, "data-response-bytes"), "3-7-9") &&
          dom_get_attr(doc->body, "data-response-formdata") &&
          !strcmp(dom_get_attr(doc->body, "data-response-formdata"),
                  "1|two words-!") &&
          dom_get_attr(doc->body, "data-response-formdata-reject") &&
          !strcmp(dom_get_attr(doc->body,
                               "data-response-formdata-reject"), "yes"),
          "script-created Response bodies consume independently");

    CHECK(jsdom_eval(j,
          "var madeRequest=new Request('/submit',{method:'post',"
          " body:'a=1',headers:{Accept:'application/json','X-One':'1'}});"
          "if(!(madeRequest instanceof Request) ||"
          " madeRequest.url!=='https://example.test/submit' ||"
          " madeRequest.method!=='POST' || madeRequest.bodyUsed ||"
          " !(madeRequest.headers instanceof Headers) ||"
          " madeRequest.headers.get('content-type')!=="
          " 'text/plain;charset=UTF-8' ||"
          " !(madeRequest.signal instanceof AbortSignal) ||"
          " madeRequest.signal.aborted || madeRequest.cache!=='default' ||"
          " madeRequest.credentials!=='same-origin' ||"
          " madeRequest.destination!=='' || madeRequest.integrity!=='' ||"
          " madeRequest.mode!=='cors' || madeRequest.redirect!=='follow' ||"
          " madeRequest.referrer!=='about:client' ||"
          " madeRequest.referrerPolicy!=='' || madeRequest.keepalive ||"
          " madeRequest.isHistoryNavigation || madeRequest.isReloadNavigation)"
          " throw Error('Request shape');"
          "var requestCopy=madeRequest.clone();"
          "madeRequest.headers.set('x-one','changed');"
          "if(requestCopy.headers.get('x-one')!=='1')"
          " throw Error('Request clone headers');"
          "requestCopy.text().then(body=>{"
          " document.body.setAttribute('data-request-body',body);"
          "});"
          "var overridden=new Request(madeRequest,{method:'PUT',"
          " body:new Uint8Array([65,66]),headers:{'X-Two':'2'},"
          " mode:'same-origin',credentials:'include',cache:'no-store',"
          " redirect:'manual',referrer:'../source',"
          " referrerPolicy:'origin',integrity:'sha256-proof',"
          " keepalive:true});"
          "if(overridden.method!=='PUT' ||"
          " overridden.headers.has('x-one') ||"
          " overridden.headers.get('x-two')!=='2' ||"
          " overridden.mode!=='same-origin' ||"
          " overridden.credentials!=='include' ||"
          " overridden.cache!=='no-store' ||"
          " overridden.redirect!=='manual' ||"
          " overridden.referrer!=='https://example.test/source' ||"
          " overridden.referrerPolicy!=='origin' ||"
          " overridden.integrity!=='sha256-proof' ||"
          " !overridden.keepalive)"
          " throw Error('Request overrides');"
          "overridden.arrayBuffer().then(buffer=>{"
          " document.body.setAttribute('data-request-bytes',"
          " new TextDecoder().decode(buffer));"
          "});"
          "var formBody=new URLSearchParams({q:'hello world',page:2});"
          "var formRequest=new Request('/form',{method:'POST',body:formBody});"
          "if(formRequest.headers.get('content-type')!=="
          " 'application/x-www-form-urlencoded;charset=UTF-8')"
          " throw Error('URLSearchParams Request type');"
          "formRequest.text().then(body=>{"
          " document.body.setAttribute('data-request-form',body);"
          "});"
          "var formResponse=new Response(formBody);"
          "if(formResponse.headers.get('content-type')!=="
          " 'application/x-www-form-urlencoded;charset=UTF-8')"
          " throw Error('URLSearchParams Response type');"
          "new Request('/blob-reader',{method:'POST',body:'request blob',"
          " headers:{'Content-Type':'Text/Custom'}}).blob().then(blob=>{"
          " blob.text().then(text=>{"
          "  document.body.setAttribute('data-request-blob',"
          "  blob.type+'-'+text);"
          " });"
          "});"
          "new Request('/bytes-reader',{method:'POST',"
          " body:new Uint8Array([4,5,6])}).bytes().then(bytes=>{"
          " document.body.setAttribute('data-request-byte-reader',"
          " bytes.length+'-'+bytes[0]+'-'+bytes[2]);"
          "});"
          "new Request('/form-reader',{method:'POST',"
          " body:new URLSearchParams('q=hello+world&q=again')})"
          ".formData().then(form=>{"
          " document.body.setAttribute('data-request-formdata-reader',"
          " form.getAll('q').join('|'));"
          "});"
          "var getBodyFailed=false;"
          "try{new Request('/bad',{method:'GET',body:'x'});}"
          "catch(e){getBodyFailed=e instanceof TypeError;}"
          "var traceFailed=false;"
          "try{new Request('/bad',{method:'TRACE'});}"
          "catch(e){traceFailed=e instanceof TypeError;}"
          "var requestNeedsNew=false;"
          "try{Request('/bad');}"
          "catch(e){requestNeedsNew=e instanceof TypeError;}"
          "var cachedModeFailed=false;"
          "try{new Request('/bad',{cache:'only-if-cached',mode:'cors'});}"
          "catch(e){cachedModeFailed=e instanceof TypeError;}"
          "var noCorsMethodFailed=false;"
          "try{new Request('/bad',{mode:'no-cors',method:'PUT'});}"
          "catch(e){noCorsMethodFailed=e instanceof TypeError;}"
          "if(!getBodyFailed || !traceFailed || !requestNeedsNew ||"
          " !cachedModeFailed || !noCorsMethodFailed)"
          " throw Error('Request validation');",
          "request-api.js", err, sizeof(err)) == 0,
          err[0] ? err : "constructible Request, overrides, and cloning");
    CHECK(jsdom_pump(j, err, sizeof(err)) > 0,
          err[0] ? err : "script-created Request Promise checkpoint");
    CHECK(dom_get_attr(doc->body, "data-request-body") &&
          !strcmp(dom_get_attr(doc->body, "data-request-body"), "a=1") &&
          dom_get_attr(doc->body, "data-request-bytes") &&
          !strcmp(dom_get_attr(doc->body, "data-request-bytes"), "AB") &&
          dom_get_attr(doc->body, "data-request-form") &&
          !strcmp(dom_get_attr(doc->body, "data-request-form"),
                  "q=hello+world&page=2") &&
          dom_get_attr(doc->body, "data-request-blob") &&
          !strcmp(dom_get_attr(doc->body, "data-request-blob"),
                  "text/custom-request blob") &&
          dom_get_attr(doc->body, "data-request-byte-reader") &&
          !strcmp(dom_get_attr(doc->body, "data-request-byte-reader"),
                  "3-4-6") &&
          dom_get_attr(doc->body, "data-request-formdata-reader") &&
          !strcmp(dom_get_attr(doc->body,
                               "data-request-formdata-reader"),
                  "hello world|again"),
          "script-created Request bodies consume independently");

    CHECK(jsdom_eval(j,
          "var aborter=new AbortController();"
          "var signal=aborter.signal;"
          "if(signal.aborted || signal.reason!==undefined ||"
          " !(signal instanceof AbortSignal) ||"
          " !(signal instanceof EventTarget)) throw Error('initial signal');"
          "var abortEvents=0;"
          "var removedAbort=()=>{abortEvents+=100;};"
          "var keptAbort=e=>{"
          " if(!(e instanceof Event) || e.type!=='abort' || e.target!==signal ||"
          "    e.currentTarget!==signal) throw Error('abort event');"
          " abortEvents++;"
          "};"
          "signal.addEventListener('abort',removedAbort);"
          "signal.removeEventListener('abort',removedAbort);"
          "signal.addEventListener('abort',keptAbort);"
          "signal.addEventListener('abort',keptAbort);"
          "signal.onabort=()=>{abortEvents+=10;};"
          "var queuedAbort='pending';"
          "fetch('/must-not-run',{signal:signal}).catch(reason=>{"
          " queuedAbort=reason;"
          " document.body.setAttribute('data-abort-fetch',reason);"
          "});"
          "aborter.abort('cancelled');"
          "aborter.abort('ignored');"
          "if(!signal.aborted || signal.reason!=='cancelled' ||"
          " abortEvents!==11) throw Error('abort transition');"
          "var thrown=false;"
          "try{signal.throwIfAborted();}catch(reason){"
          " thrown=reason==='cancelled';"
          "}"
          "if(!thrown) throw Error('throwIfAborted');"
          "var defaultAborter=new AbortController();defaultAborter.abort();"
          "if(defaultAborter.signal.reason.name!=='AbortError')"
          " throw Error('default reason');"
          "var earlySignal=AbortSignal.abort('early');"
          "if(!earlySignal.aborted || earlySignal.reason!=='early')"
          " throw Error('static abort');"
          "fetch('/also-must-not-run',{signal:earlySignal}).catch(reason=>{"
          " document.body.setAttribute('data-abort-early',reason);"
          "});"
          "var ownerA=new AbortController();"
          "var ownerB=new AbortController();"
          "var composed=AbortSignal.any([ownerA.signal,ownerB.signal,"
          " ownerA.signal]);"
          "var composedEvents=0;"
          "composed.addEventListener('abort',()=>composedEvents++);"
          "fetch('/composed-must-not-run',{signal:composed}).catch(reason=>{"
          " document.body.setAttribute('data-abort-any',reason);"
          "});"
          "ownerB.abort('owner-b');"
          "if(!composed.aborted || composed.reason!=='owner-b' ||"
          " composedEvents!==1) throw Error('any propagation');"
          "var preComposed=AbortSignal.any([AbortSignal.abort('first'),"
          " AbortSignal.abort('second')]);"
          "var emptyComposed=AbortSignal.any([]);"
          "if(preComposed.reason!=='first' || emptyComposed.aborted)"
          " throw Error('any initial state');"
          "var timeoutSignal=AbortSignal.timeout(0);"
          "var timeoutEvents=0;"
          "timeoutSignal.onabort=()=>{"
          " timeoutEvents++;"
          " if(timeoutSignal.reason.name!=='TimeoutError')"
          "   throw Error('timeout reason');"
          "};"
          "fetch('/timeout-must-not-run',{signal:timeoutSignal})"
          ".catch(reason=>{"
          " document.body.setAttribute('data-abort-timeout',reason.name);"
          "});"
          "var negativeTimeout=false;"
          "try{AbortSignal.timeout(-1);}catch(e){"
          " negativeTimeout=e instanceof RangeError;"
          "}"
          "if(!negativeTimeout) throw Error('timeout range');"
          "var illegalSignal=false;"
          "try{new AbortSignal();}catch(e){illegalSignal=e instanceof TypeError;}"
          "var controllerNeedsNew=false;"
          "try{AbortController();}catch(e){"
          " controllerNeedsNew=e instanceof TypeError;"
          "}"
          "if(!illegalSignal || !controllerNeedsNew)"
          " throw Error('constructor rules');",
          "abort.js", err, sizeof(err)) == 0,
          err[0] ? err : "AbortSignal state, events, and fetch cancellation");
    CHECK(fetch_calls == 0,
          "aborted queued fetches do not reach the network host");
    CHECK(jsdom_pump(j, err, sizeof(err)) > 0,
          err[0] ? err : "aborted fetch Promise checkpoint");
    CHECK(jsdom_eval(j,
          "if(timeoutEvents!==1 || !timeoutSignal.aborted)"
          " throw Error('timeout event count');",
          "abort-after-pump.js", err, sizeof(err)) == 0,
          err[0] ? err : "timed AbortSignal dispatches exactly once");
    CHECK(dom_get_attr(doc->body, "data-abort-fetch") &&
          !strcmp(dom_get_attr(doc->body, "data-abort-fetch"), "cancelled") &&
          dom_get_attr(doc->body, "data-abort-early") &&
          !strcmp(dom_get_attr(doc->body, "data-abort-early"), "early") &&
          dom_get_attr(doc->body, "data-abort-any") &&
          !strcmp(dom_get_attr(doc->body, "data-abort-any"), "owner-b") &&
          dom_get_attr(doc->body, "data-abort-timeout") &&
          !strcmp(dom_get_attr(doc->body, "data-abort-timeout"),
                  "TimeoutError"),
          "manual, composed, and timed aborts reject with exact reasons");

    CHECK(jsdom_eval(j,
          "var arrowBase=4;"
          "var arrowAdd=(a,b)=>a+b+arrowBase;"
          "if(arrowAdd(2,3)!==9 || (()=>42)()!==42)"
          " throw Error('expression body');"
          "var arrowBlock=x=>{var y=x*2;return y+1;};"
          "if(arrowBlock(5)!==11) throw Error('block body');"
          "var arrowLexical={value:7,method:function(){"
          " var f=()=>this.value;"
          " return f.call({value:99});"
          "}};"
          "if(arrowLexical.method()!==7) throw Error('lexical this');"
          "var arrowArgs=function(x){return (()=>arguments[0])();};"
          "if(arrowArgs(11)!==11) throw Error('lexical arguments');"
          "var arrowNewFailed=false;"
          "try{new arrowAdd();}catch(e){"
          " arrowNewFailed=e instanceof TypeError;"
          "}"
          "if(!arrowNewFailed) throw Error('constructible');"
          "Promise.resolve(6).then(v=>v+1).then(v=>{"
          " document.body.setAttribute('data-arrow-promise',''+v);"
          "});",
          "arrows.js", err, sizeof(err)) == 0,
          err[0] ? err : "arrow function syntax and lexical bindings");

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
          "var apiRequest=new Request('/api/data',{method:'POST',body:'q=1',"
          " headers:{Accept:'application/json','X-Kestrel':'runtime'}});"
          "fetch(apiRequest)"
          ".then(function(response){"
          " if (!(response instanceof Response) ||"
          " !response.ok || response.status!==201 ||"
          " response.type!=='basic' || !response.redirected)"
          " throw Error('status');"
          " if (!(response.headers instanceof Headers) ||"
          " response.headers.get('content-type')!=='application/json')"
          "   throw Error('headers');"
          " return response.json();"
          "}).then(function(data){"
          " document.body.setAttribute('data-fetch',"
          "   data.message+'-'+data.count);"
          "});"
          "if(!apiRequest.bodyUsed) throw Error('fetch Request bodyUsed');"
          "if(order!=='sync') throw Error('promise ran synchronously');",
          "async.js", err, sizeof(err)) == 0,
          err[0] ? err : "register promises and fetch");
    CHECK(!dom_get_attr(doc->body, "data-chain"),
          "promise callbacks wait for checkpoint");
    CHECK(jsdom_pump(j, err, sizeof(err)) > 0,
          err[0] ? err : "promise/fetch checkpoint");
    CHECK(dom_get_attr(doc->body, "data-arrow-promise") &&
          !strcmp(dom_get_attr(doc->body, "data-arrow-promise"), "7"),
          "arrow functions compose with Promise reactions");
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
    CHECK(jsdom_eval(j,
          "if(!navigator.sendBeacon('/beacon','BEACON'))"
          " throw Error('beacon queue');"
          "if(navigator.sendBeacon('/beacon',new Uint8Array(65537)))"
          " throw Error('oversized beacon accepted');",
          "beacon.js", err, sizeof(err)) == 0,
          err[0] ? err : "navigator.sendBeacon queues bounded POST");
    CHECK(jsdom_pump(j, err, sizeof(err)) > 0,
          err[0] ? err : "sendBeacon event-loop delivery");
    CHECK(beacon_calls == 1,
          "sendBeacon delivers once and rejects oversized payload");

    CHECK(jsdom_eval(j,
          "localStorage.clear();sessionStorage.clear();"
          "if(localStorage.getItem('missing')!==null) throw Error('null');"
          "localStorage.setItem('theme','dark');"
          "localStorage.setItem('count',3);"
          "sessionStorage.setItem('draft','yes');"
          "if(localStorage.length!==2 || sessionStorage.length!==1)"
          " throw Error('length');"
          "if(localStorage.key(0)===null || localStorage.key(2)!==null)"
          " throw Error('key');"
          "localStorage.removeItem('count');"
          "if(localStorage.getItem('theme')!=='dark' ||"
          " localStorage.getItem('count')!==null) throw Error('items');",
          "storage.js", err, sizeof(err)) == 0,
          err[0] ? err : "Web Storage operations");
    CHECK(web_storage_length(local_store, "https://example.test") == 1 &&
          !strcmp(web_storage_get(local_store, "https://example.test",
                                  "theme"), "dark"),
          "localStorage is origin partitioned");
    CHECK(web_storage_length(session_store, "https://example.test") == 1,
          "sessionStorage uses runtime service");
    storage_blob = web_storage_export(local_store, &storage_blob_len);
    roundtrip_store = web_storage_new(4096, 64);
    CHECK(storage_blob != 0 && roundtrip_store != 0 &&
          web_storage_import(roundtrip_store, storage_blob,
                             storage_blob_len) == WEB_STORAGE_OK &&
          !strcmp(web_storage_get(roundtrip_store, "https://example.test",
                                  "theme"), "dark"),
          "storage persistence round trip");
    CHECK(web_storage_import(roundtrip_store, "broken", 6) ==
              WEB_STORAGE_BAD_DATA &&
          !strcmp(web_storage_get(roundtrip_store, "https://example.test",
                                  "theme"), "dark"),
          "malformed storage import is transactional");
    free(storage_blob);
    web_storage_free(roundtrip_store);

    jsdom_free(j);
    dom_document_free(doc);

    doc = html_parse_document("<html><body></body></html>", 26);
    cfg.url = "https://example.test/other";
    cfg.base_url = 0;
    j = jsdom_new(doc, &cfg);
    CHECK(j != 0 && jsdom_eval(j,
          "if(localStorage.getItem('theme')!=='dark' ||"
          " sessionStorage.getItem('draft')!=='yes')"
          " throw Error('runtime persistence');",
          "storage-same-origin.js", err, sizeof(err)) == 0,
          err[0] ? err : "storage survives same-runtime navigation");
    jsdom_free(j);
    dom_document_free(doc);

    doc = html_parse_document("<html><body></body></html>", 26);
    cfg.url = "https://other.test/";
    j = jsdom_new(doc, &cfg);
    CHECK(j != 0 && jsdom_eval(j,
          "if(localStorage.length!==0 || sessionStorage.length!==0 ||"
          " localStorage.getItem('theme')!==null)"
          " throw Error('origin isolation');",
          "storage-other-origin.js", err, sizeof(err)) == 0,
          err[0] ? err : "storage isolates origins");
    jsdom_free(j);
    dom_document_free(doc);
    web_storage_free(session_store);
    web_storage_free(local_store);
    printf("%d passed, %d failed\n", checks - failures, failures);
    return failures ? 1 : 0;
}
