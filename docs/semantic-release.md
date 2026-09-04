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
- **refactor**: A code change that neither fixes a bug nor adds a feature (triggers a **patch** version bump)
- **docs**: Documentation only changes (no release)
- **style**: Code style changes (formatting, missing semicolons, etc.) (no release)
- **test**: Adding or updating tests (no release)
- **build**: Changes to build system or dependencies (no release)
- **ci**: Changes to CI configuration files and scripts (no release)
- **chore**: Other changes that don't modify src or test files (no release)
- **revert**: Reverts a previous commit (triggers a **patch** version bump)

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

## Workflow

1. Make changes to your code
2. Commit using conventional commit format:
   ```bash
   git add .
   git commit -m "fix: correct BLE connection timeout handling"
   ```
3. Push to the appropriate branch:
   ```bash
   git push origin main
   ```
4. The semantic-release workflow will:
   - Analyze your commits since the last release
   - Determine the next version number
   - Update `CMakeLists.txt` with the new version
   - **Then** build and package the release, from that rewritten version
   - Generate/update `CHANGELOG.md`
   - Commit the version bump back to the repository
   - Create a git tag
   - Create a GitHub release with artifacts

The order of steps 3 and 4 matters. `project(meshclient VERSION ...)` in `CMakeLists.txt` is
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

### Prereleases on `beta` and `rc`

CMake's `project(VERSION)` accepts only numeric components — it errors outright on
`1.13.0-beta.1`. So the script splits the two:

- `CMakeLists.txt` gets the numeric part (`1.13.0`), which also keeps the rewrite idempotent:
  a suffix left in the file would not match the pattern next time and the version would
  compound rather than be replaced.
- the whole tag goes to the build as `-DMESHCLIENT_VERSION_FULL=1.13.0-beta.1`, and that is
  what `meshclient --version` and the About screen report.

A prerelease client also asks GitHub a different question. `releases/latest` skips prereleases
by design, so a beta build polls `releases?per_page=1` instead and is offered the newest
release of any kind; a stable build keeps using `releases/latest` and is never offered a beta.

### Only the release build is a release

`scripts/release-build.sh` is the only thing that passes `-DMESHCLIENT_RELEASE_BUILD=ON`. Every
other build — `make debug`, `make release`, `make brick` — reports `<version>-dev` and the
updater refuses to touch it. That is what stops a build you just deployed to a Brick from being
replaced by whatever is on GitHub, so do not stamp a local build to try the updater out; point
`MESHCLIENT_UPDATE_REPO` at a scratch repo instead.

## What Gets Updated

When semantic-release runs, it automatically:

1. **CMakeLists.txt**: Updates `project(meshclient VERSION x.y.z ...)`
2. **CHANGELOG.md**: Generates release notes from commit messages
3. **Git tags**: Creates a new tag (e.g., `v1.2.3`)
4. **GitHub Releases**: Creates a release with:
   - `MeshClient.pak.zip` - The packaged TrimUI pak, for a fresh install
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

## Testing Locally

You can test what version would be released without actually releasing:

```bash
npm install
npx semantic-release --dry-run --no-ci
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

Check that:
- Your commit messages follow the conventional format
- You're pushing to `main`, `beta`, or `rc` branch
- The commits include release-worthy types (`feat`, `fix`, `perf`, etc.)

### Version not updated in CMakeLists.txt

The workflow uses `sed` to update the version. Ensure the CMakeLists.txt has the format:
```cmake
project(meshclient VERSION 0.1.0 LANGUAGES C)
```

### Permission errors

Ensure the `GITHUB_TOKEN` has write permissions in the workflow. This is automatically provided by GitHub Actions.

## More Information

- [Conventional Commits](https://www.conventionalcommits.org/)
- [Semantic Release](https://semantic-release.gitbook.io/)
- [Semantic Versioning](https://semver.org/)
