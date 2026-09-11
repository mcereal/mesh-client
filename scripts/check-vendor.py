#!/usr/bin/env python3
"""Fail when a vendored third-party file is not the one its README says it is.

`third_party/wuffs/wuffs-v0.4.c` is 3.6 MB of generated C that nobody is going to read, which
makes it precisely the kind of file somebody patches in place when a fix is needed. A local edit
that survives into a release is a fix nobody upstream knows about and nobody here can find
again - and it is invisible in review, because a diff against a file that size is not something
anyone looks at. This is what notices.

The expected digest is not kept here. It is read out of the vendored directory's own README,
which is where a human looks and therefore the copy that has to be right: a digest in this
script and a different one in the README would leave the documentation lying while the check
passed. Updating a dependency means editing the README, and this makes that the *only* place.

Run it directly, or through `ctest` / `make test`, which is where it will catch somebody.
"""

import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Each vendored file, and the README that states what it should be. Add a row when something
# else arrives in third_party/ as a file rather than as a submodule.
VENDORED = [
    ("third_party/wuffs/wuffs-v0.4.c", "third_party/wuffs/README.md"),
]


def expected_digest(readme: Path, name: str) -> str:
    """The SHA-256 out of the README's provenance table."""
    for line in readme.read_text(encoding="utf-8").splitlines():
        if "SHA-256" not in line:
            continue
        found = re.search(r"`([0-9a-f]{64})`", line)
        if found:
            return found.group(1)
    sys.exit(f"check-vendor: {readme} states no SHA-256 for {name}")


def main() -> int:
    problems = []
    for relative, readme_relative in VENDORED:
        path = ROOT / relative
        readme = ROOT / readme_relative
        if not path.exists():
            problems.append(f"{relative} is missing")
            continue
        if not readme.exists():
            problems.append(f"{readme_relative} is missing")
            continue
        want = expected_digest(readme, relative)
        got = hashlib.sha256(path.read_bytes()).hexdigest()
        if got != want:
            problems.append(
                f"{relative} is not what {readme_relative} says it is\n"
                f"    expected {want}\n"
                f"    found    {got}\n"
                "    A vendored file is upstream's, not ours: fix it there and re-vendor the\n"
                "    new revision, updating the README's table - do not edit it in place."
            )
        else:
            print(f"ok  {relative} matches {readme_relative} ({got[:12]}...)")

    for problem in problems:
        print(f"check-vendor: {problem}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
