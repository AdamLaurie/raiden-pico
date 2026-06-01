---
name: sync-requirements
description: Keep requirements.txt and requirements-dev.txt in sync with the actual third-party Python imports across the repo. Invoke whenever you add, remove, or move a Python `import` of a non-stdlib package in any .py file — especially before committing changes that touch .py files. The repo's dependency graph on GitHub reads these manifests.
allowed-tools: Bash, Read, Edit
---

Audit third-party Python imports across the repo and reconcile them against `requirements.txt` (runtime) and `requirements-dev.txt` (test-only). Run this BEFORE committing any change that adds/removes/renames a Python `import` of a non-stdlib package.

1. Sweep the repo for third-party imports, excluding build/ and __pycache__:
   ```bash
   find . -path './build*' -prune -o -path '*/__pycache__*' -prune -o -name '*.py' -print \
     | xargs -I{} grep -hE '^(from |import )' {} 2>/dev/null \
     | grep -vE '^(from|import) (\.|os|sys|time|re|json|csv|argparse|datetime|http\.|queue|threading|subprocess|random|math|hashlib|struct|collections|itertools|functools|pathlib|tempfile|signal|select|socket|enum|typing|io|copy|glob|shutil|warnings|traceback|logging|string|binascii|configparser|asyncio|concurrent|inspect|operator|abc|dataclasses|contextlib|html|urllib)' \
     | sort -u
   ```
   Any package name in the output that isn't a known local module (e.g. `glitch_marathon`, `jtag_chipshouter_isp`) is a third-party dep.

2. For each third-party package found:
   - If it's used by `tests/` only → must appear in `requirements-dev.txt`.
   - Otherwise → must appear in `requirements.txt`.
   - Use a sensible `>=` minimum (current installed version is a reasonable floor; verify with `python3 -c "import <pkg>; print(<pkg>.__version__)"`).

3. For each entry in `requirements.txt` / `requirements-dev.txt`:
   - Confirm it still has at least one `import` somewhere outside build/ and __pycache__.
   - If a manifest line is no longer referenced anywhere, remove it.

4. Re-run the sweep after editing to confirm imports and manifests match.

When to invoke:
- BEFORE commit, if the diff touches any `*.py` file with import changes.
- When the user mentions "deps", "requirements", "dependency graph", or "Dependabot".
- After a refactor that moves logic between scripts (imports may move).

When NOT to invoke:
- Pure runtime-data edits (e.g. tweaking constants, parsing) that don't touch import lines.
- C-only changes (`src/*.c`, `include/*.h`, `CMakeLists.txt`).

Notes:
- Keep `requirements.txt` to runtime deps (what a user driving the host scripts needs). Test-only deps stay in `requirements-dev.txt` which already includes `-r requirements.txt`.
- Don't pin to exact versions (`==`); use `>=` so Dependabot can suggest upgrades.
- Don't add stdlib modules — that breaks pip with confusing errors.
- If a script is gated behind `if HAVE_FOO:` or imported optionally inside a function, still add it to the manifest. GitHub's dep graph is for visibility, not strict gating.
