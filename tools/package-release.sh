#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  tools/package-release.sh <version-tag> <compiled-bin> [output-dir]

Example:
  tools/package-release.sh v0.07 \
    build/HU058D_Custom.ino.bin \
    dist/v0.07

Creates:
  HU058D_Custom-<version>.bin
  HU058D_Custom-<version>.bin.sha256
  manifest.txt

The script records the Git tag commit and warns if the current checkout does
not match the tag. It cannot prove that a supplied binary was compiled from
that commit, so build/export the binary from the tagged source.
EOF
}

[[ $# -ge 2 && $# -le 3 ]] || { usage >&2; exit 2; }

version="$1"
input_bin="$2"
output_dir="${3:-dist/$version}"

[[ -f "$input_bin" ]] || {
  echo "Binary not found: $input_bin" >&2
  exit 1
}

git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
  echo "Run this script from inside the HU-058D Git repository." >&2
  exit 1
}

tag_commit="$(git rev-list -n 1 "$version" 2>/dev/null || true)"
[[ -n "$tag_commit" ]] || {
  echo "Git tag not found: $version" >&2
  exit 1
}

head_commit="$(git rev-parse HEAD)"
if [[ "$head_commit" != "$tag_commit" ]]; then
  cat >&2 <<EOF
WARNING: current HEAD does not match $version.
  HEAD: $head_commit
  tag:  $tag_commit

Make sure the supplied binary was exported from the tagged $version source.
EOF
fi

mkdir -p "$output_dir"

safe_version="${version#v}"
output_bin="$output_dir/HU058D_Custom-v${safe_version}.bin"
output_hash="$output_bin.sha256"
manifest="$output_dir/manifest.txt"

cp "$input_bin" "$output_bin"

(
  cd "$output_dir"
  sha256sum "$(basename "$output_bin")" > "$(basename "$output_hash")"
)

bin_size="$(stat -c '%s' "$output_bin")"
sha256="$(awk '{print $1}' "$output_hash")"

cat > "$manifest" <<EOF
HU-058D firmware release artifact
version=$version
git_tag=$version
git_commit=$tag_commit
binary=$(basename "$output_bin")
binary_size_bytes=$bin_size
sha256=$sha256
created_utc=$(date -u +'%Y-%m-%dT%H:%M:%SZ')

Arduino target settings:
  Board:            Generic ESP8266 Module
  Flash Size:       1MB (FS:none OTA:~502KB)
  CPU Frequency:    80 MHz
  Flash Mode:       QIO
  Flash Frequency:  80 MHz
  Upload Speed:     115200
EOF

echo "Release artifacts created:"
echo "  $output_bin"
echo "  $output_hash"
echo "  $manifest"
echo
echo "Verify with:"
echo "  (cd '$output_dir' && sha256sum -c '$(basename "$output_hash")')"
