#!/usr/bin/env bash

set -Eeuo pipefail
umask 022

readonly ASSET_NAME="hyprspaces-waybar-full-x86_64.tar.zst"
readonly -a REQUIRED_TREE_PATHS=(
    "usr/bin/waybar"
    "usr/lib/systemd/user/waybar.service"
    "etc/xdg/waybar/config.jsonc"
    "etc/xdg/waybar/style.css"
    "usr/share/man/man5/waybar.5"
    "usr/share/man/man5/waybar-hyprland-workspaces.5"
)

log() {
    printf '==> %s\n' "$*" >&2
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<EOF
Usage: $(basename -- "$0") [options]

Build, test and package hyprspaces-waybar.

Options:
  --source-dir DIR   Checked-out Waybar source (default: repository root)
  --source-ref REF   Expected source tag or commit (default: HEAD)
  --output-dir DIR   Asset output directory (default: SOURCE_DIR/dist)
  --jobs COUNT       Parallel build jobs (default: available CPUs)
  -h, --help         Show this help
EOF
}

require_value() {
    local option="$1"
    local value="${2-}"

    [[ -n "$value" ]] || die "$option requires a value"
}

require_commands() {
    local command_name
    local -a missing=()

    for command_name in "$@"; do
        if ! command -v "$command_name" >/dev/null 2>&1; then
            missing+=("$command_name")
        fi
    done

    ((${#missing[@]} == 0)) || die "missing required commands: ${missing[*]}"
}

validate_install_tree() {
    local stage_dir="$1"
    local binary="$stage_dir/usr/bin/waybar"
    local service="$stage_dir/usr/lib/systemd/user/waybar.service"
    local strings_file="$2"
    local ldd_output
    local needed_libraries
    local version_output
    local invalid_entry
    local key
    local path
    local -a service_commands=()
    local -a required_keys=(
        "hyprspaces-dynamic-workspaces"
        "hyprspaces-workspace-count"
        "hyprspaces-special-overlay"
    )

    invalid_entry=$(find "$stage_dir" -mindepth 1 ! -type d ! -type f -print -quit)
    [[ -z "$invalid_entry" ]] || die "install tree contains non-file entry: $invalid_entry"

    invalid_entry=$(find "$stage_dir" -type f -links +1 -print -quit)
    [[ -z "$invalid_entry" ]] || die "install tree contains hard-linked file: $invalid_entry"

    invalid_entry=$(find "$stage_dir" -type d ! -perm 0755 -print -quit)
    [[ -z "$invalid_entry" ]] || die "install tree contains directory with unexpected mode: $invalid_entry"

    invalid_entry=$(find "$stage_dir" -type f ! -path "$binary" ! -perm 0644 -print -quit)
    [[ -z "$invalid_entry" ]] || die "install tree contains data file with unexpected mode: $invalid_entry"

    for path in "${REQUIRED_TREE_PATHS[@]}"; do
        [[ -f "$stage_dir/$path" && ! -L "$stage_dir/$path" ]] || \
            die "install tree is missing regular file $path"
    done

    [[ -x "$binary" ]] || die "installed Waybar is not executable"
    [[ "$(stat -c '%a' "$binary")" == "755" ]] || die "installed Waybar mode must be 755"

    mapfile -t service_commands < <(grep '^ExecStart=' "$service" || true)
    ((${#service_commands[@]} == 1)) || die "Waybar service must contain one ExecStart"
    [[ "${service_commands[0]}" == "ExecStart=/usr/bin/waybar" ]] || \
        die "Waybar service does not launch /usr/bin/waybar"

    strings "$binary" >"$strings_file"
    for key in "${required_keys[@]}"; do
        grep -Fqx -- "$key" "$strings_file" || die "binary is missing hyprspaces-waybar key: $key"
    done

    version_output=$(env \
        -u HYPRLAND_INSTANCE_SIGNATURE \
        -u SWAYSOCK \
        -u WAYLAND_DISPLAY \
        -u WAYLAND_SOCKET \
        "$binary" --version 2>&1) || die "installed Waybar cannot report its version"
    [[ -n "$version_output" ]] || die "installed Waybar returned an empty version"
    log "Waybar version: $version_output"

    needed_libraries=$(readelf -d "$binary" | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
    if grep -Eq '^lib(cava|epoxy|gps)([.]so|$)' <<<"$needed_libraries"; then
        die "binary has forbidden libcava, libepoxy or libgps DT_NEEDED entry"
    fi

    ldd_output=$(ldd "$binary" 2>&1) || die "ldd failed for installed Waybar"
    if grep -Fq 'not found' <<<"$ldd_output"; then
        printf '%s\n' "$ldd_output" >&2
        die "installed Waybar has unresolved runtime libraries"
    fi
}

validate_archive() {
    local archive="$1"
    local listing="$2"
    local verbose_listing="${listing}.verbose"
    local invalid_entries
    local path

    zstd -dc -- "$archive" | tar -tf - >"$listing"
    zstd -dc -- "$archive" | tar --numeric-owner -tvf - >"$verbose_listing"

    if grep -Eq '(^/|(^|/)[.][.](/|$))' "$listing"; then
        die "archive contains an unsafe path"
    fi

    for path in "${REQUIRED_TREE_PATHS[@]}"; do
        grep -Fqx -- "$path" "$listing" || die "archive is missing $path"
    done

    invalid_entries=$(awk '
        substr($1, 1, 1) == "d" {
            if ($1 != "drwxr-xr-x") print
            next
        }
        substr($1, 1, 1) == "-" {
            expected = ($NF == "usr/bin/waybar") ? "-rwxr-xr-x" : "-rw-r--r--"
            if ($1 != expected) print
            next
        }
        { print }
    ' "$verbose_listing")
    if [[ -n "$invalid_entries" ]]; then
        printf '%s\n' "$invalid_entries" >&2
        die "archive contains invalid entry type or mode"
    fi
}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
default_source_dir=$(cd -- "$script_dir/../.." && pwd -P)
source_dir="$default_source_dir"
source_ref="HEAD"
output_dir=""
jobs="${JOBS:-$(nproc)}"

while (($# > 0)); do
    case "$1" in
        --source-dir)
            require_value "$1" "${2-}"
            source_dir="$2"
            shift 2
            ;;
        --source-ref)
            require_value "$1" "${2-}"
            source_ref="$2"
            shift 2
            ;;
        --output-dir)
            require_value "$1" "${2-}"
            output_dir="$2"
            shift 2
            ;;
        --jobs)
            require_value "$1" "${2-}"
            jobs="$2"
            shift 2
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die "--jobs must be a positive integer"
[[ "$source_ref" =~ ^[A-Za-z0-9][A-Za-z0-9._/@+-]*$ ]] || die "invalid source ref: $source_ref"
[[ "$(uname -m)" == "x86_64" ]] || die "release asset requires an x86_64 build host"

require_commands awk find git grep ldd meson ninja nproc readelf sed sha256sum stat strings tar zstd

[[ -d "$source_dir" ]] || die "source directory does not exist: $source_dir"
source_dir=$(cd -- "$source_dir" && pwd -P)
[[ -f "$source_dir/meson.build" ]] || die "source directory has no meson.build: $source_dir"

source_commit=$(git -C "$source_dir" rev-parse --verify "${source_ref}^{commit}" 2>/dev/null) || \
    die "source ref does not resolve to a commit: $source_ref"
head_commit=$(git -C "$source_dir" rev-parse --verify 'HEAD^{commit}' 2>/dev/null) || \
    die "source directory is not a Git checkout: $source_dir"
[[ "$source_commit" == "$head_commit" ]] || \
    die "source checkout $head_commit does not match $source_ref ($source_commit)"

if [[ -z "$output_dir" ]]; then
    output_dir="$source_dir/dist"
fi
mkdir -p -- "$output_dir"
output_dir=$(cd -- "$output_dir" && pwd -P)

asset_path="$output_dir/$ASSET_NAME"
[[ ! -e "$asset_path" ]] || die "refusing to overwrite existing asset: $asset_path"

work_dir=""
asset_tmp=""

cleanup() {
    [[ -z "$work_dir" ]] || rm -rf -- "$work_dir"
    [[ -z "$asset_tmp" ]] || rm -f -- "$asset_tmp"
}
trap cleanup EXIT

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/hyprspaces-waybar-release.XXXXXX")
asset_tmp=$(mktemp "$output_dir/.${ASSET_NAME}.XXXXXX")

build_dir="$work_dir/build"
build_source_dir="$work_dir/source"
stage_dir="$work_dir/stage"
mkdir -p -- "$stage_dir"

export LC_ALL=C
export TZ=UTC
export SOURCE_DATE_EPOCH
SOURCE_DATE_EPOCH=$(git -C "$source_dir" show -s --format=%ct "$source_commit")

log "Building $source_ref ($source_commit)"
git clone --quiet --no-checkout --no-local "$source_dir" "$build_source_dir"
git -C "$build_source_dir" checkout --quiet --detach "$source_commit"

meson setup "$build_dir" "$build_source_dir" \
    --prefix=/usr \
    --libdir=lib \
    --buildtype=plain \
    --wrap-mode=nodownload \
    -Dcpp_std=c++20 \
    -Dexperimental=true \
    -Dcava=disabled \
    -Dman-pages=enabled \
    -Dsystemd=enabled \
    -Dtests=enabled

meson compile -C "$build_dir" -j "$jobs"

log "Running test suite without live compositor IPC variables"
env \
    -u HYPRLAND_INSTANCE_SIGNATURE \
    -u SWAYSOCK \
    -u WAYLAND_DISPLAY \
    -u WAYLAND_SOCKET \
    meson test -C "$build_dir" --print-errorlogs --no-rebuild --suite waybar

log "Installing full Waybar Meson tree"
meson install -C "$build_dir" \
    --destdir "$stage_dir" \
    --no-rebuild \
    --skip-subprojects \
    --strip
validate_install_tree "$stage_dir" "$work_dir/waybar.strings"

log "Creating reproducible release asset"
tar \
    --sort=name \
    --mtime="@$SOURCE_DATE_EPOCH" \
    --owner=0 \
    --group=0 \
    --numeric-owner \
    --format=gnu \
    -C "$stage_dir" \
    -cf - \
    etc usr | zstd -19 -T0 --no-progress -f -o "$asset_tmp"

validate_archive "$asset_tmp" "$work_dir/archive.list"

chmod 0644 "$asset_tmp"
ln -- "$asset_tmp" "$asset_path" || die "refusing to overwrite existing asset: $asset_path"
rm -f -- "$asset_tmp"

asset_sha256=$(sha256sum "$asset_path" | cut -d ' ' -f 1)
log "Asset: $asset_path"
log "SHA256: $asset_sha256"
