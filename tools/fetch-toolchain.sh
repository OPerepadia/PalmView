#!/usr/bin/env bash
# Pull the prebuilt palm-toolchain image and copy the toolchain out of it,
# so later builds run natively without Docker.
#
#   sg docker -c tools/fetch-toolchain.sh
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd -P)"
image="${PALM_TOOLCHAIN_IMAGE:-ghcr.io/palmsnipe/palm-toolchain:latest@sha256:155a1f8082b55507825a61ca271521e662f4a88c3d030958a33ecab9e8379532}"
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
