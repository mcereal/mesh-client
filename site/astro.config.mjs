// @ts-check
import { defineConfig } from "astro/config";

// A static build: every page is HTML at build time and Cloudflare serves the files as they are.
// No adapter, because nothing here renders on request.
export default defineConfig({
    site: "https://meshclient.dev",
    vite: {
        // The screenshots are imported from ../.github/resources/, where `make screenshots`
        // writes them, so the site and the README show the same pictures. The dev server only
        // serves files inside its root unless told otherwise.
        server: { fs: { allow: [".."] } },
    },
});
