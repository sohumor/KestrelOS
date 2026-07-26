/* ua_style.c - the user-agent stylesheet.
 *
 * This is the default rendering of HTML: what a document looks like with
 * no author stylesheet at all. It is written as CSS source and parsed by
 * our own parser at startup rather than being built as a table of
 * struct computed_style, for two reasons. It is far smaller in both
 * source and object size, and it is the most demanding single test the
 * parser gets: if the cascade cannot make <h1> bold and 2em and give
 * <blockquote> its 40px indents, nothing else is going to work either.
 *
 * It follows the HTML Standard's rendering section and the CSS 2.1
 * appendix, trimmed to the properties this engine actually has. What is
 * deliberately absent: generated content (::before/::after, quotes,
 * counters), which the layout engine cannot render, and anything that
 * needs alpha compositing, which the framebuffer does not have.
 *
 * Colours are the browser defaults - black on white, blue links - not
 * the desktop's dark theme. A page is a document, not a dialog.
 */

static const char ua_css[] =

/* ---- the document ---- */
"html { display: block; color: black; background-color: white;"
"       font-family: serif; font-size: 16px; line-height: normal }\n"
"body { display: block; margin: 8px }\n"

/* ---- block level ---- */
"address, article, aside, blockquote, center, dd, details, dialog, dir,"
"div, dl, dt, fieldset, figcaption, figure, footer, form, frameset,"
"h1, h2, h3, h4, h5, h6, header, hgroup, hr, legend, listing, main,"
"menu, nav, ol, p, plaintext, pre, section, summary, ul, xmp"
"  { display: block }\n"

/* ---- not rendered ---- */
"area, base, basefont, datalist, head, link, meta, noembed, noframes,"
"param, rp, script, style, template, title"
"  { display: none }\n"

/* ---- headings ---- */
"h1 { font-size: 2em;    font-weight: bold; margin: 0.67em 0 }\n"
"h2 { font-size: 1.5em;  font-weight: bold; margin: 0.83em 0 }\n"
"h3 { font-size: 1.17em; font-weight: bold; margin: 1em 0 }\n"
"h4 { font-size: 1em;    font-weight: bold; margin: 1.33em 0 }\n"
"h5 { font-size: 0.83em; font-weight: bold; margin: 1.67em 0 }\n"
"h6 { font-size: 0.67em; font-weight: bold; margin: 2.33em 0 }\n"

/* ---- paragraphs and grouping ---- */
"p { margin: 1em 0 }\n"
"blockquote, figure { margin: 1em 40px }\n"
"dl { margin: 1em 0 }\n"
"dd { margin-left: 40px }\n"
"pre, listing, plaintext, xmp"
"  { font-family: monospace; white-space: pre; margin: 1em 0 }\n"
"hr { display: block; margin: 0.5em 0; border-top: 1px solid #808080;"
"     overflow: hidden }\n"
"fieldset { margin: 0 2px; padding: 0.35em 0.75em 0.625em;"
"           border: 2px groove #c0c0c0 }\n"
"legend { padding: 0 2px }\n"

/* ---- lists ---- */
"ul, menu, dir { list-style-type: disc; margin: 1em 0; padding-left: 40px }\n"
"ol { list-style-type: decimal; margin: 1em 0; padding-left: 40px }\n"
"li { display: list-item }\n"
"ul ul, ol ul, menu ul { list-style-type: circle }\n"
"ul ul ul, ul ol ul, ol ul ul, ol ol ul { list-style-type: square }\n"
"ol ol, ul ol, menu ol { list-style-type: lower-alpha }\n"
"ol ol ol, ul ul ol, ul ol ol, ol ul ol { list-style-type: lower-roman }\n"
"ul ul, ul ol, ol ul, ol ol, ul dl, ol dl, dl ul, dl ol"
"  { margin-top: 0; margin-bottom: 0 }\n"

/* ---- inline emphasis ---- */
"b, strong { font-weight: bold }\n"
"i, em, cite, var, dfn, address, caption { font-style: italic }\n"
"code, kbd, samp, tt { font-family: monospace }\n"
"u, ins { text-decoration: underline }\n"
"s, strike, del { text-decoration: line-through }\n"
"big { font-size: larger }\n"
"small { font-size: smaller }\n"
"sub { vertical-align: sub; font-size: smaller }\n"
"sup { vertical-align: super; font-size: smaller }\n"
"mark { background-color: #ffff00; color: black }\n"
"abbr, acronym { text-decoration: none }\n"
"nobr { white-space: nowrap }\n"
"center { text-align: center }\n"
"q { font-style: normal }\n"

/* ---- links ---- */
"a[href] { color: #0000ee; text-decoration: underline }\n"
"a[href]:visited { color: #551a8b }\n"
"a[href]:hover { color: #ee0000 }\n"

/* ---- tables ---- */
"table { display: table; border-collapse: separate; border-spacing: 2px;"
"        border-color: #808080 }\n"
"thead { display: table-header-group }\n"
"tbody { display: table-row-group }\n"
"tfoot { display: table-footer-group }\n"
"col { display: table-column }\n"
"colgroup { display: table-column-group }\n"
"tr { display: table-row; vertical-align: inherit }\n"
"td { display: table-cell; vertical-align: inherit; padding: 1px }\n"
"th { display: table-cell; vertical-align: inherit; padding: 1px;"
"     font-weight: bold; text-align: center }\n"
"caption { display: table-caption; text-align: center; font-style: normal }\n"

/* ---- replaced and form controls ---- */
"img, iframe, embed, object, canvas, video, svg { display: inline-block }\n"
"input, select, textarea, button"
"  { display: inline-block; font-size: 13px; padding: 1px 2px;"
"    border: 1px solid #767676; background-color: #ffffff; color: black }\n"
"button, input[type=submit], input[type=reset], input[type=button]"
"  { text-align: center; background-color: #efefef;"
"    border: 2px outset #c0c0c0 }\n"
"textarea { font-family: monospace; white-space: pre-wrap;"
"           vertical-align: text-bottom }\n"
"input[type=hidden] { display: none }\n"
"select { background-color: #ffffff }\n"
"optgroup { font-style: italic; font-weight: bold }\n"
"option { display: block }\n"
"label { cursor: default }\n"

/* ---- print-only rules the media evaluator must exclude on screen ---- */
"@media print {\n"
"  body { margin: 0 }\n"
"  a[href] { color: black; text-decoration: none }\n"
"  nav, aside, form { display: none }\n"
"}\n"

/* ---- hidden regardless ---- */
"[hidden] { display: none }\n"
"[disabled] { color: #808080 }\n";

const char *css_ua_stylesheet(void)
{
    return ua_css;
}

unsigned long css_ua_stylesheet_len(void)
{
    return sizeof(ua_css) - 1;
}
