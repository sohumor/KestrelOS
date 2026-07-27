# Wave 2 browser status-assertion ruling

Status: QA-only acceptance assertion defect. No browser, library, or fixture
change is authorized.

## Evidence

`Harness.clean()` removes carriage returns and leaves normalized `\n`.
`Harness.expect()` then consumes the buffer through the end of every match.
In `t_browser_text`, the second resolved-link regex includes its trailing
newline. That newline is also the separator before the shell's next standalone
`BROWSER-LOCAL-STATUS-0` line. After the link match, the unconsumed buffer
therefore starts with the status marker and has no leading newline.

Serial showed the complete content, both links, and
`BROWSER-LOCAL-STATUS-0`. Requiring the already-consumed leading `\n` caused
the timeout; the application did not omit or misreport its exit status.

## Matching contract

Every browser status marker remains a unique standalone shell-output line.
Match it after newline normalization with:

```text
(?:^|\n)BROWSER-<CASE>-STATUS-<EXPECTED>\n
```

The `^` branch handles a marker at the start of the current unconsumed harness
buffer after a previous assertion consumed the shared newline. The `\n` branch
handles a marker later in a captured block. This remains resistant to false
positives: the serially echoed command contains `echo BROWSER-...`, so the
marker is not at the start of that line and satisfies neither branch.

Do not require a leading newline unconditionally, use an unanchored marker
substring, or change global newline cleaning to accommodate one test.
End-sentinel block capture remains valid; status checks within those blocks
must use the same start-or-newline boundary.

## Ownership and done criterion

- **QA owns `tools/e2e.py` only.** Apply the boundary contract consistently to
  local, home, controlled HTTP, certificate-negative TLS, and public HTTPS
  status assertions/captures. Preserve expected zero versus nonzero status and
  every body/error assertion.
- **QA owns a harness self-test** in the same file: consume a preceding line
  including its trailing newline, prove a standalone status at buffer start
  matches, and prove the same marker embedded in an echoed command does not.
- **Frontend, Backend, Lead, and rootfs fixtures require no change.**

The issue is done when the harness self-test passes, focused local/home and all
network browser status cases report their explicit expected exit codes, and
the current full E2E acceptance run has zero failures.
