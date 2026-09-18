/**
 * semantic-release configuration.
 *
 * This is JavaScript rather than the `.releaserc.json` it replaced for one reason: a prerelease
 * and a stable release do not run the same plugins. Everything else here is the JSON, unchanged.
 *
 * A stable release writes three files back to `main` - the version in CMakeLists.txt, the
 * CHANGELOG entry, and pak.json for the Pak Store. A prerelease writes *nothing*, and that is
 * what keeps the channel branches usable: with no commit of its own, `beta` stays a pure
 * fast-forward of `main` forever, so pointing it at a newer `main` is always a fast-forward and
 * every `-beta.N` tag stays reachable from the branch that produced it. Let a prerelease commit
 * a version bump instead and the branch diverges on the one line a release rewrites: the next
 * sync is a merge with a conflict, and a force-push "fixing" that orphans the commit the last
 * prerelease tag points at - after which semantic-release cannot see that tag and cuts `beta.1`
 * on top of a tag that already exists.
 *
 * Nothing is lost by skipping them, because none of the three means anything on a prerelease:
 * CMakeLists.txt is rewritten in the build by `prepareCmd` whether or not the result is
 * committed, pak.json is deliberately left on its last stable version (see release-build.sh),
 * and CHANGELOG.md describes releases people are offered rather than every candidate for one.
 *
 * The channel comes from RELEASE_CHANNEL, which .github/workflows/semantic-release.yml sets for
 * every trigger. The GITHUB_REF fallback is for a run that did not come from that workflow, and
 * the branch *names* below are unconditional either way - a mislabelled channel would change
 * which plugins run, never which versions a branch may publish.
 */
const channel = (
  process.env.RELEASE_CHANNEL ||
  (process.env.GITHUB_REF || "").replace(/^refs\/heads\//, "") ||
  "stable"
).trim();

const prerelease = channel === "beta" || channel === "rc";

/** The stable-only half: the three files a release commits back to its branch. */
const writeBackPlugins = [
  [
    "@semantic-release/changelog",
    {
      changelogFile: "CHANGELOG.md",
    },
  ],
  [
    "@semantic-release/git",
    {
      assets: ["CMakeLists.txt", "CHANGELOG.md", "pak.json"],
      message:
        "chore(release): ${nextRelease.version} [skip ci]\n\n${nextRelease.notes}",
    },
  ],
];

export default {
  branches: [
    "main",
    {
      name: "beta",
      prerelease: true,
    },
    {
      name: "rc",
      prerelease: true,
    },
  ],
  plugins: [
    [
      "@semantic-release/commit-analyzer",
      {
        preset: "conventionalcommits",
        releaseRules: [
          { type: "feat", release: "minor" },
          { type: "fix", release: "patch" },
          { type: "perf", release: "patch" },
          { type: "revert", release: "patch" },
          { type: "docs", release: false },
          { type: "style", release: false },
          { type: "chore", release: false },
          { type: "refactor", release: false },
          { type: "test", release: false },
          { type: "build", release: false },
          { type: "ci", release: false },
          { breaking: true, release: "major" },
        ],
      },
    ],
    [
      "@semantic-release/release-notes-generator",
      {
        preset: "conventionalcommits",
        presetConfig: {
          types: [
            { type: "feat", section: "Features" },
            { type: "fix", section: "Bug Fixes" },
            { type: "perf", section: "Performance Improvements" },
            { type: "revert", section: "Reverts" },
            { type: "docs", section: "Documentation", hidden: false },
            { type: "style", section: "Styles", hidden: true },
            { type: "chore", section: "Miscellaneous Chores", hidden: true },
            { type: "refactor", section: "Code Refactoring" },
            { type: "test", section: "Tests", hidden: true },
            { type: "build", section: "Build System", hidden: true },
            { type: "ci", section: "Continuous Integration", hidden: true },
          ],
        },
      },
    ],
    [
      "@semantic-release/exec",
      {
        verifyReleaseCmd: "echo ${nextRelease.version}",
        prepareCmd: "./scripts/release-build.sh ${nextRelease.version}",
      },
    ],
    ...(prerelease ? [] : writeBackPlugins),
    [
      "@semantic-release/github",
      {
        assets: [
          { path: "dist/MeshClient.pak.zip", label: "MeshClient.pak.zip" },
          {
            path: "dist/MeshClient.pak.zip.sha256",
            label: "MeshClient.pak.zip.sha256",
          },
          {
            path: "dist/meshclient-tg5040-aarch64",
            label: "meshclient-tg5040-aarch64",
          },
          {
            path: "dist/meshclient-tg5040-aarch64.sha256",
            label: "meshclient-tg5040-aarch64.sha256",
          },
          // The desktop and server download: the CLI, static, for the machine the release was
          // cut on. Named for the runner's architecture, which is x86-64 on ubuntu-24.04 - a
          // release cut somewhere else publishes the pak and the device binary as usual and
          // logs this one as unreadable, which is the right trade for an extra convenience
          // asset. See scripts/linux-cli-build.sh.
          {
            path: "dist/meshclient-linux-x86_64",
            label: "meshclient-linux-x86_64",
          },
          {
            path: "dist/meshclient-linux-x86_64.sha256",
            label: "meshclient-linux-x86_64.sha256",
          },
        ],
      },
    ],
  ],
};
