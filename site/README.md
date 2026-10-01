# meshclient.dev

The project website: a static [Astro](https://astro.build) site served by Cloudflare as a
Worker with static assets only. No code runs on a request; every page is an HTML file written
at build time.

```bash
cd site
npm ci
npm run dev       # http://localhost:4321, reloads on save
npm run build     # astro check (types) + the static build into dist/
npm run preview   # build, then serve dist/ the way Cloudflare will (wrangler dev)
```

Node 22.12 or later.

## Where the pages are

- `src/pages/` holds the home page, the download page and the 404, which are plain Astro pages
  in `src/layouts/Base.astro`.
- `src/content/docs/guide/` holds the user guide at `/guide/`, one Markdown file per page. It is
  [Starlight](https://starlight.astro.build), which supplies the sidebar, the search (Pagefind,
  built at build time) and the light/dark switch. The sidebar order is the `sidebar` list in
  `astro.config.mjs`, so a new page goes there as well as in the folder.

The guide is written for people using the client, not working on it; `docs/` in the repository
root is the developer reference. A guide page names the labels the client draws, such as
**Show my contact code**, so a change to one in `include/mesh/i18n/catalog.def` is a change to
check against the guide.

## What it reads from the rest of the repository

- **Screenshots** are imported from `../.github/resources/screenshots/`, the files the README
  shows and `make screenshots` redraws, so the site never carries a second copy.
- **Download links** are `releases/latest/download/<asset>` URLs, which GitHub redirects to the
  newest stable release. The asset names in `src/pages/download.astro` are the ones
  `release.config.mjs` and the desktop packaging jobs publish; renaming one there breaks its
  link here.
- **The version label** is the one thing read at build time, from the GitHub API
  (`src/lib/release.ts`). If the lookup fails the page leaves the label out rather than failing
  the build. Set `GITHUB_TOKEN` in the build environment if the unauthenticated rate limit bites.

## Deploying

Cloudflare builds and deploys this directory on every push, from its Git integration (Workers
Builds). One-time setup in the dashboard:

1. **Workers & Pages → Create → Import a repository**, pick `mcereal/mesh-client`.
2. **Root directory** `site`, **build command** `npm run build`, **deploy command**
   `npx wrangler deploy`. The Worker's name must match `name` in `wrangler.jsonc`.
3. **Build watch paths**: include `site/*` and `.github/resources/screenshots/*`, so a push that
   only touches C sources does not rebuild the site.
4. Turn on non-production branch builds for a preview URL per pull request.

`wrangler.jsonc` attaches `meshclient.dev` and `www.meshclient.dev` as custom domains on
deploy, which creates their DNS records and certificates. The zone has to be active on the same
Cloudflare account first.

Because the version label is fixed at build time, a release leaves it one version behind until
the next deploy. A **Deploy Hook** (the Worker's **Settings → Builds → Deploy Hooks**) is a URL
that rebuilds on a `POST`; calling it from the release workflow keeps the label current.
