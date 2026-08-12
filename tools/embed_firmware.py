#!/usr/bin/env python3
"""Generate fwEmbeddedFirmware.{h,cpp} from firmware/manifest.json.

Deflates each UF2 with zlib (miniz inflates it at runtime with
TINFL_FLAG_PARSE_ZLIB_HEADER) and records size + SHA-256 so the app can verify
what it embedded. A missing image is a warning, not an error: only release
builds need the real files.

This script is invoked by an ALWAYS-RUN build step (see cmake/EmbedFirmware.cmake
for why), so it owns its own up-to-date check and it has to be cheap when there
is nothing to do:

  * It hashes its inputs into a stamp file and returns immediately, having
    deflated nothing, when the stamp still matches. SHA-256 over the ~21 MB of
    images costs ~0.04 s; deflating them at level 9 costs ~9 s. The whole point
    of the stamp is to pay the first and not the second.
  * It writes fwEmbeddedFirmware.{h,cpp} only when their content actually
    changes. They are compiled into fwog_core, so touching them on every build
    would recompile a 32 MB translation unit and relink every target -- which is
    exactly the cost the always-run design has to avoid to be worth having.
  * The stamp is written LAST. An interrupted or failed run therefore leaves a
    stamp that does not match, and the next build regenerates rather than
    trusting half-written output.

Usage: embed_firmware.py <firmware-dir> <out-dir>
"""
import hashlib
import json
import sys
import zlib
from pathlib import Path

# Bump when the shape of the emitted C++ changes in a way that existing outputs
# would not reflect. Included in the stamp, so a format change regenerates even
# when every input image is byte-identical. (Hashing this script's own bytes,
# below, covers that too; the tag is here so the reason is legible in the stamp
# file when someone opens it.)
GENERATOR_ID = "fwog-embed-firmware/2"


def sha256_file(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def input_stamp(script_path, fw_dir, manifest_path, manifest):
    """Hash every byte this generator reads, in a fixed order.

    Deliberately CONTENT, not timestamps: the failure this exists to catch is an
    image replaced in place with an mtime older than the generated blob, which
    every mtime-based dependency in CMake and ninja reports as up to date.

    The set is exactly what generate() reads -- this script, manifest.json, and
    each image manifest.json names -- so there are no false positives (an
    unrelated .uf2 dropped in the directory changes no output and forces no
    work) and no false negatives (an image the manifest names is hashed whether
    or not it lives in the firmware directory, which is how ../probe/probe.uf2
    is covered).
    """
    lines = [GENERATOR_ID, "generator " + sha256_file(script_path)]
    lines.append("manifest " + (sha256_file(manifest_path)
                                if manifest_path.is_file() else "MISSING"))
    for img in manifest.get("images", []):
        path = fw_dir / img["file"]
        lines.append("image {} {}".format(
            img["file"], sha256_file(path) if path.is_file() else "MISSING"))
    return "\n".join(lines) + "\n"


def write_if_different(path, text):
    """Write only on a real content change; report whether it wrote.

    Returning early keeps the file's mtime where it was, which is what stops an
    always-run generator from recompiling and relinking everything downstream.
    """
    data = text.encode()
    try:
        if path.read_bytes() == data:
            return False
    except OSError:
        pass
    path.write_bytes(data)
    return True


def c_array(name, data):
    out = [f"static const unsigned char {name}[] = {{"]
    for i in range(0, len(data), 16):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i:i + 16])
        out.append("    " + chunk + ",")
    out.append("};")
    return "\n".join(out)


def main():
    script_path = Path(__file__).resolve()
    fw_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    manifest_path = fw_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}

    header = out_dir / "fwEmbeddedFirmware.h"
    source = out_dir / "fwEmbeddedFirmware.cpp"
    stamp = out_dir / "fwEmbeddedFirmware.stamp"

    want_stamp = input_stamp(script_path, fw_dir, manifest_path, manifest)
    # Both outputs have to exist as well as the stamp matching: a stamp alone
    # would happily declare a deleted fwEmbeddedFirmware.cpp up to date.
    if (header.is_file() and source.is_file() and stamp.is_file()
            and stamp.read_text() == want_stamp):
        return

    blobs, meta = [], []
    available = set()
    for img in manifest.get("images", []):
        # "file" is resolved relative to the firmware directory and MAY reach
        # outside it. Exactly one image does: "fwog_probe" lives at
        # ../probe/probe.uf2, because it is the only .uf2 in this repo that is
        # committed to git and it belongs beside the probe/probe.c it was built
        # from, not in the directory whose contents are deliberately absent (see
        # .gitignore). manifest.json is a source file in this repository -- it
        # never comes from a catalog or a download -- so a relative path here is
        # a repo-layout statement, not untrusted input.
        path = fw_dir / img["file"]
        if not path.exists():
            print(f"warning: {path} not found; skipping embedded image "
                  f"{img['id']!r}", file=sys.stderr)
            continue
        raw = path.read_bytes()
        sym = "k_" + img["id"].replace("-", "_")
        blobs.append(c_array(sym, zlib.compress(raw, 9)))
        meta.append({
            "id": img["id"], "sym": sym, "version": img.get("version", ""),
            "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest(),
        })
        available.add(img["id"])

    # Drop entries whose images are not all present, so the app never offers a
    # firmware it cannot actually write.
    entries = [e for e in manifest.get("entries", [])
               if all(a["id"] in available for a in e.get("uf2", []))]

    # parseCatalogJson (Task 7) reads "embeddedId" inside each uf2 object, not
    # "id" -- rewrite the key so the generated catalog is resolvable.
    for e in entries:
        for a in e.get("uf2", []):
            if "id" in a:
                a["embeddedId"] = a.pop("id")

    wrote = []
    if write_if_different(header,
        "// Generated by tools/embed_firmware.py. Do not edit.\n"
        "#pragma once\n"
        "#include <cstddef>\n#include <cstdint>\n#include <string>\n#include <vector>\n\n"
        "namespace fwog::generated {\n\n"
        "struct EmbeddedImage {\n"
        "    const char*          id;\n"
        "    const char*          version;\n"
        "    const unsigned char* deflated;\n"
        "    std::size_t          deflatedSize;\n"
        "    std::size_t          rawSize;\n"
        "    const char*          sha256;\n"
        "};\n\n"
        "extern const EmbeddedImage* const kImages;\n"
        "extern const std::size_t         kImageCount;\n"
        "/// firmware/manifest.json's \"entries\" array, verbatim.\n"
        "extern const char* const         kEntriesJson;\n\n"
        "} // namespace fwog::generated\n"):
        wrote.append(header.name)

    body = ["// Generated by tools/embed_firmware.py. Do not edit.",
            '#include "fwEmbeddedFirmware.h"', "",
            "namespace fwog::generated {", "namespace {", ""]
    body += blobs
    body += ["", "const EmbeddedImage kTable[] = {"]
    for m in meta:
        body.append(f'    {{ "{m["id"]}", "{m["version"]}", {m["sym"]}, '
                    f'sizeof({m["sym"]}), {m["size"]}, "{m["sha256"]}" }},')
    if not meta:
        body.append("    { nullptr, nullptr, nullptr, 0, 0, nullptr },")
    body += ["};", "", "} // namespace", ""]

    entries_json = json.dumps({"apps": entries}, separators=(",", ":"))
    escaped = entries_json.replace("\\", "\\\\").replace('"', '\\"')

    body += [f"const EmbeddedImage* const kImages = kTable;",
             f"const std::size_t kImageCount = {len(meta)};",
             f'const char* const kEntriesJson = "{escaped}";', "",
             "} // namespace fwog::generated"]

    if write_if_different(source, "\n".join(body) + "\n"):
        wrote.append(source.name)

    # LAST, and only now: everything above succeeded, so this stamp describes
    # output that is actually on disk.
    stamp.write_text(want_stamp)
    print(f"embedded {len(meta)} image(s), {len(entries)} entry(ies); "
          + (f"wrote {', '.join(wrote)}" if wrote
             else "generated output unchanged"))


if __name__ == "__main__":
    main()
