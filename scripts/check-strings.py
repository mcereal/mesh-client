#!/usr/bin/env python3
"""Fail when a screen spells out an English sentence instead of naming a catalog id.

Every word the user reads comes from include/mesh/i18n/catalog.def (see docs/i18n.md). That is
only true for as long as nobody adds a `"Not connected"` back into a renderer, and the compiler
has no opinion about it - a string literal is a string literal. This is the check that does.

It reads the files that draw or announce something, and reports any string literal that looks
like prose: three or more letters in a row, outside a comment, outside a log call. Everything a
renderer legitimately spells out - a printf glue string, a path, an environment variable, a
protocol token - is either not prose or is listed in ALLOWED below, with the reason.

Run it directly, or through `ctest` / `make test`, which is where it will catch somebody.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The files whose strings reach a screen. A file not listed here is not exempt from the rule;
# it is a file with no user-facing text in it, and adding text to one means adding it here.
#
# The transports are deliberately absent, and are not an exception: everything they say to a
# person goes out through mesh_ble_set_error() / mesh_serial_set_error(), whose first parameter
# is an `enum mesh_str_id`. A string literal there does not compile, which is a stronger check
# than this one - and the rest of those files is D-Bus paths and BlueZ diagnostics, which this
# script would only be able to tell apart from prose with a very long ALLOWED list.
CHECKED = [
    "src/ui/actions.c",
    "src/ui/chrome.c",
    "src/ui/help.c",
    "src/ui/layout.c",
    "src/ui/nav.c",
    "src/ui/nav_canned.c",
    "src/ui/nav_conversations.c",
    "src/ui/nav_keyboard.c",
    "src/ui/nav_settings.c",
    "src/ui/nav_waypoints.c",
    "src/ui/node_detail.c",
    "src/ui/waypoints.c",
    "src/ui/settings.c",
    "src/ui/settings_rows.c",
    "src/ui/backends/fb_draw.c",
    "src/ui/backends/fb_screens.c",
    "src/ui/backends/fb_widgets.c",
    "src/ui/input.c",
    "src/core/app_actions.c",
    "src/core/app_publish.c",
    "src/core/app_settings.c",
]

# Literals that are not prose even though they read like it, each with the reason it stays.
ALLOWED = {
    # Wire and file formats, which are the same in every language.
    '"%08x"': "a node number in the Meshtastic apps' own hex form",
    # printf glue: a renderer composing two already-translated halves.
    '"%s"': "printf glue",
    '"%s%s"': "printf glue",
    '"%s%s %s"': "printf glue",
    '"%s%s%s%s"': "printf glue",
    '"%s%s%u"': "printf glue",
    '"%s_"': "the draft's cursor",
    '"%02x"': "one byte of a key fingerprint",
    '"%06u"': "a six-digit PIN",
    '"%d"': "a number typed into a text row",
    '"%u"': "a number",
    '"%.*s"': "a bounded copy",
    '"%zu/%zu"': "the draft's byte meter",
    '"%.1f%%"': "a percentage",
    # strftime patterns; the C library localises these, not us.
    '"%H:%M"': "strftime: the clock",
    '"%b"': "strftime: the month",
    '"%a"': "strftime: the weekday",
    '"%e"': "strftime: the day of the month",
    # Files and paths, which are not read as words.
    '"canned.txt"': "the canned-message file's name",
    '"r"': "fopen mode",
    '"w"': "fopen mode",
    '"\\r\\n"': "line endings when reading canned.txt",
    # The keycaps in the action bar. These are printed on the Brick's case, so they read the
    # same in every language for the same reason a region code does; the verb beside each is
    # the translated half. See include/mesh/ui/actions.h.
    '"START"': "a keycap: what is printed on the button",
    '"SELECT"': "a keycap: what is printed on the button",
    '"MENU"': "a keycap: what is printed on the button",
    '"K%u"': "a keycap for a quit key MESHCLIENT_QUIT_KEYS rebound, which has no printed name",
    # A glyph, drawn rather than read: the star beside a pinned node.
    '"\\xE2\\xAD\\x90"': "the pinned-node star",
    # The keyboard's own layers. These are the keys, not words about them: a locale that wants
    # AZERTY needs a second layout table rather than a translation of this one. See docs/i18n.md.
    '"1234567890"': "the keyboard's number row",
    '"qwertyuiop"': "the keyboard's lower layer",
    '"asdfghjkl\'"': "the keyboard's lower layer",
    '"zxcvbnm,.?"': "the keyboard's lower layer",
    '"QWERTYUIOP"': "the keyboard's upper layer",
    '"ASDFGHJKL\\\""': "the keyboard's upper layer",
    '"ZXCVBNM!-:"': "the keyboard's upper layer",
}

LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')
PROSE = re.compile(r"[A-Za-z]{3,}")

# Two shapes that are never prose, exempted by their form rather than one by one so that the
# next environment variable or device path does not have to be added to ALLOWED by hand.
#
# The SHOUTING_CASE rule wants an underscore on purpose: it should match MESHCLIENT_QUIT_KEYS
# and not a bare word like "MQTT", which is a name we choose not to translate for its own
# reasons rather than something that is not text at all.
ENV_NAME = re.compile(r"^[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+$")
PATH_NAME = re.compile(r"^/[A-Za-z0-9_./%*+-]*$")


def strip_comments(text):
    """Blank out comments, keeping line numbers so a report can point at the right line."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:end]))
            i = end
        elif text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        elif text[i] == '"':
            m = LITERAL.match(text, i)
            if m is None:
                out.append(text[i])
                i += 1
            else:
                out.append(m.group(0))
                i = m.end()
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def findings(path):
    """Every prose literal in `path`, skipping includes and whole mesh_log() calls.

    A log call is skipped to its closing paren rather than to the end of its first line: the
    arguments that pick a word - `favorite ? "Pinned" : "Unpinned"` - are usually on the second.
    """
    source = strip_comments((ROOT / path).read_text())
    depth = 0
    for number, line in enumerate(source.split("\n"), 1):
        if depth > 0:
            depth += line.count("(") - line.count(")")
            continue
        stripped = line.strip()
        if stripped.startswith("#include"):
            continue
        if "mesh_log" in line:
            depth = line[line.index("mesh_log"):].count("(") - line[line.index("mesh_log"):].count(")")
            continue
        for match in LITERAL.finditer(line):
            literal = match.group(0)
            text = match.group(1)
            if literal in ALLOWED or not PROSE.search(text):
                continue
            if ENV_NAME.match(text) or PATH_NAME.match(text):
                continue
            yield f"{path}:{number}: {literal} is spelled out; give it a catalog id"


def main():
    problems = [problem for path in CHECKED for problem in findings(path)]
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        print(
            f"\n{len(problems)} hardcoded string(s). Add an entry to "
            "include/mesh/i18n/catalog.def and use mesh_str(); see docs/i18n.md.\n"
            "If the string is genuinely not prose, list it in ALLOWED in this script with "
            "the reason.",
            file=sys.stderr,
        )
        return 1
    print(f"No hardcoded user-facing strings in {len(CHECKED)} files.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
