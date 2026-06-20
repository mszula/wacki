#!/bin/sh
# fetch-deps.sh — download + verify the vendored single-header dependencies.
#
# third_party/ is gitignored; this fetches PINNED, checksummed versions so the
# build is reproducible without committing third-party code into the repo.
#
# Idempotent: a file already present with the right sha256 is left untouched and
# NO network request is made, so this is cheap to run on every build and works
# fully offline once the deps are in place. A checksum mismatch aborts the build
# (supply-chain safety) rather than silently compiling unexpected code.
#
# To bump a dependency: change its ref in the URL base + its sha256 below, then
# run `make deps` (or just `make`) — the new file is fetched and verified.
set -eu

# Operate relative to assets-explorer/ regardless of the caller's cwd.
cd "$(dirname "$0")/.."

# sha256 tool: shasum (macOS/BSD) or sha256sum (GNU/Linux).
if command -v shasum >/dev/null 2>&1; then
    sha_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
elif command -v sha256sum >/dev/null 2>&1; then
    sha_of() { sha256sum "$1" | cut -d' ' -f1; }
else
    echo "fetch-deps: need 'shasum' or 'sha256sum' on PATH" >&2; exit 1
fi

# downloader: curl or wget.
if command -v curl >/dev/null 2>&1; then
    download() { curl -fsSL --retry 3 -o "$2" "$1"; }
elif command -v wget >/dev/null 2>&1; then
    download() { wget -qO "$2" "$1"; }
else
    echo "fetch-deps: need 'curl' or 'wget' on PATH" >&2; exit 1
fi

fetched=0
# dep PATH SHA256 URL — fetch into PATH if missing or wrong, verifying SHA256.
dep() {
    path=$1; want=$2; url=$3
    if [ -f "$path" ] && [ "$(sha_of "$path")" = "$want" ]; then
        return 0                      # already present & correct — skip (offline-safe)
    fi
    echo "  fetch $path"
    mkdir -p "$(dirname "$path")"
    tmp="$path.tmp.$$"
    download "$url" "$tmp"
    got=$(sha_of "$tmp")
    if [ "$got" != "$want" ]; then
        rm -f "$tmp"
        echo "fetch-deps: CHECKSUM MISMATCH for $path" >&2
        echo "  url      $url"  >&2
        echo "  expected $want" >&2
        echo "  got      $got"  >&2
        exit 1
    fi
    mv "$tmp" "$path"
    fetched=$((fetched + 1))
}

# ---- pinned dependencies ---------------------------------------------------
# Refs are immutable (release tag or full commit SHA); the sha256 is the real
# guarantee — moving a tag or a CDN swap fails the check instead of injecting.
NK=https://raw.githubusercontent.com/Immediate-Mode-UI/Nuklear/v4.13.3
STB=https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083
GIF=https://raw.githubusercontent.com/notnullnotvoid/msf_gif/a17d1109ddb8f5965d5a0c194991e094838a14a0
TFD=https://raw.githubusercontent.com/native-toolkit/tinyfiledialogs/cc6b593c029110af8045826ce691f540c85e850c

dep third_party/nuklear/nuklear.h              2e3b7c3f6528cd8e43aeae87e343fe9ca529b6f7380a99ebb63a41cf1d4a1552 "$NK/nuklear.h"
dep third_party/nuklear/nuklear_sdl_renderer.h 0aaf0cdc4c84170a6c3217e923ba6b4f5bbcd557916739d0a99a46aa61cc5901 "$NK/demo/sdl_renderer/nuklear_sdl_renderer.h"
dep third_party/stb/stb_image_write.h          cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05 "$STB/stb_image_write.h"
dep third_party/msf_gif/msf_gif.h              538513b65e93aba4e0a31be953d543f8d063ffe097dffb4d99b9c2b4d213cbf0 "$GIF/msf_gif.h"
dep third_party/tinyfd/tinyfiledialogs.h       598faa191a8505723be3d4884dda8b214f19b2f00b3db7cdfa323ddba7a8737e "$TFD/tinyfiledialogs.h"
dep third_party/tinyfd/tinyfiledialogs.c       59dcd02254a6dd443c9e0fb2e9260284b5fff498dae238eedf5409afd12bac6a "$TFD/tinyfiledialogs.c"

if [ "$fetched" -gt 0 ]; then
    echo "deps: fetched $fetched file(s), all checksums OK"
fi
