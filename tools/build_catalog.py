#!/usr/bin/env python3
"""Build the published remote catalog: apps.json plus the .uf2 files beside it.

The output of this script is a directory tree that is copied verbatim into the
documentation site's static assets, so that

    https://docs.freewili.com/og-apps/apps.json

is the catalog the app fetches and

    https://docs.freewili.com/og-apps/uf2/<name>.uf2

are the images its entries point at. See publish/README.md for the upload step.

WHAT IS AUTHORED AND WHAT IS DERIVED
------------------------------------
Almost nothing about an app is written by hand. Every OG main image carries
`fwog_uf2_info_t` records under the wiliOGbsp contract -- one for the MAIN CPU
and one for the display image embedded inside it -- and those records already
hold the app's name, its description, its version and the git description of
the build. Reading them here means the published catalog says exactly what the
bytes say, and cannot drift from them when an app is rebuilt.

publish/sources.json therefore carries only what an image genuinely cannot know
about itself: which apps to publish at all, and the storefront metadata around
them (category, author, repository, tags).

WHY THE VALIDATION IS A COPY OF THE APP'S
-----------------------------------------
`check_uf2()` below reimplements src/catalog/fwUf2Header.cpp's parseUf2() rule
for rule. That duplication is deliberate. The app refuses to flash an image
that fails those checks, and a catalog that publishes one produces a download
that always fails at the last step -- after the user has picked an app, watched
it download, and been told to put their board in bootloader mode. Failing here,
at publish time, with the file name in the message, is the whole point.

Run:  python tools/build_catalog.py [--sources publish/sources.json]
                                    [--out publish/out]
                                    [--base-url https://.../og-apps/]
"""

import argparse
import datetime
import hashlib
import json
import pathlib
import shutil
import struct
import sys

# --- UF2 container, from the RP2040/pico-sdk definition ---------------------
UF2_BLOCK = 512
UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_PRESENT = 0x00002000
UF2_MAX_PAYLOAD = 476
FAMILY_RP2040 = 0xE48BFF56

# --- fwog_uf2_info_t, the OG app record -------------------------------------
# Layout mirrored from src/catalog/fwOgAppInfo.h, which derived it from real
# images. Every field is bounds-checked there and here for the same reason: it
# is an inference from shipped binaries, not a published header.
INFO_MAGIC = b"FWGOINFO"
INFO_SIZE = 0xD8
INFO_CPU_DISPLAY = 0
INFO_CPU_MAIN = 1


class BuildError(Exception):
    """A reason this catalog must not be published, phrased for a human."""


def check_uf2(data: bytes, label: str) -> None:
    """Raise BuildError unless `data` is an image the app would accept.

    Mirrors parseUf2() (src/catalog/fwUf2Header.cpp): whole 512-byte blocks,
    both magics plus the end magic on every block, sequential block numbers, a
    declared count that matches the file, a payload that fits, and -- where a
    family ID is present at all -- RP2040 on every block, not merely the first.
    """
    if not data:
        raise BuildError(f"{label}: the file is empty")
    if len(data) % UF2_BLOCK:
        raise BuildError(
            f"{label}: not a whole number of 512-byte UF2 blocks "
            f"({len(data)} bytes); it is probably truncated"
        )

    actual_blocks = len(data) // UF2_BLOCK
    first_family_present = None
    first_family_id = None

    for i in range(actual_blocks):
        b = i * UF2_BLOCK
        magic0, magic1 = struct.unpack_from("<II", data, b)
        (magic_end,) = struct.unpack_from("<I", data, b + 508)
        if magic0 != UF2_MAGIC0 or magic1 != UF2_MAGIC1 or magic_end != UF2_MAGIC_END:
            raise BuildError(f"{label}: block {i} is not a UF2 block (magic missing)")

        flags, _addr, payload_size, block_no, num_blocks, family = struct.unpack_from(
            "<IIIIII", data, b + 8
        )
        if payload_size > UF2_MAX_PAYLOAD:
            raise BuildError(
                f"{label}: block {i} declares {payload_size} payload bytes, "
                f"more than the {UF2_MAX_PAYLOAD} a block can hold"
            )
        if block_no != i:
            raise BuildError(
                f"{label}: blocks are out of order (block {i} says it is {block_no}); "
                "the file is corrupt"
            )
        if num_blocks != actual_blocks:
            raise BuildError(
                f"{label}: the block count disagrees with the file size "
                f"({num_blocks} declared, {actual_blocks} actual); the file is "
                "corrupt or incomplete"
            )

        family_present = bool(flags & UF2_FLAG_FAMILY_PRESENT)
        family_id = family if family_present else None

        if i == 0:
            first_family_present, first_family_id = family_present, family_id
            if family_present and family_id != FAMILY_RP2040:
                raise BuildError(
                    f"{label}: family ID 0x{family_id:08X} is not RP2040 "
                    f"(0x{FAMILY_RP2040:08X}); the FreeWili OG cannot run this image"
                )
        elif family_present != first_family_present or (
            first_family_present and family_id != first_family_id
        ):
            # A spliced file -- a good block 0 followed by blocks from another
            # image -- must not pass just because block 0 was checked.
            raise BuildError(
                f"{label}: block {i} belongs to a different chip family than block 0; "
                "the file is spliced or corrupt"
            )


def flatten_payload(data: bytes) -> bytes:
    """Concatenate every block's payload into the flat image it describes.

    The records are searched in THIS, not in the .uf2 file's raw bytes: a
    record can straddle a block boundary, and the file's own headers would
    otherwise be searched too. Same reasoning as flattenUf2Payload() in
    src/catalog/fwOgAppInfo.cpp.
    """
    out = bytearray()
    for b in range(0, len(data), UF2_BLOCK):
        (payload_size,) = struct.unpack_from("<I", data, b + 16)
        out += data[b + 32 : b + 32 + payload_size]
    return bytes(out)


def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def find_app_records(image: bytes) -> list[dict]:
    """Every fwog_uf2_info_t in a flattened image, in the order they appear."""
    records = []
    at = image.find(INFO_MAGIC)
    while at >= 0:
        if at + INFO_SIZE <= len(image):
            fmt_version, cpu = struct.unpack_from("<HH", image, at + 8)
            (version,) = struct.unpack_from("<I", image, at + 0x0C)
            build_ts, crc = struct.unpack_from("<II", image, at + 0xD0)
            records.append(
                {
                    "cpu": "main" if cpu == INFO_CPU_MAIN else "display",
                    "formatVersion": fmt_version,
                    "version": version,
                    "name": _cstr(image[at + 0x10 : at + 0x30]),
                    "description": _cstr(image[at + 0x30 : at + 0xB0]),
                    "build": _cstr(image[at + 0xB0 : at + 0xD0]),
                    "buildTimestamp": build_ts,
                    "crc32": crc,
                }
            )
        at = image.find(INFO_MAGIC, at + 1)
    return records


def format_version(version: int) -> str:
    """"001" for 1, matching formatOgAppVersion() in the app.

    A larger number is printed in full rather than truncated to three digits.
    """
    return f"{version:03d}"


def build_entry(app: dict, base_url: str, out_uf2_dir: pathlib.Path,
                repo_root: pathlib.Path) -> dict:
    """One apps.json entry, from one authored source app plus its image."""
    slug = app.get("slug", "")
    if not slug:
        raise BuildError("an entry in sources.json has no \"slug\"")

    source = app.get("source", "")
    if not source:
        raise BuildError(f"{slug}: no \"source\" path to a .uf2")

    # Relative paths resolve against the repo root, not the working directory,
    # so the tool gives the same answer wherever it is run from.
    path = pathlib.Path(source)
    if not path.is_absolute():
        path = repo_root / path
    if not path.is_file():
        raise BuildError(f"{slug}: no such file: {path}")

    data = path.read_bytes()
    check_uf2(data, f"{slug} ({path.name})")

    records = find_app_records(flatten_payload(data))
    main = next((r for r in records if r["cpu"] == "main"), None)
    if main is None:
        # An image with no MAIN record is either not an OG app or is a display
        # image somebody tried to publish on its own -- and a display image
        # written to the MAIN CPU is exactly the mistake this project exists to
        # prevent. Refuse rather than publish an entry that guesses.
        raise BuildError(
            f"{slug}: {path.name} carries no MAIN-CPU app record, so it is not an "
            "OG main image. Publish the <app>_main.uf2 that the OG build produces; "
            "a display image is delivered inside it, never on its own."
        )
    if main["formatVersion"] != 1:
        raise BuildError(
            f"{slug}: app record format version {main['formatVersion']} is not 1, "
            "which is the only layout this tool knows how to read"
        )

    display = next((r for r in records if r["cpu"] == "display"), None)

    # Published under the slug rather than under the source file's own name:
    # two different apps built from the same template can both produce
    # `template_main.uf2`, and the published tree is flat.
    filename = f"{slug}_main.uf2"
    (out_uf2_dir / filename).write_bytes(data)

    entry = {
        "slug": slug,
        # The image's own name is the fallback, not the override: a record
        # carries a C identifier ("orca_catalog") where a storefront wants
        # "Orca Field Notes".
        "name": app.get("name") or main["name"],
        # The DISPLAY record first, and this is not arbitrary. The two records
        # are written for different readers: the MAIN one describes the program
        # on the main CPU, which for an OG app is very often just the carrier
        # ("Carries the Orca Field Notes display firmware"), while the DISPLAY
        # one describes what the user will actually see ("72 sourced stories,
        # QR links, and natural-history audio"). A storefront one-liner wants
        # the second. Falls back to the MAIN record for a main-only app, which
        # has no display record at all.
        "tagline": app.get("tagline") or _tagline(main, display),
        # The long description keeps BOTH records: each is true and neither is
        # the whole story of what gets flashed.
        "description": app.get("description") or _describe(main, display),
        "author": app.get("author", ""),
        "github": app.get("github", ""),
        "category": app.get("category", "Apps"),
        "tags": app.get("tags", []),
        "version": format_version(main["version"]),
        "updated": _date_of(main["buildTimestamp"]),
        # Always stated, never left to the app to infer. An absent flashScheme
        # is defaulted to OgApp by the parser and marked "inferred", which the
        # UI then tells the user about -- correct behaviour for a catalog
        # somebody else wrote, and a needless caveat on this one.
        "flashScheme": "OgApp",
        "uf2": [
            {
                "cpu": "main",
                "url": base_url + "uf2/" + filename,
                "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data),
            }
        ],
    }
    return entry


def _sentence(text: str) -> str:
    """`text` with terminal punctuation, so fragments can be joined.

    The records hold sentence FRAGMENTS -- "Pomodoro timer: themed countdown,
    LED progress ring, chimes and tilt-to-pause" -- with no full stop, because
    each is displayed on its own. Concatenating them raw runs two sentences
    together ("...display firmware On the display: ..."), which is how the
    first generated catalog read.
    """
    text = text.strip()
    if not text:
        return ""
    return text if text[-1] in ".!?" else text + "."


def _tagline(main: dict, display: dict | None) -> str:
    """The one-line summary: what the user will see, not what carries it."""
    if display and display["description"]:
        return display["description"].strip()
    return main["description"].strip()


def _describe(main: dict, display: dict | None) -> str:
    """The long description, from the image's own records.

    The two records say different things -- the MAIN one describes the program
    that runs on the main CPU, the display one describes what appears on the
    screen -- so both are kept when they differ, and the build identity is
    appended because it is the only thing that distinguishes two builds of the
    same version number.
    """
    parts = [_sentence(main["description"])]
    if display and display["description"] and display["description"] != main["description"]:
        parts.append("On the display: " + _sentence(display["description"]))
    if main["build"]:
        parts.append(f"Built from {main['build']}.")
    return " ".join(p for p in parts if p)


def _date_of(unix_seconds: int) -> str:
    """YYYY-MM-DD in UTC, or "" when the record carries no timestamp.

    UTC and not local time: the catalog is published once and read everywhere,
    so the date must not depend on which machine built it.
    """
    if not unix_seconds:
        return ""
    return datetime.datetime.fromtimestamp(
        unix_seconds, datetime.timezone.utc
    ).strftime("%Y-%m-%d")


def main(argv: list[str]) -> int:
    repo_root = pathlib.Path(__file__).resolve().parent.parent

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sources", default=str(repo_root / "publish" / "sources.json"),
                    help="authored catalog input (default: publish/sources.json)")
    ap.add_argument("--out", default=str(repo_root / "publish" / "out"),
                    help="output directory (default: publish/out)")
    ap.add_argument("--base-url", default=None,
                    help="override the baseUrl in sources.json; useful for "
                         "serving the tree from a local test server")
    args = ap.parse_args(argv)

    sources_path = pathlib.Path(args.sources)
    try:
        sources = json.loads(sources_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        print(f"error: no such file: {sources_path}", file=sys.stderr)
        return 2
    except json.JSONDecodeError as e:
        print(f"error: {sources_path} is not valid JSON: {e}", file=sys.stderr)
        return 2

    base_url = args.base_url or sources.get("baseUrl", "")
    if not base_url:
        print("error: no baseUrl in sources.json and none given with --base-url",
              file=sys.stderr)
        return 2
    # Every URL in the output is base + "uf2/...", so a missing separator would
    # silently produce ".../og-appsuf2/x.uf2".
    if not base_url.endswith("/"):
        base_url += "/"
    if not base_url.startswith("https://"):
        # The app refuses a plain-http catalog URL outright
        # (normalizeRemoteCatalogUrl), and the image URLs inside it decide
        # which CPU gets written. Catch it here rather than at fetch time.
        print(f"error: baseUrl must be https://, got {base_url!r}", file=sys.stderr)
        return 2

    site_dir = pathlib.Path(args.out) / "og-apps"
    uf2_dir = site_dir / "uf2"
    # Rebuilt from scratch, so an app dropped from sources.json also disappears
    # from the tree that gets uploaded. Leaving stale .uf2 files behind would
    # publish images no catalog entry mentions and nothing ever removes.
    if site_dir.exists():
        shutil.rmtree(site_dir)
    uf2_dir.mkdir(parents=True)

    entries = []
    problems = []
    for app in sources.get("apps", []):
        try:
            entries.append(build_entry(app, base_url, uf2_dir, repo_root))
        except BuildError as e:
            problems.append(str(e))

    if problems:
        # Every problem, not just the first: a maintainer fixing three stale
        # source paths should learn about all three in one run.
        print("error: the catalog was not written.", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        shutil.rmtree(site_dir, ignore_errors=True)
        return 1

    if not entries:
        print("error: sources.json lists no apps", file=sys.stderr)
        shutil.rmtree(site_dir, ignore_errors=True)
        return 1

    doc = {"apps": entries}
    apps_json = site_dir / "apps.json"
    apps_json.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")

    total = sum(e["uf2"][0]["size"] for e in entries)
    print(f"wrote {apps_json}")
    for e in entries:
        print(f"  {e['slug']:<16} v{e['version']}  {e['uf2'][0]['size']:>9,} bytes  "
              f"{e['uf2'][0]['sha256'][:16]}...")
    print(f"{len(entries)} apps, {total:,} bytes of images, base {base_url}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
