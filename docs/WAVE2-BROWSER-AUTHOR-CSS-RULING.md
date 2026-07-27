# Wave 2 author-CSS acceptance ruling

Status: acceptance-blocking QA assertion correction; no browser or fixture
change is authorized by this failure.

## Evidence

- `rootfs/doc/test.html` places the visible `CSS-AUTHOR-OK` proof panel before
  the later `RENDERER-OK` paragraph.
- `tools/e2e.py::t_browser_text` consumes serial output forward but currently
  waits for `RENDERER-OK` and then the earlier `CSS-AUTHOR-OK`.
- The target browser rendered the complete local page and links and reported
  explicit status 0. The marker was therefore consumed before QA asked for it;
  it was not absent from browser output.

This is a QA assertion-order defect, not a Frontend rendering defect, Backend
CSS defect, or Lead fixture defect.

## Intended observables

Text and GUI prove different parts of the shared pipeline:

- **Text mode** proves that the styled DOM reaches layout and remains readable.
  It must emit, in document order, the title, `CSS-AUTHOR-OK`,
  `RENDERER-OK`, the remaining content/link markers, and explicit exit status
  0. Terminal text cannot prove colors, borders, backgrounds, or padding.
- **GUI mode** proves author presentation. At 900x620 or larger, the
  `/doc/test.html` screenshot must visibly show the `CSS-AUTHOR-OK` text in its
  light-green panel with green border and padding, distinct from the light-blue
  body, plus the author-styled heading/table treatment. A plain text marker
  without that presentation is not GUI CSS proof.

## Ownership and done criterion

- **QA owns only `tools/e2e.py::t_browser_text`:** move the
  `CSS-AUTHOR-OK` expectation before `RENDERER-OK` so forward expectations
  mirror `rootfs/doc/test.html`. Preserve all later content, link-resolution,
  and `BROWSER-LOCAL-STATUS-0` assertions.
- **QA owns GUI evidence:** capture and retain the wide styled-page screenshot
  described above as the author-CSS artifact.
- **Lead changes no rootfs file:** `rootfs/doc/test.html` already contains the
  stable marker and unambiguous embedded author styles.
- **Frontend and Backend change no production code** for this failure.

The issue is done when the focused local browser test passes with the corrected
document order, the GUI artifact demonstrates the author styles, and the
current full E2E acceptance run has zero failures.
