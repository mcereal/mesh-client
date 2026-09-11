# Wuffs

[Wuffs](https://github.com/google/wuffs) is a language that compiles to C, and this is the
single C file its authors publish as the distribution form of the library. The client uses one
thing out of it: the PNG decoder that turns a map tile into pixels.

| | |
|---|---|
| Upstream | https://github.com/google/wuffs |
| File | [`release/c/wuffs-v0.4.c`](https://github.com/google/wuffs/blob/0f214ba59c20c0c9c7ba841ecc3683f863965312/release/c/wuffs-v0.4.c) |
| Revision | `0f214ba59c20c0c9c7ba841ecc3683f863965312` |
| Version | `0.4.0-alpha.10+3966.20260623` |
| SHA-256 | `1f8039ef82911604c063f6ac2ed57254bdb17d742aebdeae06356530d4a0fde7` |
| Licence | Apache-2.0 or MIT, at your option |

## Why it is a file here and not a submodule

`third_party/nanopb` is a submodule and this is not, which is worth explaining rather than
leaving as an inconsistency. Wuffs *publishes* this file: the single-file C release is the
supported way to consume the library, not a build artifact of it. Taking it as a submodule would
clone the language, its compiler, its test data and every previous release - tens of megabytes -
to reach one 3.6 MB file that upstream already packages for exactly this use. nanopb's submodule
earns its keep differently: the build runs its *generator* out of the checkout.

The file is unmodified. `scripts/check-vendor.py` runs in `make test` and re-checks the digest
above, because a 3.6 MB generated file is precisely the kind of thing somebody patches in place
when a fix is needed - and a local edit that survives into a release is a fix nobody upstream
knows about and nobody here can find again. Fixes go upstream and come back as a new revision.

## Updating it

1. Pick the revision and download that file.
2. Put the new digest, revision and version in the table above.
3. `make test` - `check-vendor` fails until the two agree.
4. Re-read `src/map/wuffs_png.h`: the module list there is what keeps the other thirty codecs
   out of the binary, and upstream occasionally splits a module.

## What is compiled in

Not all of it. `src/map/wuffs_png.h` sets `WUFFS_CONFIG__MODULES` and then names ADLER32,
CRC32, DEFLATE, ZLIB, PNG and three of BASE's seven sub-modules, so the JPEG, GIF, BMP, WEBP,
CBOR and JSON decoders in this file are never compiled - checked by symbol count, not assumed:
the built object carries 56 `wuffs_png__` symbols and zero from any of the other twelve codecs.

**It costs more than the roadmap's measurement predicted, and the reason is the pak's build
rather than the decoder.** That measurement put this subset at +106 KB, built with
`-ffunction-sections -fdata-sections -Wl,--gc-sections`; `scripts/cross-build.sh` uses plain
`-Os` and none of those, so nothing gets dropped *within* an object and the whole compiled
subset ships. Measured on the host at `-Os`, the object is **335 KB of text** - 31 KB of which
naming BASE's sub-modules individually already saves, and the rest of which `--gc-sections`
would take back. See [`docs/maps-roadmap.md`](../../docs/maps-roadmap.md) for the numbers and
what is proposed about them.
