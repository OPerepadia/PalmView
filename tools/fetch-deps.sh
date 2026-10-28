#!/usr/bin/env bash
# Fetches the Palm OS SDK 5r4 and PalmTLS at pinned commits, and the
# TJpgDec JPEG decoder at a pinned release, into deps/. Installs the SDK
# into the toolchain and builds PalmTLS.
# Run tools/fetch-toolchain.sh first (or build palm-toolchain yourself and
# point PALM_TOOLCHAIN_ROOT at it).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd -P)"
deps="$root/deps"
sdk_commit=1fa2206
palm_tls_commit=545bc3f
tjpgd_sha256=052fe3efbc9a8be29f31597ad009c5b51a4f6905878eb28569e0ab3d46d0c013
toolchain="${PALM_TOOLCHAIN_ROOT:-$deps/palm-toolchain}"

clone_at() {
  local url="$1" dir="$2" commit="$3"
  if [[ ! -d "$dir/.git" ]]; then
    git clone -q "$url" "$dir"
  fi
  # Fetch from the URL itself, so a clone whose origin is another fork
  # still gets the pinned commit.
  git -C "$dir" fetch -q "$url" '+refs/heads/*:refs/remotes/fetch-deps/*'
  git -C "$dir" checkout -q --detach "$commit"
}

mkdir -p "$deps"
if [[ ! -x "$toolchain/.toolchain/prefix/bin/m68k-none-elf-gcc" ]]; then
  echo "No toolchain in $toolchain; run tools/fetch-toolchain.sh first." >&2
  exit 1
fi

if [[ ! -d "$deps/palm-os-sdk/.git" ]]; then
  git clone -q --filter=blob:none --sparse \
    https://github.com/jichu4n/palm-os-sdk.git "$deps/palm-os-sdk"
  git -C "$deps/palm-os-sdk" sparse-checkout set sdk-5r4 PalmOne
fi
git -C "$deps/palm-os-sdk" checkout -q --detach "$sdk_commit"
clone_at https://github.com/OPerepadia/palm-tls.git "$deps/palm-tls" "$palm_tls_commit"
if [[ ! -f "$deps/tjpgd/src/tjpgd.c" ]]; then
  curl -fsSL --retry 3 -o "$deps/tjpgd3.zip" https://elm-chan.org/fsw/tjpgd/arc/tjpgd3.zip
  echo "$tjpgd_sha256  $deps/tjpgd3.zip" | sha256sum -c --quiet
  unzip -q -o "$deps/tjpgd3.zip" 'src/*' -d "$deps/tjpgd"
  rm "$deps/tjpgd3.zip"
fi

export PALM_TOOLCHAIN_ROOT="$toolchain"
export PALM_TOOLCHAIN_PREFIX="$toolchain/.toolchain/prefix"
make -C "$toolchain" install-sdk PALM_SDK_SOURCE="$deps/palm-os-sdk/sdk-5r4"
make -C "$deps/palm-tls" check
echo "Dependencies ready. Build with: make"
