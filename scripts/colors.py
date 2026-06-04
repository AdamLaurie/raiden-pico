#!/usr/bin/env python3
"""
colors.py -- shared ANSI color helpers + a unified colorized argparse help formatter
for the raiden-pico host scripts (nrf_*.py / pic_*.py / rigol_*.py).

Colors auto-disable when stdout is not a TTY, when NO_COLOR is set (https://no-color.org),
or when TERM=dumb -- so piped/redirected output and log files stay clean. Force on with
CLICOLOR_FORCE=1.

Output helpers (semantic = the unified scheme):
    hdr(s)   bold cyan    -- section / banner headers
    ok(s)    bold green   -- success
    warn(s)  yellow       -- warnings
    err(s)   bold red     -- errors
    info(s)  cyan         -- informational
    key(s)   dim          -- a field name
    val(s)   bold         -- a field value
    plus the plain styles: bold/dim/red/green/yellow/cyan/magenta/gray.

Help formatter:
    ColorHelpFormatter -- pass as argparse `formatter_class` for a uniform colored help.

Misc:
    strip(s) -- remove ANSI codes (used when teeing colored output to a file).
"""
import argparse
import os
import re
import sys

_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def _color_enabled():
    force = os.environ.get("CLICOLOR_FORCE")
    if force not in (None, "", "0"):
        return True
    if os.environ.get("NO_COLOR") is not None:
        return False
    if os.environ.get("TERM") == "dumb":
        return False
    try:
        return sys.stdout.isatty()
    except Exception:
        return False


ENABLED = _color_enabled()

_SGR = {
    "reset": "0", "bold": "1", "dim": "2", "italic": "3", "underline": "4",
    "red": "31", "green": "32", "yellow": "33", "blue": "34",
    "magenta": "35", "cyan": "36", "white": "37", "gray": "90",
}


def style(text, *names):
    """Wrap `text` in the given SGR styles (no-op when colors are disabled)."""
    if not ENABLED or not names:
        return text
    codes = ";".join(_SGR[n] for n in names if n in _SGR)
    return f"\033[{codes}m{text}\033[0m" if codes else text


def strip(text):
    """Remove ANSI SGR codes from `text` (for writing colored output to a file)."""
    return _ANSI_RE.sub("", text)


# --- plain styles ---
def bold(s):    return style(s, "bold")
def dim(s):     return style(s, "dim")
def red(s):     return style(s, "red")
def green(s):   return style(s, "green")
def yellow(s):  return style(s, "yellow")
def cyan(s):    return style(s, "cyan")
def magenta(s): return style(s, "magenta")
def gray(s):    return style(s, "gray")

# --- semantic (the unified scheme) ---
def hdr(s):  return style(s, "bold", "cyan")
def ok(s):   return style(s, "bold", "green")
def warn(s): return style(s, "yellow")
def err(s):  return style(s, "bold", "red")
def info(s): return style(s, "cyan")
def key(s):  return style(s, "dim")
def val(s):  return style(s, "bold")


# --- unified argparse help formatter ---
# Post-process the rendered help so column widths (computed by argparse on PLAIN text)
# stay correct -- ANSI is only injected afterwards. Extends RawDescriptionHelpFormatter
# so module docstrings / examples keep their formatting.
_OPT_RE = re.compile(r"(?<![\w-])(--?[A-Za-z][\w-]*)")     # -x / --long-opt tokens
_HDR_RE = re.compile(r"^([A-Za-z][\w /+-]*:)\s*$")          # "options:", "Examples:", ...


class ColorHelpFormatter(argparse.RawDescriptionHelpFormatter):
    """Uniform colored argparse help: bold `usage:`, bold-yellow section/headers,
    cyan option flags. Falls back to plain when colors are disabled."""
    def format_help(self):
        text = super().format_help()
        if not ENABLED:
            return text
        out = []
        for line in text.split("\n"):
            m = _HDR_RE.match(line)
            if m:                                   # a section / docstring header line
                out.append(style(line, "bold", "yellow"))
                continue
            if line.startswith("usage:"):
                line = style("usage:", "bold") + line[len("usage:"):]
            line = _OPT_RE.sub(lambda mm: style(mm.group(1), "cyan"), line)
            out.append(line)
        return "\n".join(out)
