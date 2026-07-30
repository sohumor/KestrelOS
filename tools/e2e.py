#!/usr/bin/env python3
"""KestrelOS end-to-end test harness.

Boots build/os.img in headless QEMU, logs in on the serial console and
drives the shell over serial (stdio). Python 3 stdlib only.

The boot now ends at /bin/login, so the suite signs in as root before it
can do anything else; a build with /etc/autologin lands straight on a
shell and is handled too. See t_login.

Writing a test: every expect() pattern must be unique to the command's
OUTPUT. The shell echoes the line you typed and then prints its prompt,
so a pattern that can match either of those consumes the wrong text and
desynchronises everything that follows. Anchoring on a leading "\\n", on
punctuation the command line does not contain, or on a word only the
program prints are the three tricks used throughout.

Usage:
    python3 tools/e2e.py            # full test sequence
    python3 tools/e2e.py --smoke    # boot + prompt only
    python3 tools/e2e.py --nic e1000
    python3 tools/e2e.py --list     # list tests without running
    python3 tools/e2e.py --selftest # exercise expect/send plumbing
                                    # against a fake child (no image)
"""

import atexit
import argparse
import base64
import collections
import http.client
import http.server
import os
import re
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time

QEMU_BASE_CMD = [
    "qemu-system-x86_64",
    "-drive", "file=build/os.img,format=raw",
    # Package/filesystem tests deliberately mutate the guest disk. Keep those
    # writes in a temporary overlay so repeated runs always start from the
    # image produced by make and never contaminate build/os.img.
    "-snapshot",
    "-no-reboot",
    "-display", "none",
    "-serial", "stdio",
]

QEMU_NICS = {
    "rtl8139": "rtl8139",
    "e1000": "e1000",
}


def qemu_command(nic, cpus):
    return QEMU_BASE_CMD + [
        "-m", "128M",
        "-smp", str(cpus),
        "-device", "%s,netdev=n0" % QEMU_NICS[nic],
        "-netdev", "user,id=n0",
    ]


DEFAULT_TIMEOUT = 20
BOOT_TIMEOUT = 30
# Recursive walkers (tree/du/find) must finish well inside this; a
# runaway recursion shows up as a timeout instead of a wedged harness.
WALK_TIMEOUT = 10
# kpkg hashes every file it installs or verifies with a userspace SHA-256,
# which is slow enough on an emulated machine to need its own budget.
PKG_TIMEOUT = 90
# The network tests all SKIP on failure, but a DNS lookup plus a TCP
# handshake to the outside world still has to be given time to fail.
NET_TIMEOUT = 45
TAIL_LINES = 40

# QEMU user networking exposes the host-side slirp endpoint here.  Browser
# transport tests bind ephemeral host ports and reach them through this IP.
QEMU_HOST = "10.0.2.2"
HTTP_BODY_MARKER = "CONTROLLED-HTTP-BODY-OK"
TLS_NEGATIVE_BODY_MARKER = "TLS-NEGATIVE-BODY-MUST-NOT-APPEAR"
TLS_NEGATIVE_REPETITIONS = 10

TLS_NEGATIVE_EXIT_RE = re.compile(r"(?m)^\[exit (-?\d+)\]$")
TLS_NEGATIVE_STATUS_RE = re.compile(
    r"(?m)^BROWSER-TLS-NEG-STATUS-(-?\d+)$")
TLS_NEGATIVE_DIAGNOSTIC_RE = re.compile(
    r"(?i)(certificate|host ?name|trusted root|self[- ]signed|"
    r"does not match)")
TLS_NEGATIVE_REASON_RE = re.compile(
    r"(?i)(kestrel-negative\.invalid|trusted root|self[- ]signed|"
    r"untrusted|not trusted|unknown issuer|"
    r"certificate chain[^\n]{0,48}(?:trust|root|issuer))")
TLS_NEGATIVE_FORBIDDEN_RE = re.compile(
    r"(?i)(uproc:|\bfault\b|\bexception\b|kernel[ -]+panic|"
    r"\bpanic\b|\[exit -1\]|BROWSER-TLS-NEG-STATUS--1|"
    r"\bno error\b)")

ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[@-_]")
# An escape sequence the current chunk ends in the middle of. QEMU hands us
# whatever has arrived, so a repaint's "\x1b[20C" is regularly split across
# two reads; stripping each half separately would leave "20C" sitting in
# the buffer where a pattern could trip over it.
ANSI_TAIL_RE = re.compile(r"\x1b(\[[0-9;?]*[ -/]*)?$")


def clean(text):
    """Strip ANSI escape sequences and carriage returns."""
    return ANSI_RE.sub("", text).replace("\r", "")


class Harness:
    def __init__(self, cmd):
        self.cmd = cmd
        self.proc = None
        self.buf = ""                 # cleaned, unconsumed text
        self.lock = threading.Lock()
        self.cond = threading.Condition(self.lock)
        self.tail = collections.deque(maxlen=TAIL_LINES)
        self._partial = ""
        self._esc = ""                # half of an escape sequence, held back
        self.eof = False

    def start(self):
        self.proc = subprocess.Popen(
            self.cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        atexit.register(self.kill)
        t = threading.Thread(target=self._reader, daemon=True)
        t.start()

    def _reader(self):
        while True:
            # read1 returns as soon as any data is available; a plain
            # buffered read(4096) would block until 4096 bytes or EOF

            chunk = self.proc.stdout.read1(4096)
            if not chunk:
                break
            raw = self._esc + chunk.decode("utf-8", errors="replace")
            m = ANSI_TAIL_RE.search(raw)
            if m:
                self._esc = raw[m.start():]
                raw = raw[:m.start()]
            else:
                self._esc = ""
            text = clean(raw)
            if not text:
                continue
            with self.cond:
                self.buf += text
                self._partial += text
                lines = self._partial.split("\n")
                self._partial = lines.pop()
                for line in lines:
                    self.tail.append(line)
                self.cond.notify_all()
        with self.cond:
            self._esc = ""            # nothing will ever complete it now
            if self._partial:
                self.tail.append(self._partial)
                self._partial = ""
            self.eof = True
            self.cond.notify_all()

    def expect(self, pattern, timeout=DEFAULT_TIMEOUT, regex=False):
        """Wait until pattern appears in output; consume through match.

        pattern: literal substring, or regex string if regex=True.
        Returns the re.Match (regex) or the matched string. Raises
        TimeoutError on timeout/EOF.
        """
        rx = re.compile(pattern if regex else re.escape(pattern))
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                m = rx.search(self.buf)
                if m:
                    self.buf = self.buf[m.end():]
                    return m if regex else m.group(0)
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self.eof:
                    raise TimeoutError(
                        "timeout waiting for %r (eof=%s)" % (pattern, self.eof))
                self.cond.wait(min(remaining, 0.5))

    def expect_any(self, patterns, timeout=DEFAULT_TIMEOUT, regex=False):
        """Wait for any of several patterns; returns the matching pattern."""
        rxs = [(p, re.compile(p if regex else re.escape(p)))
               for p in patterns]
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                best = None
                for p, rx in rxs:
                    m = rx.search(self.buf)
                    if m and (best is None or m.start() < best[1].start()):
                        best = (p, m)
                if best:
                    self.buf = self.buf[best[1].end():]
                    return best[0]
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self.eof:
                    raise TimeoutError(
                        "timeout waiting for any of %r (eof=%s)"
                        % (patterns, self.eof))
                self.cond.wait(min(remaining, 0.5))

    def capture_until(self, pattern, timeout=DEFAULT_TIMEOUT, regex=False):
        """Consume through pattern and return the complete consumed block."""
        rx = re.compile(pattern if regex else re.escape(pattern))
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                m = rx.search(self.buf)
                if m:
                    block = self.buf[:m.end()]
                    self.buf = self.buf[m.end():]
                    return block
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self.eof:
                    raise TimeoutError(
                        "timeout capturing through %r (eof=%s)"
                        % (pattern, self.eof))
                self.cond.wait(min(remaining, 0.5))

    def send(self, line):
        self.send_raw(line + "\n")

    def send_raw(self, data):
        self.proc.stdin.write(data.encode("utf-8"))
        self.proc.stdin.flush()

    def print_tail(self):
        print("---- last %d lines of serial output ----" % TAIL_LINES)
        with self.lock:
            for line in list(self.tail):
                print(line)
            if self._partial:
                print(self._partial)
        print("---- end of tail ----")

    def kill(self):
        if self.proc is None or self.proc.poll() is not None:
            return
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass


class _QuietHTTPServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, *args, **kwargs):
        self._get_count = 0
        self._path_counts = {}
        self._get_count_lock = threading.Lock()
        super().__init__(*args, **kwargs)

    def record_get(self, path="/"):
        with self._get_count_lock:
            self._get_count += 1
            self._path_counts[path] = self._path_counts.get(path, 0) + 1

    @property
    def get_count(self):
        with self._get_count_lock:
            return self._get_count

    def path_count(self, path):
        with self._get_count_lock:
            return self._path_counts.get(path, 0)

    def handle_error(self, request, client_address):
        # The certificate-negative client deliberately aborts during the
        # handshake.  That expected TLS close is not a harness failure.
        del request, client_address


def _marker_handler(marker):
    body = (
        "<!doctype html><html><head><title>Kestrel controlled fixture</title>"
        "</head><body><h1>%s</h1></body></html>" % marker
    ).encode("ascii")

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            self.server.record_get(path)
            if path == "/api/redirect":
                self.send_response(302)
                self.send_header("Location", "/api/redirected")
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return
            content_type = "text/html; charset=us-ascii"
            response = body
            if path == "/compat":
                response = (
                    "<!doctype html><html><head>"
                    "<title>Kestrel compatibility fixture</title>"
                    "<link rel=stylesheet href=/compat.css>"
                    "<script src=/compat.js></script>"
                    "<script type=module src=/main.mjs></script>"
                    "</head><body><h1>WEB-COMPAT-START</h1>"
                    "<p id=css-proof style='display:none'>CSS-EXTERNAL-OK</p>"
                    "<p id=import-proof style='display:none'>CSS-IMPORT-OK</p>"
                    "<p id=message>JS-NOT-RUN</p>"
                    "<img src=/pixel.png alt=IMAGE-RESOURCE-OK>"
                    "<script>var x=document.createElement('strong');"
                    "x.textContent=' INLINE-JS-OK';"
                    "document.body.appendChild(x);</script>"
                    "<script type=module>"
                    "const inlineMeta=document.createElement('p');"
                    "inlineMeta.textContent=import.meta.url.indexOf("
                    "'/compat')>=0?'INLINE-META-OK':'INLINE-META-BAD';"
                    "document.body.appendChild(inlineMeta);"
                    "</script>"
                    "</body></html>"
                ).encode("ascii")
            elif path == "/module-policy":
                blocked = getattr(self.server, "blocked_module_url", "")
                response = (
                    "<!doctype html><html><head>"
                    "<title>Module policy fixture</title>"
                    "<script type=module src=/bad-mime.mjs></script>"
                    "<script type=module src='%s'></script>"
                    "</head><body><p>MODULE-POLICY-PAGE</p></body></html>"
                    % blocked
                ).encode("ascii")
            elif path == "/compat.css":
                content_type = "text/css"
                response = (
                    "@import url('/import.css');"
                    "#css-proof{display:block!important;color:#123456}"
                ).encode("ascii")
            elif path == "/import.css":
                content_type = "text/css"
                response = b"#import-proof{display:block!important}"
            elif path == "/compat.js":
                content_type = "text/javascript"
                response = (
                    "var p=document.querySelector('body #message');"
                    "p.textContent='JS-DOM-OK';"
                    "p.classList.add('executed');"
                    "p.style.fontWeight='bold';"
                    "document.cookie='clientcookie=client; Path=/';"
                    "var ck=document.createElement('p');"
                    "ck.textContent=document.cookie.indexOf("
                    "'clientcookie=client')>=0 && document.cookie.indexOf("
                    "'servercookie=server')<0?'COOKIE-DOM-OK':"
                    "'COOKIE-DOM-BAD';"
                    "document.body.appendChild(ck);"
                    "var cs=document.createElement('p');"
                    "cs.textContent=getComputedStyle("
                    "document.getElementById('css-proof')).display==='block'"
                    "?'COMPUTED-STYLE-OK':'COMPUTED-STYLE-BAD';"
                    "document.body.appendChild(cs);"
                    "var modern=document.createElement('p');"
                    "var query=new URLSearchParams({q:'hello world',n:2});"
                    "var text=new TextDecoder().decode("
                    "new TextEncoder().encode('Kestrel'));"
                    "modern.textContent=text==='Kestrel' && "
                    "query.toString()==='q=hello+world&n=2'"
                    "?'TEXT-URL-API-OK':'TEXT-URL-API-BAD';"
                    "document.body.appendChild(modern);"
                    "var runtime=document.createElement('p');"
                    "var parsed=new URL('../v1?x=1',location.href);"
                    "parsed.searchParams.set('x','2');"
                    "var aborter=new AbortController();"
                    "var alternate=new AbortController();"
                    "var abortEvents=0;"
                    "var combined=AbortSignal.any([aborter.signal,"
                    "alternate.signal]);"
                    "combined.addEventListener('abort',()=>abortEvents++);"
                    "aborter.abort('fixture-stop');"
                    "runtime.textContent=parsed.pathname==='/v1' && "
                    "parsed.search==='?x=2' && combined.aborted && "
                    "combined.reason==='fixture-stop' && abortEvents===1"
                    "?'URL-ABORT-API-OK':'URL-ABORT-API-BAD';"
                    "document.body.appendChild(runtime);"
                    "var deadline=AbortSignal.timeout(0);"
                    "deadline.onabort=function(){"
                    "var timed=document.createElement('p');"
                    "timed.textContent=deadline.reason.name==='TimeoutError'"
                    "?'ABORT-TIMEOUT-OK':'ABORT-TIMEOUT-BAD';"
                    "document.body.appendChild(timed);"
                    "};"
                    "var eventProof=document.createElement('p');"
                    "var eventTarget=new EventTarget();"
                    "var eventDetail=0;"
                    "eventTarget.addEventListener('proof',function(event){"
                    "eventDetail=event.detail.value;event.preventDefault();"
                    "});"
                    "var customEvent=new CustomEvent('proof',{"
                    "detail:{value:7},cancelable:true});"
                    "eventProof.textContent="
                    "eventTarget.dispatchEvent(customEvent)===false && "
                    "eventDetail===7 && customEvent.defaultPrevented && "
                    "customEvent instanceof Event"
                    "?'EVENT-API-OK':'EVENT-API-BAD';"
                    "document.body.appendChild(eventProof);"
                    "var eventOptionsProof=document.createElement('p');"
                    "var optionTarget=new EventTarget();"
                    "var optionController=new AbortController();"
                    "var onceOptionCalls=0;"
                    "var objectOptionCalls=0;"
                    "var optionPathLength=0;"
                    "optionTarget.addEventListener('once-option',function(){"
                    "onceOptionCalls++;},{once:true});"
                    "optionTarget.dispatchEvent(new Event('once-option'));"
                    "optionTarget.dispatchEvent(new Event('once-option'));"
                    "var passiveOptionEvent=new Event('passive-option',"
                    "{cancelable:true});"
                    "optionTarget.addEventListener('passive-option',"
                    "function(e){e.preventDefault();},{passive:true});"
                    "optionTarget.dispatchEvent(passiveOptionEvent);"
                    "optionTarget.addEventListener('signal-option',function(){"
                    "objectOptionCalls+=100;},{signal:optionController.signal});"
                    "optionTarget.addEventListener('object-option',{"
                    "handleEvent:function(e){objectOptionCalls++;"
                    "optionPathLength=e.composedPath().length;"
                    "if(e.srcElement!==optionTarget)objectOptionCalls+=100;}});"
                    "optionController.abort();"
                    "optionTarget.dispatchEvent(new Event('signal-option'));"
                    "var objectOptionEvent=new Event('object-option');"
                    "optionTarget.dispatchEvent(objectOptionEvent);"
                    "var keyboardOptionEvent=new KeyboardEvent('keydown',{"
                    "key:'K',code:'KeyK',ctrlKey:true,location:1});"
                    "var pointerOptionEvent=new PointerEvent('pointerdown',{"
                    "pointerId:9,pointerType:'pen',pressure:0.5,"
                    "clientX:4,clientY:5,isPrimary:true});"
                    "eventOptionsProof.textContent="
                    "onceOptionCalls===1 && objectOptionCalls===1 && "
                    "optionPathLength===1 && "
                    "objectOptionEvent.composedPath().length===0 && "
                    "!passiveOptionEvent.defaultPrevented && "
                    "Event.CAPTURING_PHASE===1 && Event.AT_TARGET===2 && "
                    "Event.BUBBLING_PHASE===3 && "
                    "keyboardOptionEvent instanceof UIEvent && "
                    "keyboardOptionEvent.key==='K' && "
                    "keyboardOptionEvent.getModifierState('Control') && "
                    "pointerOptionEvent instanceof MouseEvent && "
                    "pointerOptionEvent.pointerId===9 && "
                    "pointerOptionEvent.pointerType==='pen' && "
                    "pointerOptionEvent.pressure===0.5"
                    "?'EVENT-OPTIONS-OK':'EVENT-OPTIONS-BAD';"
                    "document.body.appendChild(eventOptionsProof);"
                    "var headerProof=document.createElement('p');"
                    "var headerSet=new Headers({'X-Kestrel':' one '});"
                    "headerSet.append('x-kestrel','two');"
                    "var headerClone=new Headers(headerSet);"
                    "headerSet.set('x-kestrel','changed');"
                    "headerProof.textContent="
                    "headerClone.get('X-KESTREL')==='one, two'"
                    "?'HEADERS-API-OK':'HEADERS-API-BAD';"
                    "document.body.appendChild(headerProof);"
                    "var iteratorProof=document.createElement('p');"
                    "var headerEntry=headerClone.entries().next().value;"
                    "var iteratorForm=new FormData();"
                    "iteratorForm.append('field','value');"
                    "var formEntry=iteratorForm.entries().next().value;"
                    "iteratorProof.textContent="
                    "headerEntry.join('=')==='x-kestrel=one, two' && "
                    "formEntry.join('=')==='field=value'"
                    "?'COLLECTION-ITERATORS-OK':"
                    "'COLLECTION-ITERATORS-BAD';"
                    "document.body.appendChild(iteratorProof);"
                    "var entropyA=new Uint8Array(16);"
                    "var entropyB=new Uint8Array(16);"
                    "crypto.getRandomValues(entropyA);"
                    "crypto.getRandomValues(entropyB);"
                    "var entropyDiffers=false;"
                    "for(var entropyIndex=0;entropyIndex<16;entropyIndex++){"
                    "if(entropyA[entropyIndex]!==entropyB[entropyIndex])"
                    "entropyDiffers=true;"
                    "}"
                    "var randomUuid=crypto.randomUUID();"
                    "var cryptoProof=document.createElement('p');"
                    "cryptoProof.textContent=entropyDiffers && "
                    "randomUuid.length===36 && randomUuid[14]==='4' && "
                    "'89ab'.indexOf(randomUuid[19])>=0"
                    "?'CRYPTO-RANDOM-OK':'CRYPTO-RANDOM-BAD';"
                    "document.body.appendChild(cryptoProof);"
                    "Promise.all(['SHA-1','SHA-256','SHA-384','SHA-512']"
                    ".map(function(name){return crypto.subtle.digest(name,"
                    "new TextEncoder().encode('abc'));}))"
                    ".then(function(buffers){"
                    "var sha1Bytes=new Uint8Array(buffers[0]);"
                    "var digestBytes=new Uint8Array(buffers[1]);"
                    "var sha384Bytes=new Uint8Array(buffers[2]);"
                    "var sha512Bytes=new Uint8Array(buffers[3]);"
                    "var digestProof=document.createElement('p');"
                    "digestProof.textContent="
                    "sha1Bytes.length===20 && sha1Bytes[0]===169 && "
                    "sha1Bytes[19]===157 && digestBytes.length===32 && "
                    "digestBytes[0]===186 && digestBytes[1]===120 && "
                    "digestBytes[2]===22 && digestBytes[3]===191 && "
                    "digestBytes[31]===173 && sha384Bytes.length===48 && "
                    "sha512Bytes.length===64 && sha512Bytes[0]===221 && "
                    "sha512Bytes[63]===159"
                    "?'CRYPTO-DIGEST-OK':'CRYPTO-DIGEST-BAD';"
                    "document.body.appendChild(digestProof);"
                    "});"
                    "var madeResponse=Response.json({ready:true},{"
                    "status:202,headers:{'X-Response':'made'}});"
                    "var responseShape=madeResponse instanceof Response && "
                    "madeResponse.status===202 && "
                    "madeResponse.type==='default' && "
                    "madeResponse.headers.get('x-response')==='made' && "
                    "madeResponse.headers.get('content-type')==="
                    "'application/json' && "
                    "Response.redirect('/moved',303).headers.get('location')"
                    ".indexOf('/moved')>0 && "
                    "Response.error().type==='error' && "
                    "Response.error().status===0;"
                    "madeResponse.clone().json().then(function(data){"
                    "var responseProof=document.createElement('p');"
                    "responseProof.textContent=responseShape && data.ready"
                    "?'RESPONSE-API-OK':'RESPONSE-API-BAD';"
                    "document.body.appendChild(responseProof);"
                    "});"
                    "var blobProofSource=new Blob(['B',"
                    "new Uint8Array([76,79,66])],{type:'Text/Plain'});"
                    "blobProofSource.slice(1).text().then(function(text){"
                    "var blobProof=document.createElement('p');"
                    "blobProof.textContent=blobProofSource.size===4 && "
                    "blobProofSource.type==='text/plain' && text==='LOB'"
                    "?'BLOB-API-OK':'BLOB-API-BAD';"
                    "document.body.appendChild(blobProof);"
                    "});"
                    "var fileProofSource=new File(['FILE'],'proof.txt',{"
                    "type:'Text/Plain',lastModified:4242});"
                    "var fileProof=document.createElement('p');"
                    "fileProof.textContent=fileProofSource instanceof Blob && "
                    "fileProofSource.name==='proof.txt' && "
                    "fileProofSource.type==='text/plain' && "
                    "fileProofSource.lastModified===4242"
                    "?'FILE-API-OK':'FILE-API-BAD';"
                    "document.body.appendChild(fileProof);"
                    "var browserReader=new FileReader();"
                    "var browserReaderEvents='';"
                    "browserReader.addEventListener('loadstart',function(){"
                    "browserReaderEvents+='start;';});"
                    "browserReader.onprogress=function(event){"
                    "if(event.loaded===1 && event.total===1)"
                    "browserReaderEvents+='progress;';};"
                    "browserReader.onload=function(){"
                    "browserReaderEvents+='load;';};"
                    "browserReader.onloadend=function(){"
                    "browserReaderEvents+='end;';"
                    "var readerNode=document.createElement('p');"
                    "readerNode.textContent="
                    "browserReader.readyState===FileReader.DONE && "
                    "browserReader.result==='data:text/plain;base64,Ug==' && "
                    "browserReaderEvents==='start;progress;load;end;'"
                    "?'FILE-READER-OK':'FILE-READER-BAD';"
                    "document.body.appendChild(readerNode);"
                    "};"
                    "browserReader.readAsDataURL("
                    "new File(['R'],'r.txt',{type:'text/plain'}));"
                    "var previewUrl=URL.createObjectURL("
                    "new Blob(['PREVIEW'],{type:'text/custom'}));"
                    "fetch(previewUrl).then(function(response){"
                    "if(response.url!==previewUrl || "
                    "response.headers.get('content-type')!=='text/custom')"
                    "throw Error('object URL metadata');"
                    "return response.text();"
                    "}).then(function(text){"
                    "URL.revokeObjectURL(previewUrl);"
                    "return fetch(previewUrl).then(function(){"
                    "throw Error('revoked object URL loaded');"
                    "},function(){return text;});"
                    "}).then(function(text){"
                    "var objectUrlProof=document.createElement('p');"
                    "objectUrlProof.textContent=text==='PREVIEW'"
                    "?'OBJECT-URL-OK':'OBJECT-URL-BAD';"
                    "document.body.appendChild(objectUrlProof);"
                    "});"
                    "Promise.all(["
                    "new Response(new Uint8Array([66,89,84,69]),{"
                    "headers:{'Content-Type':'Text/Custom'}}).blob()"
                    ".then(function(blob){return blob.text().then("
                    "function(text){return blob.type+'-'+text;});}),"
                    "new Request('/reader',{method:'POST',"
                    "body:new Uint8Array([3,4,5])}).bytes()"
                    "]).then(function(values){"
                    "var readerProof=document.createElement('p');"
                    "readerProof.textContent="
                    "values[0]==='text/custom-BYTE' && "
                    "values[1].length===3 && values[1][2]===5"
                    "?'BODY-READERS-OK':'BODY-READERS-BAD';"
                    "document.body.appendChild(readerProof);"
                    "});"
                    "var readForm=new FormData();"
                    "readForm.append('field','multipart value');"
                    "readForm.append('upload',new File(['UPLOAD'],"
                    "'read.txt',{type:'text/plain'}));"
                    "Promise.all(["
                    "new Response('q=hello+world&q=again',{headers:{"
                    "'Content-Type':"
                    "'application/x-www-form-urlencoded;charset=UTF-8'}})"
                    ".formData(),"
                    "new Request('/form-reader',{method:'POST',body:readForm})"
                    ".formData()"
                    "]).then(function(forms){"
                    "var readFile=forms[1].get('upload');"
                    "return readFile.text().then(function(fileText){"
                    "var formReaderProof=document.createElement('p');"
                    "formReaderProof.textContent="
                    "forms[0].getAll('q').join('|')==='hello world|again' && "
                    "forms[1].get('field')==='multipart value' && "
                    "readFile instanceof File && "
                    "readFile.name==='read.txt' && fileText==='UPLOAD'"
                    "?'BODY-FORMDATA-OK':'BODY-FORMDATA-BAD';"
                    "document.body.appendChild(formReaderProof);"
                    "});"
                    "});"
                    "console.log('COMPAT-CONSOLE-OK');"
                ).encode("ascii")
            elif path == "/main.mjs":
                content_type = "text/javascript"
                cors_import = (
                    "import {corsMarker} from '%s';\n"
                    % getattr(self.server, "cors_module_url", ""))
                policy_url = (
                    "const policyUrl='%s';\n"
                    % getattr(self.server, "cors_module_url", ""))
                response = (
                    "import base, {suffix as ending} from './dep.mjs';"
                    "const minifiedTail='MINIFIED-TAIL-OK';\n"
                    + cors_import + policy_url +
                    "/*\n"
                    "import('./commented-out.mjs');\n"
                    "*/\n"
                    "const marker=base+'-IMPORT-'+ending;\n"
                    "const node=document.createElement('p');\n"
                    "node.textContent=marker+' '+minifiedTail;\n"
                    "document.body.appendChild(node);\n"
                    "const corsNode=document.createElement('p');\n"
                    "corsNode.textContent=corsMarker;\n"
                    "document.body.appendChild(corsNode);\n"
                    "const holder={};\n"
                    "holder.import=function(){return 'MEMBER-IMPORT-OK';};\n"
                    "const memberNode=document.createElement('p');\n"
                    "memberNode.textContent=holder.import();\n"
                    "document.body.appendChild(memberNode);\n"
                    "const navigatorNode=document.createElement('p');\n"
                    "navigatorNode.textContent="
                    "navigator.appCodeName==='Mozilla' && "
                    "navigator.product==='Gecko' && "
                    "navigator.platform.indexOf('KestrelOS')>=0 && "
                    "navigator.languages.join(',')==='en-US,en' && "
                    "navigator.hardwareConcurrency>=1 && "
                    "navigator.onLine && !navigator.webdriver"
                    "?'NAVIGATOR-API-OK':'NAVIGATOR-API-BAD';\n"
                    "document.body.appendChild(navigatorNode);\n"
                    "const screenQuery=matchMedia('screen and (min-width: 1px)');\n"
                    "const exactQuery=matchMedia('(width: '+innerWidth+'px)');\n"
                    "const mediaNode=document.createElement('p');\n"
                    "mediaNode.textContent="
                    "screenQuery.matches && exactQuery.matches && "
                    "!matchMedia('print').matches && "
                    "screen.width===innerWidth && screen.height===innerHeight && "
                    "devicePixelRatio===1 && "
                    "typeof screenQuery.addListener==='function'"
                    "?'MATCH-MEDIA-OK':'MATCH-MEDIA-BAD';\n"
                    "document.body.appendChild(mediaNode);\n"
                    "const parsedLocation=new URL(location.href);\n"
                    "const locationNode=document.createElement('p');\n"
                    "locationNode.textContent="
                    "location.origin===parsedLocation.origin && "
                    "location.protocol===parsedLocation.protocol && "
                    "location.host===parsedLocation.host && "
                    "location.hostname===parsedLocation.hostname && "
                    "location.port===parsedLocation.port && "
                    "location.pathname==='/compat' && "
                    "location.toString()===location.href && "
                    "document.URL===location.href && "
                    "document.documentURI===location.href && "
                    "document.baseURI===location.href && "
                    "document.characterSet==='UTF-8' && "
                    "document.compatMode==='CSS1Compat' && "
                    "document.defaultView===window"
                    "?'LOCATION-API-OK':'LOCATION-API-BAD';\n"
                    "document.body.appendChild(locationNode);\n"
                    "let popstateCount=0;\n"
                    "let popstateSteps='';\n"
                    "addEventListener('popstate',function(event){"
                    "popstateCount++;popstateSteps+=event.state.step;});\n"
                    "history.pushState({step:1},'', '#route-one');\n"
                    "history.replaceState({step:2},'', '#route-two');\n"
                    "history.pushState({step:3},'', '#route-three');\n"
                    "history.back();\n"
                    "history.forward();\n"
                    "const historyNode=document.createElement('p');\n"
                    "historyNode.textContent="
                    "history.length===3 && history.state.step===3 && "
                    "location.hash==='#route-three' && "
                    "popstateCount===2 && popstateSteps==='23'"
                    "?'HISTORY-API-OK':'HISTORY-API-BAD';\n"
                    "document.body.appendChild(historyNode);\n"
                    "const modernRoot=document.createElement('div');\n"
                    "modernRoot.id='modern-root';\n"
                    "modernRoot.className='panel';\n"
                    "modernRoot.classList.add('ready','interactive');\n"
                    "modernRoot.classList.replace('ready','mounted');\n"
                    "modernRoot.style.setProperty('border-color','#123456');\n"
                    "modernRoot.style.setProperty('--accent','teal','important');\n"
                    "const styleNode=document.createElement('p');\n"
                    "styleNode.textContent="
                    "modernRoot.style.getPropertyValue('border-color')==="
                    "'#123456' && "
                    "modernRoot.style.getPropertyValue('--accent')==='teal' && "
                    "modernRoot.style.getPropertyPriority('--accent')==="
                    "'important' && modernRoot.style.length===2"
                    "?'STYLE-DECLARATION-OK':'STYLE-DECLARATION-BAD';\n"
                    "document.body.appendChild(styleNode);\n"
                    "const compatMap=new Map([['a',1],['b',2],['a',3]]);\n"
                    "compatMap.set(NaN,'nan');\n"
                    "const compatSet=new Set(['x','x','y']);\n"
                    "const mapEntry=compatMap.entries().next().value;\n"
                    "let mapForEach='';\n"
                    "compatMap.forEach(function(value,key){"
                    "if(key!=='a' && key!=='b' && key===key)"
                    "return;mapForEach+=String(key)+String(value);});\n"
                    "const collectionNode=document.createElement('p');\n"
                    "collectionNode.textContent="
                    "compatMap.size===3 && compatMap.get('a')===3 && "
                    "compatMap.get(NaN)==='nan' && "
                    "mapEntry[0]==='a' && mapEntry[1]===3 && "
                    "mapForEach.indexOf('a3')===0 && "
                    "compatSet.size===2 && compatSet.has('y')"
                    "?'MAP-SET-OK':'MAP-SET-BAD';\n"
                    "document.body.appendChild(collectionNode);\n"
                    "const weakKey={},weakOther={};\n"
                    "const compatWeakMap=new WeakMap([[weakKey,'value']]);\n"
                    "const compatWeakSet=new WeakSet([weakKey]);\n"
                    "compatWeakMap.set(weakOther,42);\n"
                    "compatWeakSet.add(weakOther);\n"
                    "const weakNode=document.createElement('p');\n"
                    "weakNode.textContent="
                    "compatWeakMap.get(weakKey)==='value' && "
                    "compatWeakMap.get(weakOther)===42 && "
                    "compatWeakSet.has(weakKey) && "
                    "compatWeakSet.delete(weakOther) && "
                    "typeof compatWeakMap.keys==='undefined' && "
                    "typeof compatWeakSet.size==='undefined'"
                    "?'WEAK-COLLECTIONS-OK':'WEAK-COLLECTIONS-BAD';\n"
                    "document.body.appendChild(weakNode);\n"
                    "const staticTarget={base:1};\n"
                    "Object.assign(staticTarget,{extra:2});\n"
                    "const staticEntries=Object.entries(staticTarget);\n"
                    "const staticArray=Array.from({0:3,1:4,length:2},"
                    "function(value,index){return value+index;});\n"
                    "const staticNode=document.createElement('p');\n"
                    "staticNode.textContent="
                    "staticTarget.extra===2 && staticEntries.length===2 && "
                    "staticArray.join(',')==='3,5' && "
                    "Array.of(7).length===1 && Object.is(NaN,NaN) && "
                    "!Object.is(0,-0) && Number.isFinite(2) && "
                    "!Number.isFinite('2') && Number.isSafeInteger(42)"
                    "?'ES-STATIC-BUILTINS-OK':'ES-STATIC-BUILTINS-BAD';\n"
                    "document.body.appendChild(staticNode);\n"
                    "const prototypeNode=document.createElement('p');\n"
                    "prototypeNode.textContent="
                    "[1,NaN,3].includes(NaN) && "
                    "[1,4,7].find(function(value){return value>3;})===4 && "
                    "[1,4,7].findIndex(function(value){return value>4;})===2 && "
                    "'kestrel-browser'.includes('browser') && "
                    "'kestrel'.startsWith('est',1) && "
                    "'kestrel'.endsWith('str',5) && "
                    "'ab'.repeat(2)==='abab' && "
                    "'7'.padStart(3,'0')==='007' && "
                    "'x'.padEnd(4,'ab')==='xaba'"
                    "?'ES-PROTOTYPE-METHODS-OK':"
                    "'ES-PROTOTYPE-METHODS-BAD';\n"
                    "document.body.appendChild(prototypeNode);\n"
                    "const modernMethodNode=document.createElement('p');\n"
                    "modernMethodNode.textContent="
                    "[1,[2,[3]]].flat(2).join(',')==='1,2,3' && "
                    "[1,2].flatMap(function(value){"
                    "return [value,value*2];}).join(',')==='1,2,2,4' && "
                    "'a-b-a'.replaceAll('a','x')==='x-b-x' && "
                    "'  kestrel'.trimStart()==='kestrel' && "
                    "'kestrel  '.trimEnd()==='kestrel'"
                    "?'ES-MODERN-METHODS-OK':'ES-MODERN-METHODS-BAD';\n"
                    "document.body.appendChild(modernMethodNode);\n"
                    "const iteratorObject=Object.fromEntries("
                    "new Map([['left',1],['right',2]]).entries());\n"
                    "const arrayIterator=['a','b'].entries();\n"
                    "const firstArrayEntry=arrayIterator.next().value;\n"
                    "const iteratorNode=document.createElement('p');\n"
                    "iteratorNode.textContent="
                    "iteratorObject.left===1 && iteratorObject.right===2 && "
                    "firstArrayEntry[0]===0 && firstArrayEntry[1]==='a' && "
                    "arrayIterator.next().value[1]==='b' && "
                    "arrayIterator.next().done"
                    "?'EXPLICIT-ITERATORS-OK':'EXPLICIT-ITERATORS-BAD';\n"
                    "document.body.appendChild(iteratorNode);\n"
                    "const modernChild=document.createElement('span');\n"
                    "modernChild.className='item';\n"
                    "modernChild.setAttribute('data-user-id','7');\n"
                    "const liveDataset=modernChild.dataset;\n"
                    "liveDataset.userId='8';\n"
                    "modernChild.textContent='child';\n"
                    "modernRoot.append('before-',modernChild);\n"
                    "document.body.appendChild(modernRoot);\n"
                    "const modernClone=modernChild.cloneNode(true);\n"
                    "modernRoot.prepend(modernClone);\n"
                    "const modernOk="
                    "modernRoot.matches('div#modern-root.panel') && "
                    "modernChild.closest('#modern-root')===modernRoot && "
                    "modernRoot.contains(modernChild) && "
                    "modernRoot.firstElementChild===modernClone && "
                    "modernRoot.lastElementChild===modernChild && "
                    "modernRoot.childElementCount===2 && "
                    "modernRoot.toggleAttribute('data-ready') && "
                    "modernRoot.isConnected;\n"
                    "modernRoot.replaceChildren('modern-',modernChild);\n"
                    "modernChild.insertAdjacentText('beforebegin','adjacent-');\n"
                    "const modernNode=document.createElement('p');\n"
                    "modernNode.textContent=modernOk && "
                    "modernRoot.textContent==='modern-adjacent-child' && "
                    "modernRoot.classList.length===3 && "
                    "modernRoot.classList.contains('mounted') && "
                    "modernRoot.childElementCount===1"
                    "?'MODERN-DOM-OK':'MODERN-DOM-BAD';\n"
                    "document.body.appendChild(modernNode);\n"
                    "const identityText=document.createTextNode('identity');\n"
                    "identityText.nodeValue='updated';\n"
                    "const constructedText=new Text('made');\n"
                    "const constructedComment=new Comment();\n"
                    "const identityException="
                    "new DOMException('outside','IndexSizeError');\n"
                    "constructedText.data='changed';\n"
                    "const identityAttributes=modernChild.attributes;\n"
                    "const identityAttribute="
                    "identityAttributes.getNamedItem('data-user-id');\n"
                    "const identityNode=document.createElement('p');\n"
                    "identityNode.textContent="
                    "liveDataset===modernChild.dataset && "
                    "liveDataset.userId==='8' && "
                    "modernChild.getAttribute('data-user-id')==='8' && "
                    "modernChild.nodeType===Node.ELEMENT_NODE && "
                    "identityText.nodeType===Node.TEXT_NODE && "
                    "document.nodeType===Node.DOCUMENT_NODE && "
                    "modernChild.nodeName==='SPAN' && "
                    "modernChild.localName==='span' && "
                    "modernChild.ownerDocument===document && "
                    "document.ownerDocument===null && "
                    "identityText.nodeName==='#text' && "
                    "identityText.nodeValue==='updated' && "
                    "document instanceof HTMLDocument && "
                    "document instanceof Document && "
                    "identityText instanceof Text && "
                    "identityText instanceof CharacterData && "
                    "constructedText.data==='changed' && "
                    "constructedText.length===7 && "
                    "constructedComment instanceof Comment && "
                    "constructedComment instanceof CharacterData && "
                    "constructedComment.data==='' && "
                    "identityException instanceof DOMException && "
                    "identityException.code===DOMException.INDEX_SIZE_ERR && "
                    "identityException.toString()==='IndexSizeError: outside' && "
                    "identityAttributes instanceof NamedNodeMap && "
                    "identityAttribute instanceof Attr && "
                    "identityAttribute.ownerElement===modernChild && "
                    "identityAttribute.value==='8' && "
                    "modernChild instanceof Node && "
                    "modernChild instanceof Element && "
                    "modernChild instanceof HTMLElement && "
                    "document instanceof Node && !(document instanceof Element)"
                    "?'DOM-IDENTITY-OK':'DOM-IDENTITY-BAD';\n"
                    "document.body.appendChild(identityNode);\n"
                    "const svgNS='http://www.w3.org/2000/svg';\n"
                    "const identitySvg=document.createElementNS(svgNS,'svg');\n"
                    "const identityPath=document.createElementNS(svgNS,'path');\n"
                    "identitySvg.appendChild(identityPath);\n"
                    "const namespaceNode=document.createElement('p');\n"
                    "namespaceNode.textContent="
                    "identitySvg instanceof SVGElement && "
                    "identityPath instanceof SVGElement && "
                    "!(identitySvg instanceof HTMLElement) && "
                    "identitySvg.namespaceURI===svgNS && "
                    "identityPath.namespaceURI===svgNS && "
                    "identityPath.localName==='path'"
                    "?'DOM-NAMESPACE-OK':'DOM-NAMESPACE-BAD';\n"
                    "document.body.appendChild(namespaceNode);\n"
                    "const rangeHost=document.createElement('section');\n"
                    "rangeHost.innerHTML='<p>alpha <b>beta</b></p>';\n"
                    "document.body.appendChild(rangeHost);\n"
                    "const compatRange=document.createRange();\n"
                    "compatRange.selectNodeContents(rangeHost.firstElementChild);\n"
                    "const compatFragment=compatRange.createContextualFragment("
                    "'<i data-range=\"yes\">context</i>');\n"
                    "const compatSelection=getSelection();\n"
                    "compatSelection.removeAllRanges();\n"
                    "compatSelection.addRange(compatRange);\n"
                    "const rangeNode=document.createElement('p');\n"
                    "rangeNode.textContent="
                    "compatRange instanceof Range && "
                    "compatRange.toString()==='alpha beta' && "
                    "compatRange.cloneContents().textContent==='alpha beta' && "
                    "compatFragment instanceof DocumentFragment && "
                    "compatFragment.firstElementChild.dataset.range==='yes' && "
                    "compatSelection instanceof Selection && "
                    "compatSelection.rangeCount===1 && "
                    "compatSelection.getRangeAt(0)===compatRange && "
                    "compatSelection.toString()==='alpha beta'"
                    "?'RANGE-SELECTION-OK':'RANGE-SELECTION-BAD';\n"
                    "document.body.appendChild(rangeNode);\n"
                    "compatSelection.removeAllRanges();\n"
                    "const fragment=document.createDocumentFragment();\n"
                    "const fragmentComment=document.createComment('marker');\n"
                    "const fragmentElement=document.createElement('strong');\n"
                    "fragmentElement.textContent='fragment';\n"
                    "fragment.append(fragmentComment,fragmentElement,'-tail');\n"
                    "const fragmentClone=fragment.cloneNode(true);\n"
                    "const fragmentHost=document.createElement('section');\n"
                    "document.body.appendChild(fragmentHost);\n"
                    "fragmentHost.appendChild(fragment);\n"
                    "const fragmentIterator=document.createNodeIterator("
                    "fragmentHost,NodeFilter.SHOW_ELEMENT);\n"
                    "const iteratorRoot=fragmentIterator.nextNode();\n"
                    "const iteratorElement=fragmentIterator.nextNode();\n"
                    "const fragmentWalker=document.createTreeWalker("
                    "fragmentHost,NodeFilter.SHOW_ELEMENT);\n"
                    "const fragmentNode=document.createElement('p');\n"
                    "fragmentNode.textContent="
                    "fragment instanceof DocumentFragment && "
                    "fragment.nodeType===Node.DOCUMENT_FRAGMENT_NODE && "
                    "fragment.nodeName==='#document-fragment' && "
                    "!fragment.hasChildNodes() && "
                    "fragmentHost.childNodes.length===3 && "
                    "fragmentHost.children.length===1 && "
                    "fragmentClone.childNodes.length===3 && "
                    "fragmentClone.textContent==='fragment-tail' && "
                    "fragmentComment.nodeType===Node.COMMENT_NODE && "
                    "fragmentComment.getRootNode()===document && "
                    "fragmentIterator instanceof NodeIterator && "
                    "iteratorRoot===fragmentHost && "
                    "iteratorElement===fragmentElement && "
                    "fragmentWalker instanceof TreeWalker && "
                    "fragmentWalker.nextNode()===fragmentElement"
                    "?'DOM-FRAGMENT-OK':'DOM-FRAGMENT-BAD';\n"
                    "document.body.appendChild(fragmentNode);\n"
                    "const observedNode=document.createElement('div');\n"
                    "document.body.appendChild(observedNode);\n"
                    "let mutationWasSync=true;\n"
                    "const compatObserver=new MutationObserver(function(records,"
                    "same){\n"
                    " const mutationNode=document.createElement('p');\n"
                    " mutationNode.textContent="
                    "!mutationWasSync && this===compatObserver && "
                    "same===compatObserver && records.length===4 && "
                    "records[0] instanceof MutationRecord && "
                    "records[0].type==='attributes' && "
                    "records[0].attributeName==='data-state' && "
                    "records[0].oldValue===null && "
                    "records[1].oldValue==='one' && "
                    "records[2].type==='childList' && "
                    "records[2].addedNodes.length===1 && "
                    "records[3].type==='characterData' && "
                    "records[3].oldValue==='before'"
                    "?'MUTATION-OBSERVER-OK':'MUTATION-OBSERVER-BAD';\n"
                    " document.body.appendChild(mutationNode);\n"
                    " compatObserver.disconnect();\n"
                    "});\n"
                    "compatObserver.observe(observedNode,{attributes:true,"
                    "attributeOldValue:true,childList:true,subtree:true,"
                    "characterDataOldValue:true,"
                    "attributeFilter:['DATA-STATE']});\n"
                    "observedNode.setAttribute('data-ignored','no');\n"
                    "observedNode.setAttribute('data-state','one');\n"
                    "observedNode.setAttribute('data-state','two');\n"
                    "const observedText=document.createTextNode('before');\n"
                    "observedNode.appendChild(observedText);\n"
                    "observedText.nodeValue='after';\n"
                    "mutationWasSync=false;\n"
                    "let timerArguments='',frameTimestamp=-1;\n"
                    "let cancelledFrame=0,cancelledIdle=0;\n"
                    "setTimeout(function(a,b){timerArguments=a+b;},"
                    "1,'left','right');\n"
                    "const cancelledFrameId=requestAnimationFrame(function(){"
                    "cancelledFrame++;});\n"
                    "cancelAnimationFrame(cancelledFrameId);\n"
                    "requestAnimationFrame(function(timestamp){"
                    "frameTimestamp=timestamp;});\n"
                    "const cancelledIdleId=requestIdleCallback(function(){"
                    "cancelledIdle++;});\n"
                    "cancelIdleCallback(cancelledIdleId);\n"
                    "requestIdleCallback(function(deadline){\n"
                    " const timingNode=document.createElement('p');\n"
                    " timingNode.textContent="
                    "timerArguments==='leftright' && frameTimestamp>=0 && "
                    "!deadline.didTimeout && deadline.timeRemaining()>0 && "
                    "cancelledFrame===0 && cancelledIdle===0 && "
                    "typeof performance.timeOrigin==='number'"
                    "?'TIMING-COMPAT-OK':'TIMING-COMPAT-BAD';\n"
                    " document.body.appendChild(timingNode);\n"
                    "});\n"
                    "const beaconNode=document.createElement('p');\n"
                    "beaconNode.textContent="
                    "navigator.sendBeacon('/api/beacon','BEACON')"
                    "?'SEND-BEACON-OK':'SEND-BEACON-BAD';\n"
                    "document.body.appendChild(beaconNode);\n"
                    "const apiRequest=new Request('/api/data',{"
                    "method:'POST',body:new Blob(['q=1'],{"
                    "type:'application/x-www-form-urlencoded'}),headers:{"
                    "'X-Kestrel':'request'}});\n"
                    "fetch(apiRequest)\n"
                    ".then(function(response){return response.json();})\n"
                    ".then(function(data){\n"
                    " const fetched=document.createElement('p');\n"
                    " fetched.textContent=data.marker;\n"
                    " document.body.appendChild(fetched);\n"
                    "});\n"
                    "const requestNode=document.createElement('p');\n"
                    "requestNode.textContent=apiRequest instanceof Request && "
                    "apiRequest.bodyUsed && apiRequest.method==='POST' && "
                    "apiRequest.headers.get('x-kestrel')==='request'"
                    "?'REQUEST-API-OK':'REQUEST-API-BAD';\n"
                    "document.body.appendChild(requestNode);\n"
                    "const policyRequest=new Request('/api/credentials',{"
                    "mode:'same-origin',credentials:'omit',cache:'no-store',"
                    "redirect:'follow',referrer:'../source',"
                    "referrerPolicy:'origin',integrity:'sha256-proof',"
                    "keepalive:true});\n"
                    "const policyNode=document.createElement('p');\n"
                    "policyNode.textContent="
                    "policyRequest.mode==='same-origin' && "
                    "policyRequest.credentials==='omit' && "
                    "policyRequest.cache==='no-store' && "
                    "policyRequest.redirect==='follow' && "
                    "policyRequest.referrer.indexOf('/source')>0 && "
                    "policyRequest.referrerPolicy==='origin' && "
                    "policyRequest.integrity==='sha256-proof' && "
                    "policyRequest.keepalive"
                    "?'REQUEST-POLICY-OK':'REQUEST-POLICY-BAD';\n"
                    "document.body.appendChild(policyNode);\n"
                    "fetch(policyRequest).then(function(response){"
                    "return response.text();}).then(function(marker){\n"
                    " const credentialsNode=document.createElement('p');\n"
                    " credentialsNode.textContent=marker;\n"
                    " document.body.appendChild(credentialsNode);\n"
                    "});\n"
                    "const multipart=new FormData();\n"
                    "multipart.append('q','form value');\n"
                    "multipart.append('upload',new File(['FILE'],"
                    "'note.txt',{type:'text/plain',lastModified:1234}));\n"
                    "fetch('/api/form',{method:'POST',body:multipart})\n"
                    ".then(function(response){return response.json();})\n"
                    ".then(function(data){\n"
                    " const formNode=document.createElement('p');\n"
                    " formNode.textContent=data.marker;\n"
                    " document.body.appendChild(formNode);\n"
                    "});\n"
                    "fetch('/api/redirect').then(function(response){\n"
                    " const redirectNode=document.createElement('p');\n"
                    " redirectNode.textContent=response.status===200 && "
                    "response.redirected && response.type==='basic' && "
                    "response.url.indexOf('/api/redirected')>0"
                    "?'REDIRECT-FETCH-OK':'REDIRECT-FETCH-BAD';\n"
                    " document.body.appendChild(redirectNode);\n"
                    "});\n"
                    "fetch('/api/redirect',{redirect:'manual'})"
                    ".then(function(response){\n"
                    " const manualNode=document.createElement('p');\n"
                    " manualNode.textContent=response.status===302 && "
                    "!response.redirected"
                    "?'REDIRECT-MANUAL-OK':'REDIRECT-MANUAL-BAD';\n"
                    " document.body.appendChild(manualNode);\n"
                    "});\n"
                    "fetch('/api/redirect',{redirect:'error'})"
                    ".then(function(){throw Error('redirect followed');})"
                    ".catch(function(){\n"
                    " const errorNode=document.createElement('p');\n"
                    " errorNode.textContent='REDIRECT-ERROR-OK';\n"
                    " document.body.appendChild(errorNode);\n"
                    "});\n"
                    "fetch(policyUrl,{mode:'same-origin'})"
                    ".then(function(){throw Error('cross-origin allowed');})"
                    ".catch(function(){\n"
                    " const modeNode=document.createElement('p');\n"
                    " modeNode.textContent='SAME-ORIGIN-MODE-OK';\n"
                    " document.body.appendChild(modeNode);\n"
                    "});\n"
                    "Promise.all([Promise.resolve('PROMISE'),'-ALL-','OK'])\n"
                    ".then(function(parts){\n"
                    " const joined=document.createElement('p');\n"
                    " joined.textContent=parts.join('');\n"
                    " document.body.appendChild(joined);\n"
                    "});\n"
                    "fetch('/module.wasm')\n"
                    ".then(function(response){return response.arrayBuffer();})\n"
                    ".then(function(buffer){return WebAssembly.instantiate(buffer);})\n"
                    ".then(function(pair){\n"
                    " const wasm=document.createElement('p');\n"
                    " wasm.textContent=pair.instance.exports.add(20,22)===42"
                    "?'WASM-FETCH-OK':'WASM-FETCH-BAD';\n"
                    " document.body.appendChild(wasm);\n"
                    "});\n"
                    "const metaOk=import.meta.url.indexOf('/main.mjs')>=0;\n"
                    "import('./dynamic.mjs').then(function(namespace){\n"
                    " const dynamic=document.createElement('p');\n"
                    " dynamic.textContent=namespace.marker+' '+"
                    "(metaOk?'META-URL-OK':'META-URL-BAD');\n"
                    " document.body.appendChild(dynamic);\n"
                    "});\n"
                    "export const exportedImport=import('./dynamic.mjs');\n"
                    "exportedImport.then(function(namespace){\n"
                    " const exported=document.createElement('p');\n"
                    " exported.textContent=namespace.marker==="
                    "'DYNAMIC-MODULE-OK'?'EXPORT-DYNAMIC-OK':"
                    "'EXPORT-DYNAMIC-BAD';\n"
                    " document.body.appendChild(exported);\n"
                    "});\n"
                    "export const moduleReady=true;\n"
                ).encode("ascii")
            elif path == "/dep.mjs":
                content_type = "text/javascript"
                response = (
                    "const base='MODULE';\n"
                    "export default base;\n"
                    "export const suffix='OK';\n"
                ).encode("ascii")
            elif path == "/dynamic.mjs":
                content_type = "text/javascript"
                response = b"export const marker='DYNAMIC-MODULE-OK';\n"
            elif path == "/cors.mjs":
                content_type = "text/javascript"
                response = b"export const corsMarker='CORS-MODULE-OK';\n"
            elif path == "/bad-mime.mjs":
                content_type = "text/plain"
                response = (
                    "const bad=document.createElement('p');"
                    "bad.textContent='BAD-MIME-EXECUTED';"
                    "document.body.appendChild(bad);"
                ).encode("ascii")
            elif path == "/blocked.mjs":
                content_type = "text/javascript"
                response = (
                    "const bad=document.createElement('p');"
                    "bad.textContent='CORS-BLOCK-EXECUTED';"
                    "document.body.appendChild(bad);"
                ).encode("ascii")
            elif path == "/api/redirected":
                content_type = "text/plain"
                response = b"redirect complete"
            elif path == "/api/credentials":
                content_type = "text/plain"
                response = (
                    b"CREDENTIALS-OMIT-OK"
                    if not self.headers.get("Cookie")
                    else b"CREDENTIALS-OMIT-BAD"
                )
            elif path == "/pixel.png":
                content_type = "image/png"
                response = base64.b64decode(
                    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwC"
                    "AAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=")
            elif path == "/module.wasm":
                content_type = "application/wasm"
                response = bytes([
                    0, 97, 115, 109, 1, 0, 0, 0,
                    1, 7, 1, 96, 2, 127, 127, 1, 127,
                    3, 2, 1, 0,
                    7, 7, 1, 3, 97, 100, 100, 0, 0,
                    10, 9, 1, 7, 0, 32, 0, 32, 1, 106, 11,
                ])
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(response)))
            if path == "/compat":
                self.send_header(
                    "Set-Cookie",
                    "servercookie=server; Path=/; HttpOnly")
            if path == "/cors.mjs":
                self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(response)

        def do_POST(self):
            path = self.path.split("?", 1)[0]
            self.server.record_get(path)
            length = int(self.headers.get("Content-Length", "0"))
            request = self.rfile.read(length)
            if path == "/api/beacon":
                valid = (
                    request == b"BEACON"
                    and self.headers.get("Content-Type")
                    == "text/plain;charset=UTF-8"
                    and "servercookie=server"
                    in self.headers.get("Cookie", "")
                )
                marker = "BEACON-DELIVERY-OK" if valid else "BEACON-DELIVERY-BAD"
            elif path == "/api/form":
                content_type = self.headers.get("Content-Type", "")
                valid = (
                    content_type.startswith(
                        "multipart/form-data; boundary="
                    )
                    and b'name="q"' in request
                    and b"form value" in request
                    and b'name="upload"; filename="note.txt"' in request
                    and b"Content-Type: text/plain" in request
                    and b"FILE" in request
                )
                marker = "FORMDATA-FETCH-OK" if valid else "FORMDATA-FETCH-BAD"
            else:
                valid = (
                    request == b"q=1"
                    and self.headers.get("Content-Type")
                    == "application/x-www-form-urlencoded"
                    and self.headers.get("X-Kestrel") == "request"
                )
                marker = "PROMISE-FETCH-OK" if valid else "FETCH-BAD"
            response = ('{"marker":"%s"}' % marker).encode("ascii")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(response)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(response)

        def log_message(self, fmt, *args):
            del fmt, args

    return Handler


class BrowserFixtureServers:
    """Controlled plain HTTP and certificate-negative TLS 1.3 endpoints."""

    def __init__(self):
        self.temp = None
        self.http = None
        self.cors = None
        self.https = None
        self.threads = []
        self.http_url = None
        self.compat_url = None
        self.module_policy_url = None
        self.cors_url = None
        self.https_url = None

    @staticmethod
    def _serve(server):
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        return thread

    def start(self):
        openssl = shutil.which("openssl")
        if not openssl:
            raise RuntimeError("openssl is required for browser TLS E2E")

        self.temp = tempfile.TemporaryDirectory(
            prefix="kestrel-browser-e2e-")
        cert = os.path.join(self.temp.name, "negative-cert.pem")
        key = os.path.join(self.temp.name, "negative-key.pem")
        cmd = [
            openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-sha256", "-days", "2", "-set_serial", "1",
            "-subj", "/CN=kestrel-negative.invalid",
            "-addext", "subjectAltName=DNS:kestrel-negative.invalid",
            "-addext", "basicConstraints=critical,CA:FALSE",
            "-addext", "keyUsage=critical,digitalSignature,keyEncipherment",
            "-addext", "extendedKeyUsage=serverAuth",
            "-keyout", key, "-out", cert,
        ]
        made = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True)
        if made.returncode != 0:
            raise RuntimeError("cannot generate negative TLS certificate: %s"
                               % made.stderr.strip())

        self.http = _QuietHTTPServer(
            ("0.0.0.0", 0), _marker_handler(HTTP_BODY_MARKER))
        self.cors = _QuietHTTPServer(
            ("0.0.0.0", 0), _marker_handler(HTTP_BODY_MARKER))
        self.https = _QuietHTTPServer(
            ("0.0.0.0", 0), _marker_handler(TLS_NEGATIVE_BODY_MARKER))
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_3
        context.maximum_version = ssl.TLSVersion.TLSv1_3
        context.load_cert_chain(cert, key)
        self.https.socket = context.wrap_socket(
            self.https.socket, server_side=True)

        self.threads.append(self._serve(self.http))
        self.threads.append(self._serve(self.cors))
        self.threads.append(self._serve(self.https))
        self.cors_url = "http://%s:%d/cors.mjs" % (
            QEMU_HOST, self.cors.server_address[1])
        self.http.cors_module_url = self.cors_url
        self.http.blocked_module_url = "http://%s:%d/blocked.mjs" % (
            QEMU_HOST, self.cors.server_address[1])
        self.http_url = "http://%s:%d/plain" % (
            QEMU_HOST, self.http.server_address[1])
        self.compat_url = "http://%s:%d/compat" % (
            QEMU_HOST, self.http.server_address[1])
        self.module_policy_url = "http://%s:%d/module-policy" % (
            QEMU_HOST, self.http.server_address[1])
        self.https_url = "https://%s:%d/negative" % (
            QEMU_HOST, self.https.server_address[1])
        print("browser fixtures: plain=%s tls13-negative=%s "
              "cert-cn=kestrel-negative.invalid"
              % (self.http_url, self.https_url))
        return self

    def close(self):
        for server in (self.http, self.cors, self.https):
            if server:
                server.shutdown()
                server.server_close()
        for thread in self.threads:
            thread.join(timeout=5)
        self.threads = []
        self.http = None
        self.cors = None
        self.https = None
        if self.temp:
            self.temp.cleanup()
            self.temp = None


PROMPT = "kestrel:"
DOTTED_QUAD = r"\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b"

SHELL_PROMPT = r"kestrel:[^\n]*\$"
LOGIN_PROMPT = r"\nlogin: "

# /etc/shadow ships these on purpose; see docs/users.md.
ROOT_USER, ROOT_PASS = "root", "root"
USER_NAME, USER_PASS = "kestrel", "kestrel"
MAX_LOGIN_TRIES = 3

# Set by t_pipe once it has found out whether this shell understands '|'.
# The shell grew redirection before it grew pipes, so the tests that need
# one have to ask rather than assume.
have_pipes = False
browser_fixtures = None


def wait_prompt(h, timeout=DEFAULT_TIMEOUT):
    h.expect_any([SHELL_PROMPT], timeout=timeout, regex=True)


def t_boot(h):
    h.expect("KESTREL READY", timeout=BOOT_TIMEOUT)


def t_login(h):
    """Get to a shell, logging in on the console if init asks us to.

    /etc/inittab respawns /bin/login unless /etc/autologin exists, so both
    outcomes are legal and the harness has to handle both: answer the login
    prompt, or find a shell already waiting. Every single test after this
    one needs a shell, which is why it retries and why it is deliberately
    the only test allowed to match a bare prompt.

    On the login path the shell's *first* prompt is left unconsumed, so
    t_prompt has something to match; on the autologin path the prompt has
    already been eaten, so a no-output `cd /` is sent to produce another.
    """
    for _ in range(MAX_LOGIN_TRIES):
        which = h.expect_any([LOGIN_PROMPT, SHELL_PROMPT],
                             timeout=BOOT_TIMEOUT, regex=True)
        if which == SHELL_PROMPT:
            h.send("cd /")
            return
        h.send(ROOT_USER)
        h.expect("password:", timeout=DEFAULT_TIMEOUT)
        h.send(ROOT_PASS)
        # login sleeps 2s after a bad password before asking again.
        got = h.expect_any(["welcome, " + ROOT_USER, "login incorrect"],
                           timeout=DEFAULT_TIMEOUT)
        if got != "login incorrect":
            return
    raise TimeoutError("could not log in as %s after %d tries"
                       % (ROOT_USER, MAX_LOGIN_TRIES))


def t_prompt(h):
    h.expect("kestrel:/$", timeout=DEFAULT_TIMEOUT)


def t_help(h):
    h.send("help")
    h.expect("commands")
    wait_prompt(h)


def t_echo(h):
    h.send("echo hello world")
    h.expect("hello world")
    wait_prompt(h)


def t_ls_bin(h):
    h.send("ls /bin")
    h.expect("ls")     # alphabetical: ls prints before sh
    h.expect("sh")
    wait_prompt(h)


def t_cat_motd(h):
    h.send("cat /etc/motd")
    h.expect("Kestrel")
    wait_prompt(h)


def t_fs_roundtrip(h):
    h.send("writefile /tmp1.txt")
    time.sleep(0.2)
    h.send("roundtrip-data-123")
    time.sleep(0.2)
    h.send_raw("\x04")
    wait_prompt(h)
    h.send("cat /tmp1.txt")
    h.expect("roundtrip-data-123")
    wait_prompt(h)


def t_ps(h):
    h.send("ps")
    h.expect("sh")
    wait_prompt(h)


def t_free(h):
    h.send("free")
    h.expect_any(["MiB", "KiB"])
    wait_prompt(h)


def t_rng(h):
    h.send("rngtest")
    h.expect("rngtest: getrandom + random devices verified")
    wait_prompt(h)


def t_demand_paging(h):
    h.send("vmstress 8")
    h.expect("vmstress: demand paging verified across 8 MiB",
             timeout=PKG_TIMEOUT)
    wait_prompt(h, timeout=PKG_TIMEOUT)


def t_signals(h):
    h.send("sigtest")
    h.expect("sigtest: handlers + masks + sigreturn verified")
    wait_prompt(h)


def t_dynamic_linking(h):
    h.send("dynhello")
    h.expect("dynhello: DT_NEEDED library returned 42")
    wait_prompt(h)


def t_ping(h):
    h.send("ping 10.0.2.2")
    got = h.expect_any(["reply", "rtt", "network unavailable"])
    wait_prompt(h)
    if got == "network unavailable":
        return "SKIP"


def t_nslookup(h):
    h.send("nslookup example.com")
    got = h.expect_any([DOTTED_QUAD, re.escape("network unavailable")],
                       regex=True)
    wait_prompt(h)
    if got != DOTTED_QUAD:
        return "SKIP"


def t_uptime(h):
    h.send("uptime")
    # "up H:MM:SS" — a bare "s" would match the prompt and eat it
    h.expect_any([r"up \d+:\d\d:\d\d", r"\d+ ?ms"], regex=True)
    wait_prompt(h)


def t_calc(h):
    h.send("calc 2*(3+4)")
    # "\n14\n" cannot match the echoed command line or the prompt
    h.expect(r"\n14\n", regex=True)
    wait_prompt(h)


def t_calc_divzero(h):
    h.send("calc 1/0")
    h.expect("divide by zero")
    wait_prompt(h)


def t_cp_wc(h):
    h.send("cp /etc/version /t2.txt")
    wait_prompt(h)
    h.send("wc -c /t2.txt")
    # wc prints "<count> /t2.txt"; the echoed command has no digits there
    h.expect(r"\n *\d+ /t2\.txt\n", regex=True)
    wait_prompt(h)


def t_mv_rm(h):
    h.send("mv /t2.txt /t3.txt")
    wait_prompt(h)
    h.send("cat /t3.txt")
    h.expect("KestrelOS 0.1.0")     # body of /etc/version, moved twice
    wait_prompt(h)
    h.send("rm /t3.txt")
    wait_prompt(h)
    h.send("cat /t3.txt")
    h.expect("cat: cannot open")    # not present in the echoed command
    wait_prompt(h)


def t_mkdir_ls(h):
    h.send("mkdir /d1")
    wait_prompt(h)
    h.send("ls /")
    # ls prints "d <size>  <name>"; the leading 'd' flag makes it unique
    h.expect(r"\nd +\d+ +d1\n", regex=True)
    wait_prompt(h)


def t_grep(h):
    h.send("grep -n Kestrel /etc/motd")
    # "<lineno>:<line>" — the echoed command has no "<digits>:"
    h.expect(r"\n\d+:[^\n]*KestrelOS", regex=True)
    wait_prompt(h)


def t_head(h):
    h.send("head -n 1 /doc/welcome.md")
    # requiring the prompt immediately after asserts "first line only"
    h.expect(r"# Welcome to KestrelOS\nkestrel:[^\n]*\$", regex=True)


def t_tail(h):
    h.send("tail -n 1 /doc/welcome.md")
    # last line of welcome.md, and nothing after it but the prompt
    h.expect(r"for the release\.\nkestrel:[^\n]*\$", regex=True)


def t_tree(h):
    h.send("tree /etc")
    h.expect(r"\d+ director(y|ies), \d+ files?", regex=True,
             timeout=WALK_TIMEOUT)
    wait_prompt(h, timeout=WALK_TIMEOUT)


def t_du(h):
    h.send("du /etc")
    h.expect(r"\n *\d+  /etc\n", regex=True, timeout=WALK_TIMEOUT)
    wait_prompt(h, timeout=WALK_TIMEOUT)


def t_find(h):
    h.send("find /etc")
    # a path the echoed "find /etc" cannot contain
    h.expect(r"\n/etc/version\n", regex=True, timeout=WALK_TIMEOUT)
    wait_prompt(h, timeout=WALK_TIMEOUT)


def t_date(h):
    h.send("date")
    got = h.expect_any([r"\d{4}-\d\d-\d\d", re.escape("no clock")],
                       regex=True)
    wait_prompt(h)
    if got != r"\d{4}-\d\d-\d\d":
        return "SKIP"


def t_err_cat_missing(h):
    h.send("cat /nope")
    h.expect("cat: cannot open")
    wait_prompt(h)


def t_err_rm_missing(h):
    h.send("rm /nope")
    h.expect("rm: cannot remove")
    wait_prompt(h)


def t_err_unknown_cmd(h):
    h.send("notacommand42")
    # the echoed line is just the word; this prefix is shell-only
    h.expect("sh: command not found")
    wait_prompt(h)


def t_long_line(h):
    # 300 chars: past the shell's 256-byte line buffer. Excess input is
    # dropped by readline, so the shell must still reach a prompt.
    h.send("zq" * 150)
    h.expect_any(["sh: command not found", "sh: too many tokens"])
    wait_prompt(h)


# ---- accounts and permissions ----------------------------------------


def t_whoami(h):
    # "\nroot\n" cannot match the echoed "whoami" or the prompt
    h.send("whoami")
    h.expect(r"\nroot\n", regex=True)
    wait_prompt(h)


def t_id(h):
    h.send("id")
    h.expect(r"uid=0\(root\) gid=0\(root\)", regex=True)
    wait_prompt(h)


def t_permissions(h):
    """An unprivileged account must not be able to read /etc/shadow.

    su drops to uid 1000 and spawns a nested shell; /etc/shadow is mode
    0600 owned by root and there is no setuid bit, so the read has to
    fail. Leaving the nested shell brings the suite back to root.
    """
    h.send("su " + USER_NAME)
    h.expect("password:")
    h.send(USER_PASS)
    h.expect("su: root -> " + USER_NAME)
    wait_prompt(h)

    h.send("whoami")
    h.expect("\n" + USER_NAME + "\n", regex=True)
    wait_prompt(h)

    h.send("id")
    h.expect(r"uid=1000\(kestrel\)", regex=True)
    wait_prompt(h)

    h.send("cat /etc/shadow")
    h.expect("cat: cannot open")
    wait_prompt(h)

    # ... and root can still read it, which is what makes the failure
    # above a permission check rather than a missing file.
    h.send("exit")
    h.expect("su: back to root")
    wait_prompt(h)
    h.send("cat /etc/shadow")
    h.expect(":4096:", timeout=DEFAULT_TIMEOUT)
    wait_prompt(h)


# ---- plumbing: redirection, pipes, /dev -------------------------------


def t_redirect(h):
    """'>' , '>>' and '<' still wire up fd 0/1 through spawn_io()."""
    h.send("echo redirect-marker > /t4.txt")
    wait_prompt(h)
    h.send("cat /t4.txt")
    h.expect(r"\nredirect-marker\n", regex=True)
    wait_prompt(h)
    # "redirect-marker\n" is 16 bytes; wc reading it off fd 0 proves '<'
    h.send("wc -c < /t4.txt")
    h.expect(r"\n *16\n", regex=True)
    wait_prompt(h)
    h.send("echo appended-line >> /t4.txt")
    wait_prompt(h)
    h.send("wc -l /t4.txt")
    h.expect(r"\n *2 /t4\.txt\n", regex=True)
    wait_prompt(h)
    h.send("rm /t4.txt")
    wait_prompt(h)


def t_pipe(h):
    """`ls /bin | wc -l`, if this shell has pipes yet.

    Without them sh hands '|', 'wc' and '-l' to ls as arguments and ls
    rejects '-l' with its usage line, which is what the SKIP branch
    matches. Sets have_pipes for the tests that want to use one.
    """
    global have_pipes

    h.send("ls /bin | wc -l")
    h.expect("| wc -l")             # consume the echoed command line
    got = h.expect_any([r"\n *\d+\n", r"usage: ls \[-a\]"], regex=True)
    wait_prompt(h)
    if got != r"\n *\d+\n":
        return "SKIP"
    have_pipes = True


def t_dev(h):
    """/dev: the devfs mount, an immediate-EOF device and an empty dump.

    /dev/zero is an endless stream, so it is only ever read through a
    pipe - and only when the shell has pipes and hexdump gives up on a
    closed one. Reading it unpiped would wedge the harness.
    """
    h.send("cat /dev/null")
    wait_prompt(h)

    h.send("ls /dev")
    # ls prints "- <size>  <name>"; "zero" appears nowhere in "ls /dev"
    h.expect(r"\n- +\d+ +zero\n", regex=True)
    wait_prompt(h)

    h.send("hexdump /dev/null")
    # an empty file dumps as nothing but the final offset
    h.expect(r"\n00000000\n", regex=True)
    wait_prompt(h)

    if have_pipes:
        h.send("cat /dev/null | wc -c")
        h.expect("| wc -c")
        h.expect(r"\n *0\n", regex=True)
        wait_prompt(h)


# ---- kernel log and services ------------------------------------------


def t_dmesg(h):
    h.send("dmesg -n 5")
    # "[<ticks>] <level> <tag> (<pid>) <msg>" - the brackets and the
    # level word are unique to the log format
    h.expect(r"\n\[ *\d+\] (info|warn|error|debug) ", regex=True)
    wait_prompt(h)


def t_service_list(h):
    h.send("service list")
    h.expect("RESTARTS")            # column header, not in the command
    h.expect(r"\nlogger +\w+", regex=True)
    wait_prompt(h)


def t_service_lifecycle(h):
    h.send("service status readiness")
    h.expect(r"\n  state +exited\n", regex=True)
    h.expect("ready=/run/readiness.ready")
    wait_prompt(h)

    h.send("service start dependent")
    h.expect("ok start dependent (running)")
    wait_prompt(h)

    h.send("service status dependent")
    h.expect(r"\n  state +running\n", regex=True)
    h.expect("requires=readiness")
    wait_prompt(h)

    h.send("service reload dependent")
    h.expect("ok reload dependent (restarting pid")
    wait_prompt(h)
    h.send("sleep 1")
    wait_prompt(h)

    # A hard requirement disappearing must stop and fail its dependents.
    h.send("service stop readiness")
    h.expect("ok stop readiness (was not running)")
    wait_prompt(h)

    # The control acknowledgement publishes dependency failure atomically;
    # status must not expose a stale "running" window.
    h.send("service status dependent")
    h.expect(r"\n  state +failed\n", regex=True)
    wait_prompt(h)
    h.send("sleep 1")
    wait_prompt(h)

    h.send("service reset-failed dependent")
    h.expect("ok reset-failed dependent (stopped)")
    wait_prompt(h)

    # Restore the one-shot requirement and prove its dependent can start
    # again after the failure state has been reset.
    h.send("service start readiness")
    h.expect("ok start readiness (running)")
    wait_prompt(h)
    h.send("sleep 1")
    wait_prompt(h)

    h.send("service start dependent")
    h.expect("ok start dependent (running)")
    wait_prompt(h)

    h.send("service stop dependent")
    h.expect("ok stop dependent (pid")
    wait_prompt(h)
    h.send("sleep 1")
    wait_prompt(h)

    h.send("service status dependent")
    h.expect(r"\n  state +stopped\n", regex=True)
    wait_prompt(h)

    h.send("service reset-failed dependent")
    h.expect("ok reset-failed dependent (stopped)")
    wait_prompt(h)

    # Reload installs configuration but must not turn an inactive unit back
    # on merely because it was manually enabled earlier in the boot.
    h.send("service reload dependent")
    h.expect("ok reload dependent (stopped)")
    wait_prompt(h)

    # A service which never publishes ready= must be killed at its deadline
    # and remain failed until reset-failed.
    h.send("service start readiness-fail")
    h.expect("err start readiness-fail (failed)")
    wait_prompt(h)
    h.send("service status readiness-fail")
    h.expect(r"\n  state +failed\n", regex=True)
    wait_prompt(h)
    h.send("service reset-failed readiness-fail")
    h.expect("ok reset-failed readiness-fail (stopped)")
    wait_prompt(h)


def t_lsmod(h):
    """Loadable modules are landing in this wave; tolerate their absence.

    The existence check is `ls /bin/lsmod` rather than running lsmod, so
    a build without the module tooling SKIPs instead of colliding with
    the shell's "command not found" line.
    """
    h.send("ls /bin/lsmod")
    got = h.expect_any([r"\n1 entry\n", r"ls: cannot access"], regex=True)
    wait_prompt(h)
    if got != r"\n1 entry\n":
        return "SKIP"

    h.send("lsmod")
    h.expect_any([r"sh: command not found",
                  r"no modules loaded",
                  r"\n(Module|MODULE|module|name) +\w",
                  r"\n[a-z0-9_.-]+ +\d+ +\d+",
                  r"lsmod: [^\n]+"], regex=True)
    wait_prompt(h)


# ---- packages ---------------------------------------------------------


def t_kpkg_list(h):
    h.send("kpkg list")
    h.expect_any([r"kpkg: no packages installed",
                  r"\n\d+ packages? installed\n"], regex=True)
    wait_prompt(h)


def t_kpkg_install(h):
    """Install `hello`, which pulls kestrel-extras in as a dependency."""
    h.send("kpkg install hello")
    h.expect("kpkg: installed kestrel-extras", timeout=PKG_TIMEOUT)
    h.expect("kpkg: installed hello", timeout=PKG_TIMEOUT)
    wait_prompt(h, timeout=PKG_TIMEOUT)

    h.send("kpkg list")
    h.expect(r"\n2 packages installed\n", regex=True, timeout=PKG_TIMEOUT)
    wait_prompt(h, timeout=PKG_TIMEOUT)


def t_pkg_hello(h):
    h.send("hello e2e")
    h.expect("Hello, e2e!")
    wait_prompt(h)


def t_pkg_cal(h):
    h.send("cal 7 2026")
    h.expect("July 2026")           # the command line says "7", not "July"
    h.expect("Su Mo Tu We Th Fr Sa")
    h.expect(r"\n[ 0-9]*\b31\b[ 0-9]*\n", regex=True)   # July has 31 days
    wait_prompt(h)


def t_pkg_factor(h):
    h.send("factor 97 360")
    h.expect(r"\n97: 97\n", regex=True)
    # No leading "\n": the previous match consumed the newline these two
    # lines share. "360: 2" cannot occur in the echoed "factor 97 360".
    h.expect(r"360: 2 2 2 3 3 5\n", regex=True)
    wait_prompt(h)


def t_kpkg_verify(h):
    h.send("kpkg verify")
    h.expect("kpkg: everything matches", timeout=PKG_TIMEOUT)
    wait_prompt(h, timeout=PKG_TIMEOUT)


# ---- browser ----------------------------------------------------------


def _validate_tls_negative_block(block, attempt):
    """Validate one complete controlled certificate-rejection transcript."""
    if TLS_NEGATIVE_BODY_MARKER in block:
        raise TimeoutError(
            "certificate-negative attempt %d rendered the server body"
            % attempt)

    forbidden = TLS_NEGATIVE_FORBIDDEN_RE.search(block)
    if forbidden:
        raise TimeoutError(
            "certificate-negative attempt %d contained forbidden output %r"
            % (attempt, forbidden.group(0)))

    exits = TLS_NEGATIVE_EXIT_RE.findall(block)
    if exits != ["1"]:
        raise TimeoutError(
            "certificate-negative attempt %d shell exits were %r, not ['1']"
            % (attempt, exits))

    statuses = TLS_NEGATIVE_STATUS_RE.findall(block)
    if statuses != ["1"]:
        raise TimeoutError(
            "certificate-negative attempt %d status lines were %r, "
            "not ['1']" % (attempt, statuses))

    for line in block.splitlines():
        if (TLS_NEGATIVE_DIAGNOSTIC_RE.search(line) and
                TLS_NEGATIVE_REASON_RE.search(line)):
            return line.strip()
    raise TimeoutError(
        "certificate-negative attempt %d lacked a certificate/hostname "
        "diagnostic containing the SAN or an equivalent trust reason"
        % attempt)


def t_browser_text(h):
    """The browser pipeline, run over a styled local page in text mode.

    rootfs/doc/test.html exercises headings, wrapped body text, inline
    styles, an author stylesheet, entities, lists, <pre>, a table, links
    and an <img> alt.  Text mode cannot expose its color declaration, but
    it does prove that the styled document remains readable and that both
    relative and absolute links survive the shared pipeline.
    """
    h.send("browser -t -l /doc/test.html; "
           "echo BROWSER-LOCAL-STATUS-$?")
    h.expect("Kestrel Renderer Test", timeout=WALK_TIMEOUT)
    h.expect("CSS-AUTHOR-OK", timeout=WALK_TIMEOUT)
    h.expect("RENDERER-OK", timeout=WALK_TIMEOUT)
    h.expect("bullet-alpha", timeout=WALK_TIMEOUT)
    h.expect("PRE-BLOCK", timeout=WALK_TIMEOUT)
    h.expect("cell-body-b", timeout=WALK_TIMEOUT)
    h.expect("[ALT-TEXT]", timeout=WALK_TIMEOUT)
    h.expect("END-OF-PAGE", timeout=WALK_TIMEOUT)
    # -l resolves every href against the page URL
    h.expect(r"\n *\[1\] /doc/welcome\.md\n", regex=True,
             timeout=WALK_TIMEOUT)
    # The previous match consumed the newline shared by these two lines.
    h.expect(r" *\[2\] http://example\.com/\n", regex=True,
             timeout=WALK_TIMEOUT)
    h.expect(r"(?:^|\n)BROWSER-LOCAL-STATUS-0\n", regex=True,
             timeout=WALK_TIMEOUT)
    wait_prompt(h, timeout=WALK_TIMEOUT)


def t_browser_home(h):
    """The start page the desktop's Browser button opens.

    Regression guard: `browser` with no argument used to print usage and
    exit 2, so clicking Browser on the desktop opened nothing at all. The
    windowed no-argument case cannot be driven headlessly, so this checks
    the two things that break it -- the page existing and rendering.
    """
    # Short phrases only: the renderer wraps at 78 columns, so anything
    # long enough to straddle a line break would match nothing.
    h.send("browser -t /doc/home.html; echo BROWSER-HOME-STATUS-$?")
    h.expect("KestrelOS", timeout=WALK_TIMEOUT)
    h.expect("HTML parser", timeout=WALK_TIMEOUT)
    h.expect("Local pages", timeout=WALK_TIMEOUT)
    h.expect("TLS 1.3", timeout=WALK_TIMEOUT)
    h.expect(r"(?:^|\n)BROWSER-HOME-STATUS-0\n", regex=True,
             timeout=WALK_TIMEOUT)
    wait_prompt(h, timeout=WALK_TIMEOUT)


def t_browser_http_controlled(h):
    """Plain HTTP through /bin/browser against a deterministic host page."""
    if not browser_fixtures:
        raise TimeoutError("controlled browser servers were not started")
    end = "BROWSER-HTTP-END"
    h.send("/bin/browser -t %s; echo BROWSER-HTTP-STATUS-$?; echo %s"
           % (browser_fixtures.http_url, end))
    block = h.capture_until(r"\n%s\n" % end, timeout=NET_TIMEOUT, regex=True)
    if HTTP_BODY_MARKER not in block:
        raise TimeoutError("controlled HTTP body marker was absent")
    statuses = re.findall(
        r"(?m)^BROWSER-HTTP-STATUS-(-?\d+)$", block)
    if statuses != ["0"]:
        raise TimeoutError(
            "controlled HTTP status lines were %r, not ['0']" % statuses)
    wait_prompt(h, timeout=NET_TIMEOUT)


def t_browser_web_compat(h):
    """External CSS/imports, scripts, live DOM mutation, and images."""
    if not browser_fixtures:
        raise TimeoutError("controlled browser servers were not started")
    paths = ["/compat", "/compat.css", "/import.css",
             "/compat.js", "/main.mjs", "/dep.mjs", "/dynamic.mjs",
             "/api/data", "/api/form", "/api/beacon", "/api/redirect",
             "/api/redirected", "/api/credentials",
             "/pixel.png", "/module.wasm"]
    commented = "/commented-out.mjs"
    cors_path = "/cors.mjs"
    before = {p: browser_fixtures.http.path_count(p) for p in paths}
    commented_before = browser_fixtures.http.path_count(commented)
    cors_before = browser_fixtures.cors.path_count(cors_path)
    end = "BROWSER-COMPAT-END"
    h.send("/bin/browser -t -v %s; echo BROWSER-COMPAT-STATUS-$?; echo %s"
           % (browser_fixtures.compat_url, end))
    block = h.capture_until(r"\n%s\n" % end, timeout=NET_TIMEOUT, regex=True)
    counts = {p: browser_fixtures.http.path_count(p) - before[p]
              for p in paths}
    for marker in ("CSS-EXTERNAL-OK", "CSS-IMPORT-OK", "JS-DOM-OK",
                   "INLINE-JS-OK", "COOKIE-DOM-OK",
                   "COMPUTED-STYLE-OK",
                   "COMPAT-CONSOLE-OK", "TEXT-URL-API-OK",
                   "URL-ABORT-API-OK", "ABORT-TIMEOUT-OK",
                   "EVENT-API-OK", "EVENT-OPTIONS-OK",
                   "HEADERS-API-OK",
                   "COLLECTION-ITERATORS-OK",
                   "NAVIGATOR-API-OK",
                   "MATCH-MEDIA-OK",
                   "LOCATION-API-OK",
                   "HISTORY-API-OK",
                   "MODERN-DOM-OK",
                   "DOM-IDENTITY-OK",
                   "DOM-NAMESPACE-OK",
                   "RANGE-SELECTION-OK",
                   "DOM-FRAGMENT-OK",
                   "MUTATION-OBSERVER-OK",
                   "TIMING-COMPAT-OK",
                   "STYLE-DECLARATION-OK",
                   "MAP-SET-OK",
                   "WEAK-COLLECTIONS-OK",
                   "ES-STATIC-BUILTINS-OK",
                   "ES-PROTOTYPE-METHODS-OK",
                   "ES-MODERN-METHODS-OK",
                   "EXPLICIT-ITERATORS-OK",
                   "SEND-BEACON-OK",
                   "CRYPTO-RANDOM-OK",
                   "CRYPTO-DIGEST-OK",
                   "RESPONSE-API-OK",
                   "BLOB-API-OK",
                   "FILE-API-OK",
                   "FILE-READER-OK",
                   "OBJECT-URL-OK",
                   "BODY-READERS-OK",
                   "BODY-FORMDATA-OK",
                   "MODULE-IMPORT-OK",
                   "PROMISE-FETCH-OK", "REQUEST-API-OK",
                   "REQUEST-POLICY-OK", "CREDENTIALS-OMIT-OK",
                   "FORMDATA-FETCH-OK",
                   "REDIRECT-FETCH-OK", "REDIRECT-MANUAL-OK",
                   "REDIRECT-ERROR-OK", "SAME-ORIGIN-MODE-OK",
                   "PROMISE-ALL-OK",
                   "WASM-FETCH-OK", "DYNAMIC-MODULE-OK",
                   "META-URL-OK", "INLINE-META-OK",
                   "MINIFIED-TAIL-OK", "MEMBER-IMPORT-OK",
                   "EXPORT-DYNAMIC-OK", "CORS-MODULE-OK"):
        if marker not in block:
            raise TimeoutError("web compatibility marker %s was absent "
                               "(resource requests %r)" % (marker, counts))
    statuses = re.findall(r"(?m)^BROWSER-COMPAT-STATUS-(-?\d+)$", block)
    if statuses != ["0"]:
        raise TimeoutError("browser compatibility status was %r" % statuses)
    if not re.search(r"(?m)^subresources: 15  resource-bytes: \d+  "
                     r"resource-errors: 0$", block):
        raise TimeoutError("resource/decode summary was missing or unhealthy")
    if not re.search(r"(?m)^stylesheets: 2  scripts: 7  script-errors: 0$",
                     block):
        raise TimeoutError("CSS/script summary was missing or unhealthy")
    missed = [p for p in paths if counts[p] <= 0]
    if missed:
        raise TimeoutError("browser did not request resources %r" % missed)
    if browser_fixtures.http.path_count(commented) != commented_before:
        raise TimeoutError("module loader fetched an import inside a comment")
    if browser_fixtures.cors.path_count(cors_path) <= cors_before:
        raise TimeoutError("cross-origin module was not requested")
    wait_prompt(h, timeout=NET_TIMEOUT)


def t_browser_module_policy(h):
    """ES modules reject invalid MIME types and cross-origin denial."""
    if not browser_fixtures:
        raise TimeoutError("controlled browser servers were not started")
    bad_mime = "/bad-mime.mjs"
    blocked = "/blocked.mjs"
    bad_before = browser_fixtures.http.path_count(bad_mime)
    blocked_before = browser_fixtures.cors.path_count(blocked)
    end = "BROWSER-MODULE-POLICY-END"
    h.send("/bin/browser -t -v %s; "
           "echo BROWSER-MODULE-POLICY-STATUS-$?; echo %s"
           % (browser_fixtures.module_policy_url, end))
    block = h.capture_until(r"\n%s\n" % end, timeout=NET_TIMEOUT, regex=True)
    if "MODULE-POLICY-PAGE" not in block:
        raise TimeoutError("module policy fixture page did not render")
    for forbidden in ("BAD-MIME-EXECUTED", "CORS-BLOCK-EXECUTED"):
        if forbidden in block:
            raise TimeoutError("blocked module executed: %s" % forbidden)
    if "non-JavaScript MIME type" not in block:
        raise TimeoutError("invalid module MIME rejection was not reported")
    if "cross-origin response did not allow" not in block:
        raise TimeoutError("module CORS rejection was not reported")
    if not re.search(r"(?m)^subresources: 1  resource-bytes: \d+  "
                     r"resource-errors: 2$", block):
        raise TimeoutError("module policy resource summary was unexpected")
    if not re.search(r"(?m)^stylesheets: 0  scripts: 0  script-errors: 2$",
                     block):
        raise TimeoutError("module policy script summary was unexpected")
    statuses = re.findall(
        r"(?m)^BROWSER-MODULE-POLICY-STATUS-(-?\d+)$", block)
    if statuses != ["0"]:
        raise TimeoutError("module policy browser status was %r" % statuses)
    if browser_fixtures.http.path_count(bad_mime) <= bad_before:
        raise TimeoutError("bad-MIME module was not requested")
    if browser_fixtures.cors.path_count(blocked) <= blocked_before:
        raise TimeoutError("CORS-blocked module was not requested")
    wait_prompt(h, timeout=NET_TIMEOUT)


def t_browser_tls_certificate_negative(h):
    """Reject a parseable self-signed TLS 1.3 certificate ten times."""
    if not browser_fixtures:
        raise TimeoutError("controlled browser servers were not started")
    initial_gets = browser_fixtures.https.get_count
    if initial_gets != 0:
        raise TimeoutError(
            "certificate-negative HTTPS handler began with %d GET requests"
            % initial_gets)

    for attempt in range(1, TLS_NEGATIVE_REPETITIONS + 1):
        begin = "BROWSER-TLS-NEG-BEGIN-%02d" % attempt
        end = "BROWSER-TLS-NEG-END-%02d" % attempt
        h.send("echo %s; /bin/browser -t %s; "
               "echo BROWSER-TLS-NEG-STATUS-$?; echo %s"
               % (begin, browser_fixtures.https_url, end))
        block = h.capture_until(
            r"\n%s\n" % re.escape(end),
            timeout=NET_TIMEOUT, regex=True)
        # Capture rather than merely consume the prompt so a late process or
        # kernel fault between the end marker and prompt is also inspected.
        block += h.capture_until(
            SHELL_PROMPT, timeout=NET_TIMEOUT, regex=True)

        begins = list(re.finditer(
            r"(?m)^%s$" % re.escape(begin), block))
        ends = re.findall(r"(?m)^%s$" % re.escape(end), block)
        if len(begins) != 1 or len(ends) != 1:
            raise TimeoutError(
                "certificate-negative attempt %d did not produce one "
                "standalone begin/end marker" % attempt)
        # Inspect the entire capture for safety signatures, including output
        # that arrived just before this attempt's begin marker.  Keep status
        # and diagnostic matching bounded to the unique attempt below.
        if TLS_NEGATIVE_BODY_MARKER in block:
            raise TimeoutError(
                "certificate-negative attempt %d capture contained the "
                "server body" % attempt)
        forbidden = TLS_NEGATIVE_FORBIDDEN_RE.search(block)
        if forbidden:
            raise TimeoutError(
                "certificate-negative attempt %d capture contained "
                "forbidden output %r" % (attempt, forbidden.group(0)))
        attempt_block = block[begins[0].end():]
        diagnostic = _validate_tls_negative_block(attempt_block, attempt)

        current_gets = browser_fixtures.https.get_count
        if current_gets != initial_gets or current_gets != 0:
            raise TimeoutError(
                "certificate-negative HTTPS handler received %d GET "
                "requests by attempt %d" % (current_gets, attempt))
        print("  TLS negative %02d/%02d: [exit 1], "
              "BROWSER-TLS-NEG-STATUS-1, HTTPS GETs=0; %s"
              % (attempt, TLS_NEGATIVE_REPETITIONS, diagnostic))

    # Give a worker released by the final connection a scheduling turn, then
    # take the sequence-wide snapshot used in the acceptance record.
    time.sleep(0.05)
    final_gets = browser_fixtures.https.get_count
    if final_gets != initial_gets or final_gets != 0:
        raise TimeoutError(
            "certificate-negative HTTPS handler GET count changed from "
            "%d to %d" % (initial_gets, final_gets))
    print("  TLS negative summary: %d/%d rejected; HTTPS handler GETs=%d"
          % (TLS_NEGATIVE_REPETITIONS, TLS_NEGATIVE_REPETITIONS,
             final_gets))


def t_browser_https(h):
    """The browser itself must fetch and verify a public HTTPS page.

    This is deliberately not a SKIP-on-network-failure probe: verified
    HTTPS through /bin/browser is a Wave 2 acceptance requirement.  Watch
    for an error as well as the success marker so a readable fast failure
    is reported immediately instead of burning the whole timeout.
    """
    h.send("/bin/browser -t https://example.com/; "
           "echo BROWSER-HTTPS-STATUS-$?")
    block = h.capture_until(
        r"(?:^|\n)BROWSER-HTTPS-STATUS-\d+\n",
        timeout=NET_TIMEOUT, regex=True)
    if "Example Domain" not in block:
        raise TimeoutError("browser HTTPS success marker was absent")
    if not re.search(r"(?:^|\n)BROWSER-HTTPS-STATUS-0\n", block):
        raise TimeoutError("browser HTTPS did not report explicit status 0")
    if re.search(r"(?i)\n(?:browser:|cannot load page|"
                 r"certificate(?: |:)|network unavailable|"
                 r"dns lookup failed|cannot connect)[^\n]*\n", block):
        raise TimeoutError("browser HTTPS output also contained a load error")
    wait_prompt(h, timeout=NET_TIMEOUT)


def t_curl_https(h):
    """The shared libc HTTP client must negotiate and verify HTTPS too."""
    h.send("curl -s https://example.com/; echo CURL-HTTPS-STATUS-$?")
    block = h.capture_until(
        r"(?:^|\n)CURL-HTTPS-STATUS-\d+\n",
        timeout=NET_TIMEOUT, regex=True)
    if "Example Domain" not in block:
        raise TimeoutError("curl HTTPS success marker was absent")
    if not re.search(r"(?:^|\n)CURL-HTTPS-STATUS-0\n", block):
        raise TimeoutError("curl HTTPS did not report explicit status 0")
    if re.search(r"(?i)\ncurl:.*(?:certificate|TLS|connect|resolve)", block):
        raise TimeoutError("curl HTTPS output also contained a load error")
    wait_prompt(h, timeout=NET_TIMEOUT)


def t_tcp_curl(h):
    """A real TCP fetch. SKIPs like the other network tests: the host may
    have no route out, and that is not a KestrelOS bug."""
    ok = r"(?i)</html>"
    h.send("curl -s http://example.com")
    got = h.expect_any([ok,
                        r"curl: network unavailable",
                        r"curl: cannot resolve",
                        r"curl: cannot connect",
                        r"curl: this build has no HTTP client",
                        r"curl: HTTP \d+",
                        r"\[exit \d+\]"],
                       timeout=NET_TIMEOUT, regex=True)
    wait_prompt(h, timeout=NET_TIMEOUT)
    if got != ok:
        return "SKIP"


def t_smp(h):
    h.send("nproc")
    h.expect(r"nproc: [2-9][0-9]* CPUs online "
             r"\([2-9][0-9]* discovered\), running on CPU [0-9]+",
             regex=True)
    wait_prompt(h)


TESTS = [
    ("boot", t_boot),
    ("login", t_login),
    ("shell-prompt", t_prompt),
    ("help", t_help),
    ("echo", t_echo),
    ("ls-bin", t_ls_bin),
    ("cat-motd", t_cat_motd),
    ("fs-roundtrip", t_fs_roundtrip),
    ("ps", t_ps),
    ("free", t_free),
    ("smp", t_smp),
    ("rng", t_rng),
    ("demand-paging", t_demand_paging),
    ("signals", t_signals),
    ("dynamic-linking", t_dynamic_linking),
    ("ping", t_ping),
    ("nslookup", t_nslookup),
    ("uptime", t_uptime),
    ("calc", t_calc),
    ("calc-divzero", t_calc_divzero),
    ("cp-wc", t_cp_wc),
    ("mv-rm", t_mv_rm),
    ("mkdir-ls", t_mkdir_ls),
    ("grep", t_grep),
    ("head", t_head),
    ("tail", t_tail),
    ("tree", t_tree),
    ("du", t_du),
    ("find", t_find),
    ("date", t_date),
    ("err-cat-missing", t_err_cat_missing),
    ("err-rm-missing", t_err_rm_missing),
    ("err-unknown-cmd", t_err_unknown_cmd),
    ("long-line", t_long_line),
    ("whoami", t_whoami),
    ("id", t_id),
    ("redirect", t_redirect),
    ("pipe", t_pipe),
    ("dev", t_dev),
    ("dmesg", t_dmesg),
    ("service-list", t_service_list),
    ("service-lifecycle", t_service_lifecycle),
    ("permissions", t_permissions),
    ("browser-text", t_browser_text),
    ("browser-home", t_browser_home),
    ("browser-http-controlled", t_browser_http_controlled),
    ("browser-web-compat", t_browser_web_compat),
    ("browser-module-policy", t_browser_module_policy),
    ("browser-tls-cert-negative", t_browser_tls_certificate_negative),
    ("browser-https", t_browser_https),
    ("curl-https", t_curl_https),
    ("kpkg-list", t_kpkg_list),
    ("kpkg-install", t_kpkg_install),
    ("pkg-hello", t_pkg_hello),
    ("pkg-cal", t_pkg_cal),
    ("pkg-factor", t_pkg_factor),
    ("kpkg-verify", t_kpkg_verify),
    ("lsmod", t_lsmod),
    ("tcp-curl", t_tcp_curl),
]

# boot + login + a prompt: the least that proves the machine came up.
SMOKE_TESTS = 3


def run_tests(h, tests):
    results = []
    failed = False
    for name, fn in tests:
        if failed:
            results.append((name, "SKIP"))
            continue
        try:
            status = fn(h) or "PASS"
        except TimeoutError as e:
            print("FAIL %s: %s" % (name, e))
            h.print_tail()
            status = "FAIL"
            failed = True
        except (OSError, BrokenPipeError) as e:
            print("FAIL %s: qemu pipe error: %s" % (name, e))
            h.print_tail()
            status = "FAIL"
            failed = True
        print("%s %s" % (status, name))
        results.append((name, status))
    return results


def summarize(results):
    npass = sum(1 for _, s in results if s == "PASS")
    nfail = sum(1 for _, s in results if s == "FAIL")
    nskip = sum(1 for _, s in results if s == "SKIP")
    print("summary: %d passed, %d failed, %d skipped, %d total"
          % (npass, nfail, nskip, len(results)))
    return nfail == 0


def selftest():
    """Exercise expect/send plumbing against a fake python child."""
    fake = (
        "import sys\n"
        "sys.stdout.write('\\x1b[32mKESTREL READY\\x1b[0m\\r\\n')\n"
        "sys.stdout.write('kestrel:/$ ')\n"
        "sys.stdout.flush()\n"
        "for line in sys.stdin:\n"
        "    line = line.strip()\n"
        "    if line.startswith('echo '):\n"
        "        sys.stdout.write(line[5:] + '\\r\\n')\n"
        "    elif line == 'quit':\n"
        "        break\n"
        "    sys.stdout.write('kestrel:/$ ')\n"
        "    sys.stdout.flush()\n"
    )
    h = Harness([sys.executable, "-u", "-c", fake])
    h.start()
    try:
        h.expect("KESTREL READY", timeout=10)
        h.expect("kestrel:/$", timeout=10)
        h.send("echo hello world")
        h.expect("hello world", timeout=10)
        h.expect_any([r"kestrel:[^\n]*\$"], timeout=10, regex=True)
        h.send("echo capture-marker")
        block = h.capture_until(r"capture-marker\n", timeout=10, regex=True)
        if "capture-marker" not in block:
            raise TimeoutError("capture_until lost the matched output")
        h.expect_any([r"kestrel:[^\n]*\$"], timeout=10, regex=True)
        h.send("quit")
    except TimeoutError as e:
        print("FAIL selftest: %s" % e)
        h.print_tail()
        return False
    finally:
        h.kill()

    # A prior consuming assertion can eat the newline shared with the next
    # standalone status line.  The status contract must accept both that
    # buffer-start case and the ordinary newline-delimited case, while
    # rejecting the same marker embedded in the shell's echoed command.
    status = r"(?:^|\n)BROWSER-SELFTEST-STATUS-0\n"

    at_start = Harness([])
    at_start.buf = "preceding-line\nBROWSER-SELFTEST-STATUS-0\n"
    at_start.expect("preceding-line\n", timeout=0)
    at_start.expect(status, timeout=0, regex=True)

    after_newline = Harness([])
    after_newline.buf = "unconsumed-line\nBROWSER-SELFTEST-STATUS-0\n"
    after_newline.expect(status, timeout=0, regex=True)

    echoed = Harness([])
    echoed.buf = (
        "kestrel:/$ echo BROWSER-SELFTEST-STATUS-0\n"
        "kestrel:/$ "
    )
    try:
        echoed.expect(status, timeout=0, regex=True)
    except TimeoutError:
        pass
    else:
        print("FAIL selftest: browser status matched echoed command text")
        return False

    valid_tls_negative = (
        "BROWSER-TLS-NEG-BEGIN-01\n"
        "browser: the certificate is for 'kestrel-negative.invalid', "
        "not '10.0.2.2'\n"
        "[exit 1]\n"
        "BROWSER-TLS-NEG-STATUS-1\n"
        "BROWSER-TLS-NEG-END-01\n"
        "kestrel:/$"
    )
    try:
        _validate_tls_negative_block(valid_tls_negative, 1)
        _validate_tls_negative_block(
            "browser: no trusted root certificate issued 'fixture'\n"
            "[exit 1]\n"
            "BROWSER-TLS-NEG-STATUS-1\n", 2)
    except TimeoutError as e:
        print("FAIL selftest: valid TLS negative transcript rejected: %s" % e)
        return False

    invalid_tls_negative = [
        ("body marker", valid_tls_negative.replace(
            "[exit 1]\n", TLS_NEGATIVE_BODY_MARKER + "\n[exit 1]\n")),
        ("uproc", valid_tls_negative.replace(
            "[exit 1]\n", "uproc: pid 7 terminated\n[exit 1]\n")),
        ("fault", valid_tls_negative.replace(
            "[exit 1]\n", "page fault at 0x18\n[exit 1]\n")),
        ("exception", valid_tls_negative.replace(
            "[exit 1]\n", "exception 14\n[exit 1]\n")),
        ("kernel panic", valid_tls_negative.replace(
            "[exit 1]\n", "kernel panic\n[exit 1]\n")),
        ("exit -1", valid_tls_negative.replace("[exit 1]", "[exit -1]")),
        ("status -1", valid_tls_negative.replace(
            "BROWSER-TLS-NEG-STATUS-1",
            "BROWSER-TLS-NEG-STATUS--1")),
        ("generic no error", valid_tls_negative.replace(
            "[exit 1]\n", "browser: no error\n[exit 1]\n")),
        ("generic certificate text", valid_tls_negative.replace(
            "the certificate is for 'kestrel-negative.invalid', "
            "not '10.0.2.2'",
            "certificate verification failed")),
        ("non-standalone exit", valid_tls_negative.replace(
            "[exit 1]", "shell said [exit 1]")),
        ("wrong positive status", valid_tls_negative.replace(
            "BROWSER-TLS-NEG-STATUS-1",
            "BROWSER-TLS-NEG-STATUS-2")),
    ]
    for name, transcript in invalid_tls_negative:
        try:
            _validate_tls_negative_block(transcript, 99)
        except TimeoutError:
            continue
        print("FAIL selftest: TLS negative validator accepted %s" % name)
        return False
    print("PASS selftest: TLS negative validator accepted 2 valid and "
          "rejected %d invalid transcripts" % len(invalid_tls_negative))

    print("PASS selftest")
    return True


def fixture_selftest():
    """Prove both controlled browser endpoints without booting an image."""
    fixtures = None
    try:
        fixtures = BrowserFixtureServers()
        fixtures.start()
        if fixtures.http.get_count != 0 or fixtures.https.get_count != 0:
            raise RuntimeError("fixture GET counters did not start at zero")

        plain = http.client.HTTPConnection(
            "127.0.0.1", fixtures.http.server_address[1], timeout=5)
        plain.request("GET", "/plain")
        body = plain.getresponse().read().decode("ascii")
        plain.close()
        if HTTP_BODY_MARKER not in body:
            raise RuntimeError("plain HTTP marker missing")
        if fixtures.http.get_count != 1:
            raise RuntimeError("plain HTTP GET count is %d, expected 1"
                               % fixtures.http.get_count)
        if fixtures.https.get_count != 0:
            raise RuntimeError("HTTPS GET count changed before TLS test")

        strict = http.client.HTTPSConnection(
            "127.0.0.1", fixtures.https.server_address[1], timeout=5)
        try:
            strict.request("GET", "/negative")
            strict.getresponse().read()
        except ssl.SSLCertVerificationError:
            pass
        else:
            raise RuntimeError("strict TLS accepted the self-signed cert")
        finally:
            strict.close()
        if fixtures.https.get_count != 0:
            raise RuntimeError(
                "strict certificate rejection reached HTTPS handler "
                "(GET count %d)" % fixtures.https.get_count)

        insecure = http.client.HTTPSConnection(
            "127.0.0.1", fixtures.https.server_address[1], timeout=5,
            context=ssl._create_unverified_context())
        insecure.request("GET", "/negative")
        body = insecure.getresponse().read().decode("ascii")
        insecure.close()
        if TLS_NEGATIVE_BODY_MARKER not in body:
            raise RuntimeError("TLS negative body marker missing")
        if fixtures.https.get_count != 1:
            raise RuntimeError("insecure HTTPS GET count is %d, expected 1"
                               % fixtures.https.get_count)
    except (OSError, RuntimeError, ssl.SSLError) as e:
        print("FAIL fixture-selftest: %s" % e)
        return False
    finally:
        if fixtures:
            fixtures.close()
    print("PASS fixture-selftest: HTTP GETs=1; TLS 1.3 strict rejection "
          "GETs=0; insecure HTTPS GETs=1 and body marker served")
    return True


def main():
    global browser_fixtures

    ap = argparse.ArgumentParser(description="KestrelOS e2e test harness")
    ap.add_argument("--smoke", action="store_true",
                    help="run boot + prompt tests only")
    ap.add_argument("--only", action="append", metavar="NAME",
                    help="run boot/login/prompt plus one named test; repeatable")
    ap.add_argument("--list", action="store_true",
                    help="list tests and exit")
    ap.add_argument("--selftest", action="store_true",
                    help="test harness plumbing without an OS image")
    ap.add_argument("--fixtures-selftest", action="store_true",
                    help="test controlled HTTP/TLS servers without an image")
    ap.add_argument("--nic", choices=sorted(QEMU_NICS), default="rtl8139",
                    help="emulated NIC to test (default: rtl8139)")
    ap.add_argument("--cpus", type=int, default=2,
                    help="virtual CPU count (default: 2)")
    args = ap.parse_args()
    if args.cpus < 1 or args.cpus > 16:
        ap.error("--cpus must be between 1 and 16")

    if args.list:
        for name, _ in TESTS:
            print(name)
        return 0

    if args.selftest:
        return 0 if selftest() else 1
    if args.fixtures_selftest:
        return 0 if fixture_selftest() else 1

    if not os.path.exists("build/os.img"):
        print("error: build/os.img not found (run make first)")
        return 1

    if args.only:
        available = dict(TESTS)
        missing = [name for name in args.only if name not in available]
        if missing:
            ap.error("unknown --only test(s): %s" % ", ".join(missing))
        selected = set(args.only)
        tests = TESTS[:SMOKE_TESTS] + [
            item for item in TESTS[SMOKE_TESTS:] if item[0] in selected]
    else:
        tests = TESTS[:SMOKE_TESTS] if args.smoke else TESTS
    fixtures = None
    if not args.smoke or args.only:
        try:
            fixtures = BrowserFixtureServers()
            fixtures.start()
            browser_fixtures = fixtures
        except (OSError, RuntimeError, ssl.SSLError) as e:
            print("error: cannot start controlled browser fixtures: %s" % e)
            if fixtures:
                fixtures.close()
            return 1

    h = Harness(qemu_command(args.nic, args.cpus))
    try:
        h.start()
    except FileNotFoundError:
        print("error: qemu-system-x86_64 not found in PATH")
        if fixtures:
            fixtures.close()
            browser_fixtures = None
        return 1
    try:
        results = run_tests(h, tests)
    finally:
        h.kill()
        if fixtures:
            fixtures.close()
            browser_fixtures = None
    return 0 if summarize(results) else 1


if __name__ == "__main__":
    sys.exit(main())
