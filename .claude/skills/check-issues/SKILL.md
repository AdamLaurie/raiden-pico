---
name: check-issues
description: Fetch open GitHub issues for the current repo so we catch problems users have reported. Use at least once per working day, ideally at the start of a session, and again when the user mentions "issues", "bugs", "reports", or asks "anything new?".
allowed-tools: Bash, Read
---

Check for open and recently-updated GitHub issues on this repo so nothing user-reported slips by.

1. List open issues, newest first, with key fields:
   ```bash
   gh issue list --repo AdamLaurie/raiden-pico --state open --limit 20 \
     --json number,title,author,createdAt,updatedAt,labels,comments
   ```
2. If any issue is unfamiliar (not previously discussed in this conversation), fetch its body with `gh issue view <N> --repo AdamLaurie/raiden-pico --json title,author,body,comments` so we have the full context.
3. Summarize what's new to the user in one tight block:
   - **NEW since last check**: number, title, author, one-line gist
   - **STILL OPEN**: just the count, unless something escalated (new comments)
4. Suggest a concrete next action for anything build-breaking or blocking (e.g. "issue #3 reports a missing header — should we patch and close it?").

When to use:
- Proactively at session start if it's been more than a working day since the last check.
- When the user references issues, bug reports, or pull requests.
- Before opening a PR (so we don't ship something that conflicts with an open report).
- Whenever the user asks "anything new?", "anything reported?", or similar.

When NOT to use:
- Mid-task when interrupting flow would be unhelpful — wait for a natural pause.
- If we've already checked in the current session and no time-relevant trigger has occurred.

Notes:
- The `gh` CLI is authenticated for this machine. If it errors with auth, surface that to the user rather than retrying.
- Do not auto-close, auto-comment, or auto-react to issues. Reporting only — actions require user confirmation.
