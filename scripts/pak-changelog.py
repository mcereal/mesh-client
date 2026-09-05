#!/usr/bin/env python3
"""Write the Pak Store changelog entry for a release into pak.json.

The store reads `changelog` as a {version: text} map and shows it in two places: a "What's new
in vX.Y.Z?" panel when the installed pak is out of date, and a combined Changelog section on the
listing. Both key off the version string, so an entry that is missing for the version in
`pak.json` simply renders nothing - which is what happens if the map is maintained by hand and
forgotten during a release.

So it is generated instead, from the same Conventional Commit subjects semantic-release already
uses for the GitHub release notes, and stamped by scripts/release-build.sh alongside the version.
The entry is deliberately a plain one-line summary rather than the release notes themselves: the
store renders it as a paragraph on a 1024x768 handheld with no markdown, so headings, bullets
and commit links would come out as noise.

Usage:
    scripts/pak-changelog.py <version> [end-ref]

`version` is the bare SemVer being released (1.19.0, no leading "v"). `end-ref` defaults to HEAD
and only exists to backfill entries for tags that already shipped.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

# A release's entry is a paragraph on a handheld, so it is capped twice: by item count, then by
# characters. Both are generous enough for a normal release and stop a 30-commit one from
# filling the screen.
MAX_ITEMS = 6
MAX_CHARS = 300

# The types worth telling a user about. These are exactly the ones semantic-release counts as a
# release (see .releaserc.json); refactor bumps a patch but describes internals, so it is left
# out along with docs, chore, test, build and ci.
USER_FACING_TYPES = ("feat", "fix", "perf", "revert")

# Breaking changes first, then features, then everything else in commit order: the front of the
# entry is what the "What's new" panel leads with.
BREAKING_ORDER = -1
TYPE_ORDER = {"feat": 0, "perf": 1, "fix": 2, "revert": 3}

STABLE_TAG = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")
CONVENTIONAL = re.compile(
    r"^(?P<type>[a-z]+)(?:\((?P<scope>[^)]*)\))?(?P<breaking>!)?:\s*(?P<subject>.+)$"
)
# A breaking change can be declared in the header with `!` or in a footer, and the footer spells
# it either way round. commit-analyzer makes both a major release, so both have to reach here -
# and the footer form is the one that can sit on a type this script would otherwise drop.
BREAKING_FOOTER = re.compile(r"^BREAKING[ -]CHANGE:[ \t]*(?P<text>.*)$", re.MULTILINE)
# What ends that footer's text: a blank line, or the token of the next footer. Commit bodies here
# are wrapped at 72 characters, so the description almost always continues onto further lines and
# reading only the first would cut it mid-sentence.
FOOTER_TOKEN = re.compile(r"^(?:[A-Za-z][A-Za-z-]*: |BREAKING[ -]CHANGE:|[A-Za-z-]+ #)")

# Field and record separators for `git log`, chosen so a commit body cannot contain them. The
# format asks git for them by escape (`%x00`) rather than carrying the bytes: a NUL cannot be
# passed in an argument.
LOG_FORMAT = "--pretty=format:%s%x00%b%x1e"
FIELD_SEP = "\x00"
RECORD_SEP = "\x1e"


def git(*args: str) -> str:
    return subprocess.run(
        ("git", *args), check=True, capture_output=True, text=True
    ).stdout.strip()


def commits(span: str) -> list[tuple[str, str]]:
    """The (subject, body) of every non-merge commit in `span`, newest first.

    The body is read as well as the subject because a `BREAKING CHANGE:` footer is the one thing
    that turns a commit this script ignores - a refactor, say - into the major release the entry
    most needs to describe.
    """
    log = git("log", "--no-merges", LOG_FORMAT, span)
    parsed = []
    for record in log.split(RECORD_SEP):
        record = record.strip("\n")
        if not record.strip():
            continue
        subject, _, body = record.partition(FIELD_SEP)
        parsed.append((subject.strip(), body))
    return parsed


def stable_tags() -> list[tuple[tuple[int, int, int], str]]:
    """Every plain vX.Y.Z tag, newest last. Prereleases are skipped for the same reason
    release-build.sh skips them: the store only ever sees stable versions."""
    tags = []
    for tag in git("tag", "--list", "v*").splitlines():
        match = STABLE_TAG.match(tag.strip())
        if match:
            tags.append((tuple(int(part) for part in match.groups()), tag.strip()))
    tags.sort()
    return tags


def previous_tag(version: tuple[int, int, int]) -> str | None:
    """The newest stable tag below the version being released.

    Not `git describe`: during a release the tag being built does not exist yet, but during a
    backfill it does, so comparing versions is the only answer that is right in both cases.
    """
    below = [tag for (parsed, tag) in stable_tags() if parsed < version]
    return below[-1] if below else None


def clean(text: str) -> str:
    """Reduce a commit subject to something safe to sit inside pak.json.

    Double quotes go because pak.json is rewritten twice by line-oriented tools that look for
    `"version"` - the sed in release-build.sh and the on-device stamp in src/core/updater.c -
    and a quoted word inside a changelog value is the one thing that could confuse either.
    Non-ASCII goes because nothing downstream promises a font for it.
    """
    text = text.replace('"', "")
    text = "".join(char for char in text if 32 <= ord(char) < 127)
    text = " ".join(text.split())
    # Items are joined with "; " and the line gets one "." at the end, so a sentence that already
    # ends in one - a breaking-change footer usually does - loses it here.
    return text[:-1] if text.endswith(".") else text


def breaking_footer(body: str) -> str | None:
    """The description a `BREAKING CHANGE:` footer gives, or None when the body has no footer.

    An empty string means the footer is there but bare, which is a real thing people write and
    has to stay distinguishable from its absence.
    """
    lines = (body or "").splitlines()
    start = None
    text: list[str] = []
    for index, line in enumerate(lines):
        match = BREAKING_FOOTER.match(line.strip())
        if match is not None:
            start = index
            text.append(match.group("text").strip())
            break
    if start is None:
        return None

    for line in lines[start + 1 :]:
        if not line.strip() or FOOTER_TOKEN.match(line.strip()):
            break
        text.append(line.strip())
    return " ".join(part for part in text if part)


def summarize(subjects: list[tuple[str, str]]) -> str:
    """Turn the (subject, body) pairs of a release into one line."""
    items: list[tuple[int, int, str]] = []
    seen = set()

    for index, (subject, body) in enumerate(subjects):
        match = CONVENTIONAL.match(subject)
        if not match:
            continue
        kind = match.group("type")

        footer = breaking_footer(body)
        breaking = match.group("breaking") is not None or footer is not None
        if kind not in USER_FACING_TYPES and not breaking:
            continue

        # The footer describes the break in its own words; the subject only describes the change
        # that caused it. Prefer the footer, and fall back when it is the bare marker.
        text = clean(footer if footer else match.group("subject"))
        if not text or text.lower() in seen:
            continue
        seen.add(text.lower())

        text = text[0].upper() + text[1:]
        if breaking:
            text = "Breaking: " + text
        items.append((BREAKING_ORDER if breaking else TYPE_ORDER.get(kind, 4), index, text))

    if not items:
        return ""

    items.sort(key=lambda item: (item[0], item[1]))
    kept = [text for (_, _, text) in items[:MAX_ITEMS]]
    more = len(items) - len(kept)

    summary = "; ".join(kept)
    while len(summary) > MAX_CHARS and len(kept) > 1:
        kept.pop()
        more = len(items) - len(kept)
        summary = "; ".join(kept)
    if len(summary) > MAX_CHARS:
        summary = summary[: MAX_CHARS - 3].rstrip() + "..."
    if more > 0:
        summary += f" (and {more} more)"
    return summary + "."


def load_pak(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def write_pak(path: Path, pak: dict, changelog: dict[str, str], keep: int) -> None:
    """Store the changelog, newest `keep` versions only, in a fixed position in the file.

    Position matters: the field must stay *after* the top-level "version", because both
    rewriters find that field with a plain search for the first `"version"` in the file.
    """
    ordered = sorted(
        changelog.items(),
        key=lambda item: tuple(int(part) for part in STABLE_TAG.match(item[0]).groups()),
        reverse=True,
    )[:keep]

    rebuilt: dict = {}
    for key, value in pak.items():
        if key == "changelog":
            continue
        rebuilt[key] = value
        if key == "release_filename":
            rebuilt["changelog"] = dict(ordered)
    if "changelog" not in rebuilt:
        rebuilt["changelog"] = dict(ordered)

    path.write_text(json.dumps(rebuilt, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="the SemVer being released, without a leading v")
    parser.add_argument(
        "end_ref", nargs="?", default="HEAD", help="range end; only used to backfill old tags"
    )
    parser.add_argument("--pak", default="pak.json", type=Path)
    parser.add_argument(
        "--keep", type=int, default=5, help="how many versions to keep in the file"
    )
    args = parser.parse_args()

    match = STABLE_TAG.match("v" + args.version)
    if not match:
        print(f"Not a stable version: {args.version}", file=sys.stderr)
        return 1
    version = tuple(int(part) for part in match.groups())
    tag = "v" + args.version

    previous = previous_tag(version)
    span = f"{previous}..{args.end_ref}" if previous else args.end_ref
    summary = summarize(commits(span))
    if not summary:
        # A release with nothing user-facing in it - a lone refactor, say. Leaving the entry out
        # is better than inventing one: the store just shows no "What's new" panel.
        print(f"No user-facing commits in {span}; leaving pak.json alone.")
        return 0

    pak = load_pak(args.pak)
    changelog = {
        key: value for key, value in (pak.get("changelog") or {}).items() if STABLE_TAG.match(key)
    }
    changelog[tag] = summary
    write_pak(args.pak, pak, changelog, args.keep)

    print(f"{tag}: {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
