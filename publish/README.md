# Publishing the remote catalog

The app ships pointed at

```
https://docs.freewili.com/og-apps/apps.json
```

(`defaultRemoteCatalogUrl()`, `src/core/fwSettingsIo.cpp`). This directory is
how that URL, and the `.uf2` files its entries link to, get produced.

```
publish/
  sources.json    which apps to publish, and the storefront metadata  (committed)
  README.md       this file                                            (committed)
  out/            the generated tree, ready to upload             (gitignored)
    og-apps/
      apps.json
      uf2/*.uf2
```

## Build it

```powershell
python tools/build_catalog.py
```

That reads `sources.json`, reads each app's `.uf2`, and writes `out/og-apps/`.
`out/og-apps` is deleted and rebuilt on every run, so an app removed from
`sources.json` also disappears from the tree that gets uploaded — a stale image
nothing links to is exactly what nobody would ever notice was still there.

Almost nothing in `apps.json` is hand-written. Every OG main image carries
`fwog_uf2_info_t` records (the wiliOGbsp contract: one for the MAIN CPU, one
for the display image travelling inside it), and the tool reads the name,
description, version and build identity straight out of them, then computes
`sha256` and `size` from the published bytes. Rebuild an app, re-run the tool,
and the catalog says what the new bytes say. `sources.json` carries only what
an image cannot know about itself: which apps to publish, and their category,
author, repository and tags.

The tool refuses to write anything if any app fails — and it reports **every**
failure, not just the first. It re-implements `parseUf2()`'s validation
(`src/catalog/fwUf2Header.cpp`) so that an image the app would refuse to flash
is caught here, at publish time, rather than after a user has chosen an app,
waited for the download and put their board into bootloader mode.

It also refuses an image with no MAIN-CPU record. A display image published on
its own would be a catalog entry that writes display firmware to the MAIN CPU,
which is the class of mistake this whole application exists to prevent.

## Upload it

Copy `publish/out/og-apps/` into the documentation site's static assets, so it
is served at the site root:

```
docs.freewili.com/og-apps/apps.json
docs.freewili.com/og-apps/uf2/ogvegas_main.uf2
...
```

On the Docusaurus site that is `static/og-apps/` — everything under `static/`
is copied to the site root verbatim at build time, with no routing involved.
Deploy the site as usual.

Two things the server has to get right:

* **HTTPS.** The app refuses a plain-`http://` catalog URL outright, and will
  not follow a redirect from `https://` down to `http://`
  (`normalizeRemoteCatalogUrl()`, `mayRedirectToPlainHttp()`). A catalog entry
  decides which of the board's two CPUs gets written, so it is not something
  anything in the middle may rewrite.
* **`.uf2` served as bytes.** The app downloads the image with the same HTTP
  GET it uses for the JSON and hashes what comes back against the `sha256` in
  the catalog. Any transformation in transit — content rewriting, an HTML error
  page returned with a 200 — fails the hash rather than reaching the board,
  which is the intended outcome, but it looks to the user like a corrupt
  download rather than a misconfigured server.

## Check it before you ship it

Serve the generated tree locally and point the app at it, which exercises the
real fetch, the real download and the real hash check without touching the live
site:

```powershell
python tools/build_catalog.py --base-url https://localhost:8443/og-apps/
python -m http.server 8000 --directory publish/out    # see the note below
```

The app will not accept an `http://` catalog URL, so a plain
`python -m http.server` is enough to eyeball `apps.json` in a browser but
cannot be pointed at from the Settings tab. To drive the app end to end you
need TLS — a self-signed certificate the OS trust store accepts, or a tunnel.
Failing that, verify the JSON and the hashes directly:

```powershell
python -c "import json,hashlib,pathlib; d=json.load(open('publish/out/og-apps/apps.json')); [print(a['slug'], hashlib.sha256(pathlib.Path('publish/out/og-apps/uf2',a['uf2'][0]['url'].rsplit('/',1)[1]).read_bytes()).hexdigest()==a['uf2'][0]['sha256']) for a in d['apps']]"
```

## Adding an app

Add an object to `sources.json`:

```json
{
  "slug": "my-app",
  "source": "C:/path/to/my_app_main.uf2",
  "name": "My App",
  "category": "Apps",
  "author": "Somebody",
  "github": "https://github.com/somebody/my-app",
  "tags": ["example"]
}
```

`source` is the `<app>_main.uf2` the OG build produces; a relative path
resolves against the repository root, an absolute one is taken as given (the
app repositories are separate checkouts and are not vendored here). `slug` must
be unique — it names the published file as well as the entry.

`name` is worth overriding, because the record usually holds a C identifier
(`orca_catalog`) where the storefront wants prose (`Orca Field Notes`). Leave
`tagline`, `description`, `version` and `updated` alone unless you have a
reason: derived from the image, they cannot drift from it.
