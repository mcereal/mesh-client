# Releasing

Versioning is [semantic-release](https://github.com/semantic-release/semantic-release) over
[Conventional Commits](https://www.conventionalcommits.org/).

| Type | Bump |
|---|---|
| `feat` | minor |
| `fix`, `perf`, `revert` | patch |
| `refactor`, `docs`, `style`, `test`, `build`, `ci`, `chore` | none |
| `!` after the type, or `BREAKING CHANGE:` in the body | major, whatever the type |

`releaseRules` are not first-match: the analyzer collects *every* rule a commit matches and takes
the highest, so the breaking rule outranks the `false` on its type's own row.

`refactor` releasing nothing is deliberate. `scripts/pak-changelog.py` excludes it as internals,
so a batch holding only refactors would cut a version whose store entry is empty.

## A release is pressed, not merged

**`main` is not a push trigger. Merging a pull request publishes nothing.** A release is
**Actions → Semantic Release → Run workflow** on `main`, with a Sunday 18:00 UTC cron as the
safety net, so a day's pull requests batch into one release. Bound to the merge, every pull
request was a release — sixteen shipped in one day, and the Pak Store nags on every one.

The dispatch takes one input, **Release channel**:

| Channel | Publishes | Who is offered it |
|---|---|---|
| `stable` | tag, assets, `CHANGELOG.md`, the version bump, and the `pak.json` the store reads | everyone |
| `beta` / `rc` | tag and assets only — **nothing is committed** | only a client set to Prerelease |

`beta` and `rc` are *plumbing the workflow points at `main`*, not branches to work on. From a
terminal, `make ship`, `make ship-beta` and `make ship-rc` press the same button through the `gh`
CLI, all dispatching on `main` whatever branch you are standing on — a release is every commit
since the last tag, not anything about your working tree.

## What a release updates

1. **`CMakeLists.txt`** — `project(meshclient VERSION x.y.z ...)`, the only place the version
   lives. **Do not edit that line by hand.** (`package.json`'s version is a placeholder; nothing
   reads it.)
2. **`pak.json`** — `version` (which the Pak Store requires to match the tag) and this version's
   `changelog` entry. Both skipped for prereleases.
3. **`CHANGELOG.md`**, the git tag, and a GitHub release carrying four assets:
   `MeshClient.pak.zip` (+ `.sha256`) for a fresh install, and `meshclient-tg5040-aarch64`
   (+ `.sha256`), the bare static binary the in-app updater downloads.

The updater verifies against the `digest` GitHub reports for the asset, not the `.sha256` file.
**Renaming or dropping the binary asset breaks self-update for every installed client** — keep
its name in step with `MESHCLIENT_UPDATE_ASSET` in `src/core/updater.c`. The zip holds the pak's
*contents*, not the `MeshClient.pak` folder, which is what the store unpacks into the folder it
creates; renaming it breaks `release_filename` in `pak.json`.

## The store changelog

The Pak Store reads `changelog` from `pak.json` as a `{version: text}` map and looks the entry up
by version string, so a map missing the current version renders nothing at all.
`scripts/pak-changelog.py` generates it from the same commit subjects, and
`release-build.sh` runs it right after the version stamp. **Do not write it by hand.**

- Only `feat`, `fix`, `perf`, `revert` and breaking changes are summarised.
- A breaking change leads the entry whatever its type, and its footer wording is used when it has
  any, since that describes the break where the subject describes the change.
- One plain-text line, features first, capped at 6 items and 300 characters — the store renders
  it as a paragraph on a handheld, so bullets and links would be noise.
- Double quotes and non-ASCII are stripped. The quotes matter: both `release-build.sh` and the
  on-device stamp find the version field by searching for the first `"version"`, so `changelog`
  has to stay *below* it.
- Newest 5 versions only. The store sorts that section by string, not SemVer, so `v1.9.0` would
  sort above `v1.18.0` — another reason to keep the window short.

Backfill a shipped tag with `scripts/pak-changelog.py 1.18.0 v1.18.0`.

## The artifact

`scripts/release-build.sh` cross-compiles with the Bootlin `aarch64--musl` toolchain and
statically links a from-source libdbus (meson, `message_bus=false`). Version pins for the
toolchain and dbus live in **two** places — `.github/workflows/semantic-release.yml` and
`docker/setup-cross.sh`. **Bump them together.**

## Troubleshooting

- **No release was created.** On `main` nothing is released by a push; that is the design. Run
  the workflow from Actions or wait for the cron. Then check the commits include a
  release-worthy type — a batch holding only `refactor`/`docs`/`chore`/`test`/`ci` is correctly
  no release.
- **`GH013: repository rule violations` on the version-bump push.** `main`'s ruleset refused the
  push: either `RELEASE_TOKEN` is missing (the workflow falls back to the per-run token, which
  cannot bypass anything) or its owner is off the bypass list.
