# The Kestrel Browser

Chrome is not going to run here, so KestrelOS has its own browser. It is
built out of four pieces, only the last of which needs a screen:

| file | what it is |
|---|---|
| `apps/html.h`, `apps/html.c` | HTML subset parser + layout engine. No GUI, no kernel, no syscalls. Compiles for the host. |
| `apps/browser.c` | the application: text renderer, graphical renderer, URL handling, history, error pages |
| `apps/curl.c` | fetch a URL to stdout or a file |
| `apps/netstat.c`, `apps/telnet.c` | network diagnostics |

The split matters: everything hard (tokenising, error recovery, word
wrap) lives in `html.c`, which can be compiled and unit-tested on a
development machine in a second, instead of only inside a booted VM.

---

## 1. The HTML engine

### 1.1 Pipeline

```
bytes ──html_parse()──▶ struct html_doc ──html_layout_build()──▶ struct html_layout
                        (flat list of boxes)                     (positioned runs
                                                                  grouped into lines)
```

`html_parse` produces a **flat** list, not a tree. A document is a
sequence of boxes, each carrying the inline state that applies to it:

```c
struct html_box {
    int kind;             /* HB_TEXT HB_BREAK HB_BLOCK HB_RULE HB_BULLET HB_IMAGE */
    unsigned int style;   /* HS_BOLD HS_ITALIC HS_UNDER HS_MONO HS_LINK HS_PRE */
    int heading;          /* 0, or 1..6 inside h1..h6 */
    int indent;           /* list / blockquote nesting depth */
    const char *text;
    const char *href;     /* set when style & HS_LINK */
};
```

The tree only ever exists as the parser's element stack (64 deep), which
holds the saved inline state. Once an element closes, its contribution is
already baked into the boxes it produced, so nothing downstream needs to
walk a DOM. That is why the layout pass is a single forward loop.

### 1.2 What is supported

Elements: `h1`–`h6`, `p`, `br`, `hr`, `a`, `b`/`strong`, `i`/`em`/`cite`/
`var`, `u`/`ins`, `s`/`strike`/`del`, `ul`/`ol`/`menu`, `li`, `pre`,
`code`/`tt`/`kbd`/`samp`, `blockquote`, `div`, `span`, `table`/`thead`/
`tbody`/`tfoot`/`tr`/`td`/`th`/`caption`, `img`, `dl`/`dt`/`dd`, `title`,
and the HTML5 wrappers `section article header footer nav main aside
figure figcaption form fieldset address center details summary`.

Void elements (`br hr img meta link base input col area source track embed
param wbr`) are never pushed on the stack, so a missing `/>` costs
nothing.

Syntax handled: attributes with double-quoted, single-quoted and unquoted
values; valueless attributes; `<!-- comments -->`; `<!DOCTYPE ...>`;
processing instructions; self-closing tags; and entities — `&amp; &lt;
&gt; &quot; &apos; &nbsp;`, `&#NN;`, `&#xHH;`, about forty named
entities, and the Latin-1 letters folded to their unaccented ASCII base
(`&eacute;` → `e`, `&szlig;` → `ss`). Entity names are matched case
sensitively first, then case insensitively, so both `&Eacute;` and the
legacy `&AMP;` behave.

Everything folds to US-ASCII because both fonts (the VGA console and the
8x16 bitmap font in `kernel/include/font.h`) are ASCII-only. A code point
with no lookalike renders as `?`. This is a deliberate, documented loss,
not an oversight.

### 1.3 Error recovery

Real pages are broken, so the parser assumes they are:

* **Unclosed `<p>` and `<li>`.** Opening any block-level element closes an
  open `<p>`; opening `<li>` closes an open `<li>`. These implied end tags
  are *scoped*: the search stops at the enclosing container, so
  `<ul><li>a<ul><li>b</ul></ul>` does not let the inner `<li>` close the
  outer one and take the nested list with it.
* **Stray closing tags.** `</div>` with no open `<div>` is dropped.
  A close tag that matches something further down the stack pops
  everything above it, which is what makes `<b>bold<p>text</b>` terminate
  sensibly.
* **Unknown elements** are transparent: the tag disappears, the content
  stays. `<blink>kept</blink>` renders "kept".
* **`<script>` and `<style>`** are raw-text elements: their content is
  scanned for the matching close tag and thrown away, so a JavaScript
  string containing `<p>` cannot leak into the page.
* **Malformed attributes** (`<a "junk" href=>`, an opening quote with no
  closing quote, `=` with nothing after it) always advance the cursor, so
  no input can wedge the tokeniser.
* **Limits.** Input is capped at `HTML_MAX_INPUT` (1 MiB), boxes at
  `HTML_MAX_BOXES` (200000), element nesting at 64. Hitting any of them
  sets `html_doc.truncated` instead of failing or growing without bound.

### 1.4 What is deliberately not supported

* **CSS.** Not parsed, not applied. `<style>` content is discarded.
* **JavaScript.** Not executed. `<script>` content is discarded.
* **Real table layout.** Tables degrade to one line per row, cells joined
  with ` | `, `<th>` in bold. There is no column measuring, no
  `colspan`/`rowspan`, and nested tables confuse the cell separator. If
  you need a table to line up, this is the wrong browser.
* **Images.** `<img>` becomes an outlined placeholder holding the `alt`
  text (falling back to `src`, then to "image"). Nothing is decoded.
* **Forms.** `<input>`, `<button>` and `<select>` render as nothing;
  `<form>` is just a block.
* **Floats, positioning, columns, frames, iframes.** No.
* **In-page anchors.** `#fragment` is stripped from URLs and a link to
  one reloads the same page.
* **HTTPS.** TLS 1.3 is supported with certificate-chain, hostname,
  validity-period and signature verification. TLS 1.2 and client
  certificates are not implemented.
* **Non-breaking space semantics.** `&nbsp;` decodes to a real space
  that does not collapse with its neighbours (so runs of them still
  indent) but it *does* allow a line break.

### 1.5 Layout

`html_layout_build(doc, width, metrics)` walks the boxes once and emits
positioned runs grouped into lines. All font knowledge is in the caller's
metric callbacks:

```c
struct html_metrics {
    int (*text_w)(void *ctx, const char *s, int len, unsigned style, int heading);
    int (*line_h)(void *ctx, unsigned style, int heading);
    void *ctx;
    int indent_w;   /* width of one indent level */
    int para_gap;   /* extra space at a block boundary */
};
```

`html_metrics_chars()` fills this in for a character-cell device (1 unit
per character, 1 per line, 2 per indent). `browser.c` supplies a pixel
version (8 wide, 18–26 tall depending on heading level). The same layout
code therefore drives the serial console and the framebuffer, and the
text dump is a true preview of the graphical rendering.

Behaviour worth knowing:

* Words wrap at spaces; a word longer than the line is hard-split rather
  than overflowing.
* `<pre>` never wraps at spaces but *is* hard-split at the width, so no
  content is ever lost off the right edge. Newlines inside `<pre>` are
  turned into `HB_BREAK` boxes by the parser, so the layout never has to
  look for one.
* List items get a hanging indent: continuation lines align under the
  item text, not under the bullet.
* Runs are bottom-aligned within their line, so mixed-height runs sit on
  a common baseline.
* `html_layout_hit(layout, x, y)` returns the run under a point. That one
  function is the whole of link hit-testing.

---

## 2. `browser`

```
browser [-t] [-w cols] [-l] [-v] <url|file>
```

| flag | meaning |
|---|---|
| `-t` | text mode: render to stdout and exit |
| `-w N` | text width in columns (default 78, max 200) |
| `-l` | after the page, list every distinct link, resolved to an absolute URL |
| `-v` | print url, status, byte count and title first |

### 2.1 What it can load

* `http://host[:port]/path` through `http_get()` in `libc/http.c`.
* `https://host[:port]/path` through the same client and `libtls`;
  port 443 is the default and verification is mandatory.
* `file:///path`, an absolute path, or a path relative to the shell's cwd
  (the `--cwd=` argument is honoured, as in `cat`). A path that exists
  wins over the "it must be a hostname" guess, so `browser -t index.html`
  does what you expect.
* Anything that is not HTML is rendered as plain text: the body is
  escaped, wrapped in `<pre>` and run through the same engine. Bytes
  outside printable ASCII become `.`, so `browser -t /bin/ls` cannot
  scramble the terminal.

Content type is decided by sniffing, because `http_get()` returns only
the body: a leading `<` followed by a name character, or one of about
fifteen markers (`<html`, `<!doctype`, `<p>`, `<table`, …) in the first
kilobyte.

### 2.2 Failure modes

Every one of these prints a sentence and exits non-zero in text mode, and
becomes a rendered error page in the window. None of them can crash the
browser.

| situation | message |
|---|---|
| no NIC | `network unavailable: no NIC is configured` |
| DNS failure | `DNS lookup failed for "host"` — the name is resolved *before* `http_get()` runs, so this is distinguishable from a connect failure |
| refused / timed out / reset | `cannot connect to host (a.b.c.d) port 80 - refused, timed out or reset` |
| non-2xx status | a `HTTP nnn` banner above the body, which is still rendered |
| body is not HTML | rendered as plain text |
| page over 1 MiB | truncated, with `[page truncated at 1048576 bytes]` |
| too many elements / too deeply nested | `[part of the page was dropped: ...]` |
| missing file, a directory, unreadable file | named explicitly |
| TLS certificate or handshake failure | names the verification or protocol failure; plaintext fallback is never attempted |
| `ftp://` and other network schemes | `unsupported scheme "x" (only http, https and file)` |
| `mailto:`, `javascript:`, `tel:`, `data:` links | refused with the link text shown, never followed |

### 2.3 The window

Requires a framebuffer (`SYS_FBINFO`) and `libgui`. Layout:

```
+--------------------------------------------------------------+
| [Back] [Fwd] [Reload]  [ http://example.com/____________ ]    |  toolbar
+-----------------------------------------------------------+--+
|                                                           |  |
|  content: laid out runs, scrolled vertically              |sb|
|                                                           |  |
+-----------------------------------------------------------+--+
| page title  -  1234 bytes                                    |  status
+--------------------------------------------------------------+
```

* **Links** are blue and underlined. A click is turned into a layout
  coordinate, hit-tested with `html_layout_hit()`, resolved against the
  current URL and loaded.
* **History** is a stack of up to 64 URLs with a cursor; a new navigation
  discards anything Forward pointed at. Back/Forward do not re-push.
* **Keyboard**: arrows and PgUp/PgDn/Home/End and space scroll;
  `ctrl-L` focuses the address bar (then arrows, Home/End, Backspace,
  Delete and ctrl-U edit it, Enter navigates, ESC cancels); `ctrl-R`
  reloads; `ctrl-B` back; `ctrl-F` forward; `ctrl-Q` quits.
* **Status line** shows the title and size, or the error, in red.

Honest limitations of the graphical mode:

* The 8x16 bitmap font has exactly one size and one weight. **Headings
  are not bigger**; they get bold (a second draw pass one pixel to the
  right), extra leading, and that is all that is available without a
  scaled font. Italic is approximated with a slightly different colour.
* Only lines that fit *entirely* inside the viewport are drawn, because
  there is no clip rectangle in the toolkit; scrolling is therefore
  line-granular at the edges.
* The window is a fixed 900x620 and `KEV_RESIZE` is ignored.
* Loading is synchronous: the window shows "loading" and stops responding
  until the fetch returns.

### 2.4 Testing it

`-t` is the interface the automated tests use, because it needs neither a
framebuffer nor a network:

```
browser -t /doc/test.html
browser -t -w 40 /doc/test.html      # a different wrap width
browser -t -l -v http://example.com/ # links and metadata
```

Host-side, `apps/html.c` compiles with the system compiler. The unit test
(see "Integration" below) renders a set of deliberately awkward documents
— nested lists, unclosed tags, entity soup, a table, a `<script>` block
containing markup, malformed attributes — plus a 1.2 MB generated page
and 20000 mutated documents. It runs clean under
`-fsanitize=address,undefined`.

---

## 3. `curl`

```
curl [-i] [-s] [-o <file>] <url>
```

Body to stdout, or to a file with `-o`. `-s` silences everything except
the body. Exit status is 0 for an HTTP 2xx and 1 for anything else, which
makes it usable in a shell test.

`-i` prints a **reconstructed** header block:

```
HTTP/1.1 200
Content-Length: 4096
X-Kestrel-Note: headers reconstructed by curl; http_get() returns only body/length/status
```

Those are not the bytes the server sent. `http_get()` exposes only the
body, its length and the status code, so the real headers are discarded
inside `libc/http.c` before `curl` ever sees them. To print them
verbatim, `libc/include/http.h` would need something like:

```c
int http_get_full(const char *url, char **hdrs, unsigned long *hdrs_len,
                  char **body, unsigned long *body_len, int *status);
```

Until then the note stays in the output so nobody parses it as genuine.

---

## 4. `netstat`

```
netstat [-i] [-r] [-p] [-a]
```

Shows what `SYS_NETINFO` provides: MAC, link state, address, netmask (and
the prefix length, or "non-contiguous" if the mask is not a valid
prefix), gateway, nameserver, and the derived network and broadcast
addresses. `-r` prints the routing table implied by that configuration
(the on-link route and the default route). `-p` pings the gateway and the
nameserver.

`-a` additionally prints what the kernel **does not** export, because the
alternative would be inventing it. `abi/kestrel_abi.h` has exactly one
network-state syscall, `SYS_NETINFO`; the ARP cache in `kernel/net.c`,
the TCP connection table in `kernel/tcp.c` (a fixed array of `TCP_CONNS`
slots) and the UDP port bindings in `kernel/udp.c` are all invisible from
ring 3. Adding them means new syscalls in the `SYS_PSINFO` shape:

```c
SYS_ARPINFO(index, struct k_arpent *)  /* ip, mac, age  -> 0 / -1 at end */
SYS_TCPINFO(index, struct k_tcpinfo *) /* state, local_port, remote_ip,
                                          remote_port, rx_queued, tx_queued */
```

Neither is in the ABI, so `netstat` says so instead of printing zeros.

---

## 5. `telnet`

```
telnet [-n] <host> <port>
```

A raw TCP client, not the telnet protocol — no option negotiation, no IAC
escaping. It exists to test `kernel/tcp.c` by hand:

```
kestrel:/$ telnet example.com 80
connecting to example.com (93.184.216.34) port 80 ...
connected. ctrl-D or "quit" to close.
GET / HTTP/1.0
Host: example.com

HTTP/1.1 200 OK
...
```

Lines are sent with CRLF (`-n` for LF only). Input is echoed locally
because the kernel has no line discipline; Backspace and ctrl-U edit the
line. ctrl-D on an empty line or the word `quit` closes the connection.
Received bytes outside printable ASCII print as `.` so a binary protocol
cannot scramble the console. The loop polls `SYS_TCP_RECV` with a 60 ms
timeout and `read_nb()` on the keyboard, so incoming data still appears
while a line is half-typed.

There are no libc wrappers for the TCP syscalls, so `telnet` calls
`syscall(SYS_TCP_CONNECT, ...)` and friends directly.

---

## 6. Integration

### 6.1 The Makefile needs one change

`apps/html.c` is a **library**, not a program. The rule

```make
APP_NAMES := $(patsubst apps/%.c,%,$(wildcard apps/*.c))
```

would try to link `build/apps/html`, which has no `main`, and
`build/apps/browser` would be missing the engine. Both are fixed by
filtering `html` out of `APP_NAMES` and adding its object to the browser:

```make
APP_NAMES := $(filter-out html,$(patsubst apps/%.c,%,$(wildcard apps/*.c)))

$(BUILD)/apps/browser: $(BUILD)/apps/html.o
$(BUILD)/apps/browser: LDEXTRA := $(BUILD)/apps/html.o
```

or, if the generic app rule is left alone, by giving `browser` its own
link rule that includes `$(BUILD)/apps/html.o`.

### 6.2 `libc/include/http.h`

`browser.c` and `curl.c` call exactly one function:

```c
int http_get(const char *url, char **body, unsigned long *len, int *status);
```

expected to return 0 on success, non-zero on transport failure, and to
hand back a `malloc`ed body the caller frees. Both files guard the
include with `__has_include(<http.h>)`, so they still build (and still
render local files) if the header is absent.

### 6.3 `libgui/gui.h`

`browser.c` uses only these primitives, through a ten-function adapter at
the top of its graphical section:

```c
typedef struct gui_win gui_win;
gui_win *gui_open(const char *title, int w, int h);
void gui_close(gui_win *w);
void gui_flush(gui_win *w);
int  gui_next_event(gui_win *w, struct k_event *ev, int timeout_ms); /* 1/0/-1 */
void gui_clear(gui_win *w, unsigned int color);
void gui_rect (gui_win *w, int x, int y, int cw, int ch, unsigned int color);
void gui_frame(gui_win *w, int x, int y, int cw, int ch, unsigned int color);
void gui_line (gui_win *w, int x0, int y0, int x1, int y1, unsigned int color);
void gui_text (gui_win *w, int x, int y, const char *s, unsigned int color);
int  gui_text_w(const char *s);
```

The buttons, address field and scrollbar are drawn from those primitives
rather than from `gui_button`/`gui_textbox`/`gui_scrollbar`, so the
browser depends on the smallest, most stable part of the toolkit. If the
widget set is preferred, only `ui_button` and the address-field block in
`draw()` need replacing. `-Igui` (or wherever `gui.h` lands) must be
added to `UCFLAGS`; without it the browser builds text-only and says so.
