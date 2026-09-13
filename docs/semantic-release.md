# Semantic Release Guide

This project uses [semantic-release](https://github.com/semantic-release/semantic-release) to automate versioning and releases based on commit messages.

## Commit Message Format

We follow the [Conventional Commits](https://www.conventionalcommits.org/) specification:

```
<type>[optional scope]: <description>

[optional body]

[optional footer(s)]
```

### Types

- **feat**: A new feature (triggers a **minor** version bump, e.g., 0.1.0 → 0.2.0)
- **fix**: A bug fix (triggers a **patch** version bump, e.g., 0.1.0 → 0.1.1)
- **perf**: A performance improvement (triggers a **patch** version bump)
- **refactor**: A code change that neither fixes a bug nor adds a feature (no release)
- **docs**: Documentation only changes (no release)
- **style**: Code style changes (formatting, missing semicolons, etc.) (no release)
- **test**: Adding or updating tests (no release)
- **build**: Changes to build system or dependencies (no release)
- **ci**: Changes to CI configuration files and scripts (no release)
- **chore**: Other changes that don't modify src or test files (no release)
- **revert**: Reverts a previous commit (triggers a **patch** version bump)

`refactor` releases nothing, and that is worth a word because it cut a patch until the cadence
changed. Under a release per merge it was harmless; under a batch it is a trap, because
`scripts/pak-changelog.py` excludes `refactor` as internals - so a batch that happened to hold
only refactors would cut a version whose store entry is empty, which is precisely the failure
[Store changelog](#store-changelog) exists to prevent. It is still *listed* in the release notes
when something else carried the release; what it no longer does is be the thing that carries one.

A breaking `refactor!:` is unaffected and still cuts a major. `releaseRules` are not first-match:
the analyzer collects *every* rule a commit matches and takes the highest release among them
(`lib/analyze-commit.js`), so the `breaking` rule - which matches whatever the type - outranks
the `false` on the type's own row.

### Breaking Changes

To trigger a **major** version bump (e.g., 0.1.0 → 1.0.0), include `BREAKING CHANGE:` in the commit body or append `!` after the type:

```bash
feat!: remove deprecated API

BREAKING CHANGE: The old `mesh_connect()` function has been removed. Use `mesh_transport_connect()` instead.
```

## Examples

### Feature (Minor Bump)
```bash
git commit -m "feat: add BLE device filtering by RSSI"
# 0.1.0 → 0.2.0
```

### Bug Fix (Patch Bump)
```bash
git commit -m "fix: resolve memory leak in bluez client"
# 0.1.0 → 0.1.1
```

### Performance Improvement (Patch Bump)
```bash
git commit -m "perf: optimize message framing algorithm"
# 0.1.0 → 0.1.1
```

### Breaking Change (Major Bump)
```bash
git commit -m "feat!: redesign transport API

BREAKING CHANGE: Transport initialization now requires explicit configuration struct"
# 0.1.0 → 1.0.0
```

### No Release
```bash
git commit -m "docs: update README with new examples"
# No version bump, no release
```

```bash
git commit -m "ci: update GitHub Actions workflow"
# No version bump, no release
```

## Branches

Semantic release is configured for these branches:

- **main**: Production releases (e.g., `v1.0.0`, `v1.1.0`)
- **beta**: Pre-release versions (e.g., `v1.0.0-beta.1`, `v1.0.0-beta.2`)
- **rc**: Release candidate versions (e.g., `v1.0.0-rc.1`, `v1.0.0-rc.2`)

## Cadence: a release is pressed, not merged

`main` is **not** a push trigger. Merging a pull request publishes nothing. A release is a
`workflow_dispatch` - **Actions → Semantic Release → Run workflow**, on `main` - with a Sunday
18:00 UTC cron behind it as the safety net.

That is a correction rather than a preference. Bound to the merge, every pull request was a
release: sixteen shipped on 2026-09-12, and four days took the client from v2.47 to v2.69. Three
things that costs, and none of them is the tag itself:

- **The Pak Store nags on every one.** Each stable release rewrites `version` in `pak.json`, and
  the store compares that against what is installed - so a day of merges is a day of "update
  available" on somebody's handheld.
- **The store's changelog window is five entries** (see [Store changelog](#store-changelog)). At
  a release per merge that covers about eight hours, so the "What's new in vX.Y.Z?" panel
  describes one pull request instead of a release.
- **The version stops describing anything.** A minor per feature is a reasonable rule when a
  release is a batch of work and an inflationary one when it is a commit.

What batching does *not* cost is the development loop, because the loop was never the release:
`ci.yml` is host-only and does not cross-compile, so a build reaches a Brick through `make brick`
and always did. Nor does it quieten a banner, because there was none to quieten - the in-client
updater has no automatic check at all (`mesh_updater_check()` has exactly one caller, the row in
Settings → About), so what a release per merge was reaching was the store, the watch list and the
changelog rather than anyone's screen.

The one thing it gives up is a fix sitting merged and unreleased. The cron bounds that at a week,
and a fix worth shipping today is a dispatch rather than a wait.

### `beta` and `rc` still release on push

Their push triggers are untouched, and they are the built-in answer if a per-merge channel is
ever wanted by people who want it: merge to `beta` for a prerelease per merge, promote to `main`
when it is a release. Nothing on that path reaches a user who has not asked for it - `pak.json`
and `CHANGELOG.md` are skipped for prereleases, so the store cannot see one, and `releases/latest`
hides them from a client that has not been set to the Prerelease channel in Settings → About.

### One run at a time, per branch

The workflow's `concurrency` group is `semantic-release-${{ github.ref }}` and it **does not
cancel in progress**. Both halves are load-bearing, and the second is subtler than it reads:
`cancel-in-progress: false` protects a run that has *started*, while a run still **pending** in
the group is cancelled by default the moment a newer one queues behind the same busy run. Under
one literal group that is a push to `beta` discarding the Sunday cron's queued `main` run - an
unrelated prerelease cancelling the safety net, which is the one failure a safety net may not
have.

Keyed on the ref, the pair worth serialising still is: a dispatch and the cron are both `main`,
and they can now land together. What that prevents is two version bumps pushed at one branch tip,
and a cancelled run leaving a tag published with nothing behind it. A pending duplicate lost
*within* one branch costs nothing, because the run ahead of it has already released everything
the second would have found.

## Workflow

1. Make changes to your code
2. Commit using conventional commit format:
   ```bash
   git add .
   git commit -m "fix: correct BLE connection timeout handling"
   ```
3. Merge to `main`. **The merge releases nothing** - see
   [Cadence](#cadence-a-release-is-pressed-not-merged).
4. Release when you decide there is one: **Actions → Semantic Release → Run workflow**, on
   `main`. Everything merged since the last tag goes into it, and the Sunday cron does it for
   you if you forget. (A push to `beta` or `rc` still releases on its own.)
5. The semantic-release workflow will:
   - Analyze your commits since the last release
   - Determine the next version number
   - Update `CMakeLists.txt` with the new version
   - **Then** build and package the release, from that rewritten version
   - Generate/update `CHANGELOG.md`
   - Commit the version bump back to the repository
   - Create a git tag
   - Create a GitHub release with artifacts

The order *within* that list matters - the version rewrite, and only then the build.
`project(meshclient VERSION ...)` in `CMakeLists.txt` is
where the client's own version comes from - it becomes the `MESHCLIENT_VERSION` compile
definition that `meshclient --version` and the About screen report, and that the in-app updater
compares against GitHub. A build that ran *before* the rewrite would ship the previous
release's number under the new tag, so both live in one `prepareCmd`:

```
prepareCmd: ./scripts/release-build.sh ${nextRelease.version}
```

`scripts/release-build.sh` does the rewrite itself and then greps the linked binary for the
version afterwards. Because `@semantic-release/exec` runs before `@semantic-release/git`, a
failed build aborts the release with nothing committed and no tag created.

### `main` is protected, and the release bot has to be let through

`main` carries a ruleset that requires the two CI checks. semantic-release does not open a pull
request - `@semantic-release/git` commits the version bump and pushes it **straight to `main`** -
so that push has to be allowed past the rule, and no amount of configuration makes it satisfy
one instead:

- the commit does not exist until the bot creates it, so the checks have never reported on it
  (the rule reads this as "expected", not "failed");
- the commit carries `[skip ci]`, so they would not run on it anyway;
- a push made with `GITHUB_TOKEN` does not trigger workflows, by design.

The way through is the ruleset's **bypass list**, and that decides which token pushes:

| Bypass actor available | Token to use |
|---|---|
| GitHub Actions | none needed - `GITHUB_TOKEN` already pushes as `github-actions[bot]` |
| Repository admin | a fine-grained PAT owned by an admin, as `RELEASE_TOKEN` |
| an installed GitHub App | an installation token minted in the workflow |

This repository uses the second: **`RELEASE_TOKEN`**, a fine-grained PAT scoped to this
repository with **Contents: write** and nothing else, added to the repository's secrets, with
**Repository admin** on the ruleset's bypass list.

It needs no other permission because the workflow keeps *two* identities apart:

| | Identity | Used for |
|---|---|---|
| `GIT_CREDENTIALS` | `RELEASE_TOKEN` | the one push: the version bump onto `main` |
| `GITHUB_TOKEN` | the scoped per-run token | the API: the release, its assets, its comments |

semantic-release builds its authenticated remote from the first of `GIT_CREDENTIALS`,
`GH_TOKEN`, `GITHUB_TOKEN` that can push (`lib/get-git-auth-url.js`), which is what makes
`@semantic-release/git` use the PAT while everything else goes on using the per-run token. The
release therefore stays published by `github-actions[bot]`, as every earlier one was.

#### Why the PAT is not handed to `actions/checkout`

It would be the obvious place - checkout persists whatever token it is given into `.git/config`,
and the push would pick it up from there. That is how this was first written, and it is wrong:
the credential would then sit on disk while the steps in between run `pip install`, build a
downloaded tarball and execute `npm install` lifecycle scripts. All of that is third-party code,
and a credential that **bypasses branch protection** is worth far more to a compromised
dependency than the scoped per-run token it replaced.

So checkout runs with `persist-credentials: false` and no token override, and the PAT appears
exactly once, as an env var on the release step. That does not make it untouchable - a
semantic-release plugin still runs with it in the environment - but it is out of reach of
everything installed before it.

#### The skip marker is the release bot's alone

The release commit's message ends with `[skip ci]`, which is how it avoids
kicking off a build of a commit that only moves a version number. That marker is not a comment:
GitHub reads it out of *any* head commit message and skips the workflow.

Now that `main` requires those checks, a marker in a pull request's head commit is a trap. The
checks never run, so they can never pass, so the pull request cannot be merged - and nothing on
the page says why, because there is no failure to look at. This has already happened once: the
commit introducing this very section carried the marker inside a sentence *explaining* it, and
CI silently declined to run.

If a message needs to talk about the marker, spell it in words, as this paragraph's neighbours
do. If a pull request has already been pushed with one, amend the message and force-push the
branch; there is no way to ask for the skipped run back.

**A fine-grained PAT expires**, and the two failures look different, which is worth knowing
before reading a red run:

- **no `RELEASE_TOKEN` at all** - the fallback hands the per-run token to the push, the ruleset
  refuses it, and the run fails with GH013 (see
  [Troubleshooting](#the-release-fails-with-gh013-repository-rule-violations));
- **an expired or revoked `RELEASE_TOKEN`** - the secret is still there, so it is still used, and
  the push fails on *authentication* inside the release step instead. No GH013, because the
  credential never gets as far as the rule.

Either way nothing is half-released. Setting a calendar reminder for the expiry is worth more
than it sounds.

### Prereleases on `beta` and `rc`

CMake's `project(VERSION)` accepts only numeric components — it errors outright on
`1.13.0-beta.1`. So the script splits the two:

- `CMakeLists.txt` gets the numeric part (`1.13.0`), which also keeps the rewrite idempotent:
  a suffix left in the file would not match the pattern next time and the version would
  compound rather than be replaced.
- the whole tag goes to the build as `-DMESHCLIENT_VERSION_OVERRIDE=1.13.0-beta.1`, and that
  is what `meshclient --version` and the About screen report. It defaults to empty, not to the
  project version: a cache entry seeded with `PROJECT_VERSION` is written on a build tree's
  first configure and never again, which silently pinned every incremental local build to the
  version its `build/` directory was created at.

A prerelease client also asks GitHub a different question. `releases/latest` skips prereleases
by design, so the Prerelease channel polls `releases?per_page=1` instead and is offered the
newest release of any kind; the Stable channel uses `releases/latest` and is never offered a
beta.

Which one a client uses is the **Update channel** row in Settings → About: `Automatic`
(prerelease builds follow prereleases, everything else follows stable — what the updater did
before the setting existed), `Stable`, or `Prerelease`. A cycles it, and it is saved to
`update_channel=` in `ui_prefs` straight away. Switching forgets whatever the last check
found, so an asset fetched on one channel is never installed after moving to the other.

### Only the release build is a release

`scripts/release-build.sh` is the only thing that passes `-DMESHCLIENT_RELEASE_BUILD=ON`. Every
other build — `make debug`, `make release`, `make brick` — reports `<version>-dev` and the
updater refuses to touch it. That is what stops a build you just deployed to a Brick from being
replaced by whatever is on GitHub. About says `Dev build; updates disabled`, a check still
reports what is out there (`Latest is 1.16.0; dev build, not installing`) and no install row is
offered.

Do not stamp a local build to try the updater out. Lift the guard for that run instead, either
way round:

- **Settings → About → Dev updates**, on the device. The row appears only on a non-release
  build, A toggles it, and it is saved to `update_allow_dev=` in `ui_prefs` — so a Brick with
  no computer nearby can still exercise the update path.
- **`MESHCLIENT_UPDATE_ALLOW_DEV=1`** for a run started from a shell. It wins over the saved
  preference for that run, and About shows the row as `on (environment)` rather than as a
  switch that would spring back.

A `-dev` build under either sorts below the release of the same version number, so it is
offered exactly the release its working tree is based on. Combine with `MESHCLIENT_UPDATE_REPO`
pointed at a scratch repo to test against releases you control.

## What Gets Updated

When semantic-release runs, it automatically:

1. **CMakeLists.txt**: Updates `project(meshclient VERSION x.y.z ...)`. This is the only place
   the client's version is stored. `package.json` exists purely to pin the semantic-release
   dev dependencies, so its `version` is the placeholder `0.0.0-semantically-released` — nothing
   reads it and nothing updates it, and a real number there would only go stale.
2. **pak.json**: Updates `version` to the release tag, which the NextUI Pak Store requires to
   match, and adds this version's `changelog` entry. Both are skipped for prereleases, so the
   file only ever carries the last stable `vX.Y.Z`. See [Store changelog](#store-changelog).
3. **CHANGELOG.md**: Generates release notes from commit messages
4. **Git tags**: Creates a new tag (e.g., `v1.2.3`)
5. **GitHub Releases**: Creates a release with:
   - `MeshClient.pak.zip` - The packaged TrimUI pak, for a fresh install. It holds the pak's
     contents rather than the `MeshClient.pak` folder, which is what the Pak Store unpacks
     into the folder it creates; renaming it breaks the `release_filename` in `pak.json`.
   - `MeshClient.pak.zip.sha256` - Checksum file
   - `meshclient-tg5040-aarch64` - The bare static binary, which is what the in-app updater
     downloads (Settings > About MeshClient). One file it can verify and rename into place,
     rather than a zip it would have to unpack on the device.
   - `meshclient-tg5040-aarch64.sha256` - Checksum file
   - Auto-generated release notes

   The updater verifies the download against the `digest` GitHub reports for the asset, not
   against the `.sha256` file; that file is published for people checking a manual download.
   Renaming or dropping the binary asset breaks self-update for every installed client, so
   keep its name in step with `MESHCLIENT_UPDATE_ASSET` in `src/core/updater.c`.

## Store changelog

The Pak Store reads `changelog` from `pak.json` as a `{version: text}` map and shows it twice: a
"What's new in vX.Y.Z?" panel when an installed pak is out of date, and a combined Changelog
section on the listing. Both look the entry up by the version string in the same file, so a map
that misses the current version renders nothing at all — which is what a hand-maintained one
does the first time a release forgets it.

`scripts/pak-changelog.py` therefore generates the entry, from the same Conventional Commit
subjects semantic-release turns into the release notes, and `release-build.sh` runs it right
after the version stamp. **Do not write `changelog` by hand**; a release overwrites its entry.

- Only `feat`, `fix`, `perf`, `revert` and breaking changes are summarised. `refactor` describes
  internals, so it stays out along with `docs`, `chore`, `test` and `ci` - and for that same
  reason it no longer triggers a release either.
- A breaking change is included whatever its type, and leads the entry. Both spellings count:
  `feat(cli)!:` in the header and a `BREAKING CHANGE:` footer, which is what a `refactor` that
  triggers a major release looks like. The footer's own wording is used when it has any, wrapped
  lines rejoined, since it describes the break where the subject only describes the change.
- The entry is one plain-text line, features first, capped at 6 items and 300 characters. The
  store renders it as a paragraph with no markdown on a handheld, so bullets and commit links
  would only be noise.
- Double quotes and non-ASCII are stripped. The quotes matter: both `release-build.sh` and the
  on-device stamp in `src/core/updater.c` find the version field by searching for the first
  `"version"` in the file, so `changelog` also has to stay *below* it.
- Only the newest 5 versions are kept, and prereleases never get an entry. Note the store sorts
  the combined section by string, not SemVer, so `v1.9.0` would sort above `v1.18.0` — another
  reason to keep the window short.
- A release with nothing user-facing in it gets no entry, and a failure to generate one is
  logged and does not fail the release.

Backfilling an already-shipped tag takes a second argument:

```bash
scripts/pak-changelog.py 1.18.0 v1.18.0
```

## How the release artifact is built

`scripts/release-build.sh` cross-compiles with the Bootlin `aarch64--musl` toolchain and
statically links a from-source libdbus, built with meson and `message_bus=false` (library only,
no daemon, no expat). Everything else the client needs is already static.

Version pins for the toolchain and for dbus live in **two** places —
`.github/workflows/semantic-release.yml` and `docker/setup-cross.sh`. **Bump them together**, or
a local pak build and a released one stop matching.

CI (`ci.yml`) is host-only gcc/clang on ubuntu-24.04 and does **not** cross-compile, so a
toolchain break only shows up at release time. `make docker-pak` is the local reproduction: it
runs `scripts/cross-build.sh`, which emits the same two artifacts `release-build.sh` does. On an
arm64 host the `cross` image uses native `musl-gcc` (exposed as `aarch64-linux-musl-gcc`) instead
of downloading Bootlin.

`scripts/package.sh` then assembles the pak: `$BUILD_ROOT/release/meshclient` into
`dist/MeshClient.pak/bin/shared/`, plus everything under
`Tools/tg5040/MeshClient.pak/bin/{shared,tg5040}/`, the CA bundle, and `launch.sh`. **`launch.sh`
must stay POSIX sh**; it sets `HOME` to the pak userdata dir, points `DBUS_SYSTEM_BUS_ADDRESS` at
the system socket, and tees output to `/.userdata/tg5040/logs/MeshClient.txt`.

### Dependency pins

Keep `conventional-changelog-conventionalcommits` on **9.x** until semantic-release's notes
generator ships `conventional-changelog-writer` 10. On 10.x, `generateNotes` fails with
"Missing helper". Dependabot is configured to ignore it.

## Testing Locally

You can test what version would be released without actually releasing:

```bash
npm install
npx semantic-release --dry-run --no-ci
```

Dry runs need **Node 24.10+**. If your host is older:

```bash
docker run --rm -v "$PWD":/src -w /src -e GITHUB_TOKEN=$(gh auth token) node:24 bash -c \
  'npm install && npx semantic-release --dry-run --branches <pushed-branch>'
```

## Initial Release

To create your first release after setting this up:

```bash
git add .
git commit -m "feat: initial release with BLE support"
git push origin main
```

This will create version `1.0.0` (since it's a new feature).

## Troubleshooting

### No release is created

First: on `main`, **nothing is released by a push**, and that is the expected behaviour rather
than a fault. Run the workflow from Actions, or wait for the Sunday cron. Then check that:
- Your commit messages follow the conventional format
- You dispatched on `main`, or pushed to `beta` or `rc`
- The commits include release-worthy types (`feat`, `fix`, `perf`, `revert`, or a breaking
  change). `refactor`, `docs`, `chore`, `test`, `build` and `ci` release nothing on their own,
  so a batch holding only those is correctly no release

### Version not updated in CMakeLists.txt

The workflow uses `sed` to update the version. Ensure the CMakeLists.txt has the format:
```cmake
project(meshclient VERSION 0.1.0 LANGUAGES C)
```

### The release fails with GH013 "repository rule violations"

```
ExecaError: Command failed with exit code 1: git push --tags '...' 'HEAD:main'
remote: error: GH013: Repository rule violations found for refs/heads/main.
remote: - 2 of 2 required status checks are expected.
 ! [remote rejected] HEAD -> main (push declined due to repository rule violations)
```

The push that carries the version bump was refused by `main`'s ruleset: whoever pushed it is not
on the bypass list. Either `RELEASE_TOKEN` is missing - the workflow then falls back to the
per-run token, which cannot bypass anything - or its owner has been taken off the list. An
*expired* token fails differently, on authentication rather than on the rule; see
[`main` is protected](#main-is-protected-and-the-release-bot-has-to-be-let-through).

Nothing is half-released when this happens: `@semantic-release/git` runs in *prepare*, before
the tag is pushed and before `@semantic-release/github` publishes, so a failure here leaves no
tag, no release and no version bump on `main`. Fix the token and re-run the failed workflow run -
semantic-release recomputes the next version from the tags and picks up where it left off.

### Permission errors

The workflow's `GITHUB_TOKEN` is provided automatically by GitHub Actions and its
`permissions:` block already grants the writes the release needs. A permission error that
mentions a *ref* rather than an API scope is usually the ruleset instead - see the entry
above.

## More Information

- [Conventional Commits](https://www.conventionalcommits.org/)
- [Semantic Release](https://semantic-release.gitbook.io/)
- [Semantic Versioning](https://semver.org/)
