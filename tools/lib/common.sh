# shellcheck shell=bash
# tools/lib/common.sh — shared helpers for the cross-build scripts.
#
# Sourced (not executed) by tools/build-*.sh to keep the Docker / data-symlink /
# version boilerplate in ONE place. Usage, after the script cd's to the repo
# root:
#
#   cd "$(dirname "$0")/.."
#   . tools/lib/common.sh
#
# Every function assumes the current directory is the repo root.

# ---- build version ---------------------------------------------------------
# Resolve the version string on the HOST, where git is present and the checkout
# is trusted. Inside a build container git is either absent or refuses the
# host-owned checkout ("dubious ownership"), so an in-container `git describe`
# silently degrades to "unknown" — which is what used to ship on the menu.
# Callers capture this and pass it into the container (WACKI_VERSION=...).
wacki_version() {
    git describe --tags --always --dirty 2>/dev/null || echo unknown
}

# ---- preflight checks ------------------------------------------------------
# data/WACKI.EXE is the build-time dependency for the embedded PE blob
# (tools/embed-pe-data reads its .rdata/.data). Fail fast + tell the user why.
require_data_exe() {
    if [ ! -f data/WACKI.EXE ]; then
        echo "error: data/WACKI.EXE missing — required for the embedded PE blob." >&2
        echo "       Drop the file in ./data/ before building." >&2
        exit 1
    fi
}

require_docker() {
    if ! command -v docker >/dev/null 2>&1; then
        echo "error: docker not installed — install Docker Desktop / docker.io first." >&2
        exit 1
    fi
}

# ---- data directory / Docker mount -----------------------------------------
# Echo the ABSOLUTE path of the real data directory, following the local-dev
# symlink (data → /path/outside/repo, which keeps the proprietary game files
# out of the source tree) when present. Empty output if data/ doesn't exist.
wacki_data_dir() {
    if [ -L data ]; then
        printf '%s/%s\n' \
            "$(cd "$(dirname "$(readlink data)")" && pwd)" \
            "$(basename "$(readlink data)")"
    elif [ -d data ]; then
        ( cd data && pwd )
    fi
}

# Populate the global array WACKI_DATA_MOUNT with the `docker run -v` args that
# make an in-tree `data` symlink resolve INSIDE the container: the symlink's
# absolute target is bind-mounted at its own path (read-only — the build only
# reads it). Empty array when data/ is a real directory (already covered by the
# workspace mount). Usage:
#
#   wacki_data_mount
#   docker run ... "${WACKI_DATA_MOUNT[@]}" ...
#
# Prints a one-line note to stderr when it adds a mount, mirroring the old
# per-script "[tag] data/ is a symlink — mounting …" messages.
# shellcheck disable=SC2034  # WACKI_DATA_MOUNT is a global consumed by callers
wacki_data_mount() {
    WACKI_DATA_MOUNT=()
    if [ -L data ]; then
        local target
        target="$(wacki_data_dir)"
        # Braces are required: bare "$target:ro" hits zsh's `:r` (remove-
        # extension) history modifier if this is ever sourced under zsh,
        # silently eating the ":r" of ":ro". ${target} stops the parse.
        if [ -n "$target" ] && [ -d "$target" ]; then
            WACKI_DATA_MOUNT=(-v "${target}:${target}:ro")
            echo "[build] data/ is a symlink — mounting target ${target} (ro)" >&2
        fi
    fi
}
