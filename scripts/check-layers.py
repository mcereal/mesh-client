#!/usr/bin/env python3
"""Fail when one area of the tree includes a header from an area it is not allowed to see.

Every `.c` in this project compiles into one library, meshclient_core, so the linker has no
opinion about direction: `src/geo/mercator.c` could include "mesh/ui/store.h" tomorrow and the
build would be delighted. The layering in docs/architecture.md - data one way, input the other -
is a rule the compiler cannot see, in exactly the way "no prose in a renderer" is. This is the
check that does, and check-strings.py is its sibling.

An area is the directory under src/ or include/mesh/, so src/transport/ble/ble_transport.c and
include/mesh/transport/transport.h are both `transport`. ALLOWED lists, per area, the areas it
may include from; including from its own area is always fine, and so is a relative include of a
file beside it. Both `"mesh/..."` and `<mesh/...>` are read, because both compile. An area missing from ALLOWED may include nothing but itself.

The direction that matters most is `core` not seeing `ui`: the session, the message log and the
admin queue answer to a radio, not to a screen, and the moment one of them reads a store record
the client can no longer be driven headless. `app` is the composition root and sits above both -
it is the one place that is allowed to know about every other, because assembling them is what
it is for.

Run it directly, or through `ctest` / `make test`, which is where it will catch somebody.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# What each area may include from, beyond itself. The comment on a line is why the edge exists;
# an edge with no reason to be here is one to delete rather than one to document.
ALLOWED = {
    # The bottom. Both are leaves on purpose: a helper that reaches back up the tree is how a
    # utility becomes a layer nobody can move.
    "utils": set(),
    "geo": set(),
    # The catalog answers with ids and text and nothing else.
    "i18n": set(),
    # Wire formats. They parse bytes; they do not know what the bytes are for.
    "proto": {"utils"},
    # Tiles are a projection and a cache, which is geometry and files.
    "map": {"geo", "utils"},
    # A link carries frames for a session, so it knows the session's types. It does not know
    # that anything draws them.
    "transport": {"utils", "proto", "i18n", "core"},
    # The radio's half of the client. Deliberately no "ui": see the docstring.
    "core": {"utils", "proto", "geo", "i18n", "transport"},
    # The screen's half. It reads core's types to render them and never the other way.
    "ui": {"utils", "core", "i18n", "geo", "map", "proto"},
    # The composition root: it owns one of everything and wires them together.
    "app": {"utils", "core", "ui", "transport", "proto", "geo", "i18n", "map"},
    # src/main.c - argument parsing around the app. Same licence, for the same reason.
    "main": {"utils", "core", "ui", "transport", "proto", "geo", "i18n", "map", "app"},
}

# Both spellings. include/ is a PUBLIC include directory on meshclient_core, so
# `#include <mesh/ui/store.h>` compiles exactly as the quoted form does - and a check that
# only reads one of them is one a change of quote character walks past. Nothing in the tree
# uses the angle-bracket form today; that is the reason to match it now rather than later.
INCLUDE = re.compile(r'^\s*#\s*include\s+["<]mesh/([a-z0-9_]+)/')


def area_of(path):
    """The area a file belongs to, or None for a file this check has no opinion about."""
    parts = path.relative_to(ROOT).parts
    if parts[0] == "src":
        # src/main.c has no directory of its own; everything else is src/<area>/...
        return "main" if len(parts) == 2 else parts[1]
    if parts[:2] == ("include", "mesh"):
        # A header directly under include/mesh/ belongs to no area: there is one, and it is the
        # inkcell compatibility list, which is deliberately tree-wide. Every layer says the old
        # name of something that moved to the toolkit, so filing it under one of them would make
        # utils include from ui to spell `mesh_text_copy`.
        return parts[2] if len(parts) > 3 else None
    return None


def sources():
    for root in (ROOT / "src", ROOT / "include" / "mesh"):
        for path in sorted(root.rglob("*")):
            if path.suffix in (".c", ".h") and path.is_file():
                yield path


def findings(path):
    area = area_of(path)
    if area is None:
        return
    allowed = ALLOWED.get(area, set()) | {area}
    for number, line in enumerate(path.read_text(errors="ignore").splitlines(), 1):
        match = INCLUDE.match(line)
        if match is None:
            continue
        target = match.group(1)
        if target in allowed:
            continue
        rel = path.relative_to(ROOT)
        yield f"{rel}:{number}: {area} may not include from {target} ({line.strip()})"


def main():
    checked = list(sources())
    unknown = sorted({area_of(p) for p in checked} - set(ALLOWED) - {None})
    problems = [problem for path in checked for problem in findings(path)]
    for problem in problems:
        print(problem, file=sys.stderr)
    if unknown:
        # A new directory with no entry would otherwise be silently allowed nothing, which reads
        # as a pile of violations rather than as the one thing that actually happened.
        print(
            f"\nNo ALLOWED entry for: {', '.join(unknown)}. Add one to this script, with the "
            "areas it may include from and why.",
            file=sys.stderr,
        )
    if problems or unknown:
        print(
            f"\n{len(problems)} include(s) cross a layer the wrong way. Either the include is "
            "the bug - move the code, or pass the value in rather than reaching for it - or the "
            "edge is real and belongs in ALLOWED in this script, with the reason.\n"
            "See docs/architecture.md.",
            file=sys.stderr,
        )
        return 1
    print(f"No layering violations in {len(checked)} files.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
