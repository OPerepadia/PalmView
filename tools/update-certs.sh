#!/usr/bin/env bash
# Rebuilds certs/google.pem, the issuers the app trusts: Google Trust
# Services WR2 (RSA) and WE2 (ECDSA). Each is checked against the live chain
# of the hosts the app talks to before the bundle is replaced.
#
# The issuers, not the roots, are trusted because WE2 is signed by a P-384
# root and PalmTLS only implements P-256.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd -P)"
temp="$(mktemp -d)"
trap 'rm -rf "$temp"' EXIT

fetch_issuer() {
  local name="$1" expected="$2"
  curl --retry 4 --retry-all-errors -fsSL "https://pki.goog/repo/certs/$name.pem" \
    -o "$temp/$name.pem"
  openssl x509 -in "$temp/$name.pem" -noout -subject | grep -F "CN=$expected" >/dev/null
}

# Leaf certificate a client limited to the given signature algorithms gets.
check_host() {
  local host="$1" sigalgs="$2" issuer="$3"
  openssl s_client -connect "$host:443" -servername "$host" -sigalgs "$sigalgs" \
      -showcerts </dev/null 2>/dev/null | awk '
        /-----BEGIN CERTIFICATE-----/ { if (++count == 1) capture = 1 }
        capture { print }
        /-----END CERTIFICATE-----/ { if (capture) exit }
      ' >"$temp/leaf.pem"
  openssl verify -partial_chain -trusted "$temp/$issuer.pem" "$temp/leaf.pem" >/dev/null
  echo "$host ($sigalgs): verified by $issuer"
}

fetch_issuer wr2 WR2
fetch_issuer we2 WE2
check_host www.youtube.com ecdsa_secp256r1_sha256 we2
check_host www.youtube.com rsa_pss_rsae_sha256:rsa_pkcs1_sha256 wr2
check_host redirector.googlevideo.com rsa_pss_rsae_sha256:rsa_pkcs1_sha256 wr2

mkdir -p "$root/certs"
cat "$temp/wr2.pem" "$temp/we2.pem" >"$root/certs/google.pem"
for name in wr2 we2; do
  openssl x509 -in "$temp/$name.pem" -noout -subject -enddate -fingerprint -sha256
done
echo "Wrote $root/certs/google.pem"
