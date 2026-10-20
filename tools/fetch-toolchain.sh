#!/usr/bin/env bash
# Pull the prebuilt palm-toolchain image and copy the toolchain out of it,
# so later builds run natively without Docker.
#
#   sg docker -c tools/fetch-toolchain.sh
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd -P)"
image="${PALM_TOOLCHAIN_IMAGE:-ghcr.io/palmsnipe/palm-toolchain:latest}"
dest="$root/deps/palm-toolchain"

if [[ -e "$dest" ]]; then
  echo "$dest already exists; remove it to fetch again." >&2
  exit 1
fi

mkdir -p "$root/deps"
docker pull "$image"
docker image inspect --format '{{index .RepoDigests 0}}' "$image" \
  > "$root/deps/palm-toolchain.image"

cid="$(docker create "$image")"
trap 'docker rm -f "$cid" >/dev/null' EXIT
docker cp "$cid:/opt/palm-toolchain" "$dest"

echo "Toolchain copied to $dest from $(cat "$root/deps/palm-toolchain.image")"
