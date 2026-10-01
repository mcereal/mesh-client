// @ts-check
import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";

// A static build: every page is HTML at build time and Cloudflare serves the files as they are.
// No adapter, because nothing here renders on request.
//
// The home and download pages are plain Astro pages under src/pages/. The guide is Starlight,
// from the Markdown under src/content/docs/guide/, which gives it the sidebar and the search.
export default defineConfig({
    site: "https://meshclient.dev",
    integrations: [
        starlight({
            title: "MeshClient",
            // The site has its own 404 page under src/pages/, in the home page's layout.
            disable404Route: true,
            description: "How to install, connect and use MeshClient.",
            logo: { src: "./public/favicon.svg", alt: "" },
            favicon: "/favicon.svg",
            social: [{ icon: "github", label: "GitHub", href: "https://github.com/mcereal/mesh-client" }],
            editLink: { baseUrl: "https://github.com/mcereal/mesh-client/edit/main/site/" },
            customCss: ["./src/styles/starlight.css"],
            sidebar: [
                {
                    label: "Start here",
                    items: ["guide", "guide/install", "guide/connecting", "guide/controls"],
                },
                {
                    label: "Using it",
                    items: [
                        "guide/messages",
                        "guide/nodes",
                        "guide/map",
                        "guide/radio",
                        "guide/settings",
                        "guide/sharing",
                    ],
                },
                {
                    label: "More",
                    items: ["guide/meshcore", "guide/mqtt", "guide/cli", "guide/troubleshooting"],
                },
            ],
        }),
    ],
    vite: {
        // The screenshots are imported from ../.github/resources/, where `make screenshots`
        // writes them, so the site and the README show the same pictures. The dev server only
        // serves files inside its root unless told otherwise.
        server: { fs: { allow: [".."] } },
    },
});
