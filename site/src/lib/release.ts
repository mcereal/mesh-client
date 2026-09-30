// Where the downloads live and what the newest one is called.
//
// The links use GitHub's /releases/latest/download/<asset> form, which always redirects to the
// newest stable release, so they never go stale. Only the version label is read at build time,
// and a failed lookup leaves it out rather than failing the build.

export const REPO = "mcereal/mesh-client";
export const REPO_URL = `https://github.com/${REPO}`;
export const RELEASES_URL = `${REPO_URL}/releases`;
export const LATEST_URL = `${RELEASES_URL}/latest`;

export const asset = (name: string): string => `${LATEST_URL}/download/${name}`;

export interface Release {
    tag: string;
    published: Date;
    url: string;
}

let cached: Promise<Release | null> | undefined;

export function latestRelease(): Promise<Release | null> {
    cached ??= fetchLatest();
    return cached;
}

async function fetchLatest(): Promise<Release | null> {
    const headers: Record<string, string> = {
        Accept: "application/vnd.github+json",
        "User-Agent": "meshclient-site",
    };
    // Unauthenticated requests share a limit of 60 an hour per address, which a shared build
    // machine can exhaust. A token in the build environment lifts it.
    const token = process.env.GITHUB_TOKEN;
    if (token) headers.Authorization = `Bearer ${token}`;

    try {
        const res = await fetch(`https://api.github.com/repos/${REPO}/releases/latest`, {
            headers,
            signal: AbortSignal.timeout(10_000),
        });
        if (!res.ok) {
            console.warn(`[release] GitHub answered ${res.status}; building without a version`);
            return null;
        }
        const body = (await res.json()) as { tag_name: string; published_at: string; html_url: string };
        return { tag: body.tag_name, published: new Date(body.published_at), url: body.html_url };
    } catch (err) {
        console.warn(`[release] ${String(err)}; building without a version`);
        return null;
    }
}
