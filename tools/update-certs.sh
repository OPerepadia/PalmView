#!/usr/bin/env bash
# Rebuilds the issuers the app trusts. Each is checked against the live
# chain of the hosts it signs for before the bundles are replaced:
#
# - certs/google.pem: Google Trust Services WR2 (RSA) and WE2 (ECDSA), for
#   YouTube.
# - certs/github.pem: Sectigo DV R36 (RSA) and E36 (ECDSA), for the update
#   check on api.github.com.
#
# The issuers, not the roots, are trusted because WE2 and E36 are signed by
# P-384 roots and PalmTLS only implements P-256.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd -P)"
temp="$(mktemp -d)"
trap 'rm -rf "$temp"' EXIT

# Sectigo serves its issuers as DER over plain HTTP; check_host below is
# what vouches for them.
fetch_issuer() {
  local name="$1" expected="$2" url="$3" format="$4"
  curl --retry 4 --retry-all-errors -fsSL "$url" -o "$temp/$name.download"
  openssl x509 -inform "$format" -in "$temp/$name.download" -out "$temp/$name.pem"
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

sectigo=http://crt.sectigo.com/SectigoPublicServerAuthenticationCADV
fetch_issuer wr2 WR2 https://pki.goog/repo/certs/wr2.pem PEM
fetch_issuer we2 WE2 https://pki.goog/repo/certs/we2.pem PEM
fetch_issuer r36 "Sectigo Public Server Authentication CA DV R36" "${sectigo}R36.crt" DER
fetch_issuer e36 "Sectigo Public Server Authentication CA DV E36" "${sectigo}E36.crt" DER
check_host www.youtube.com ecdsa_secp256r1_sha256 we2
check_host www.youtube.com rsa_pss_rsae_sha256:rsa_pkcs1_sha256 wr2
check_host redirector.googlevideo.com rsa_pss_rsae_sha256:rsa_pkcs1_sha256 wr2
check_host api.github.com ecdsa_secp256r1_sha256 e36
check_host api.github.com rsa_pss_rsae_sha256:rsa_pkcs1_sha256 r36

mkdir -p "$root/certs"
cat "$temp/wr2.pem" "$temp/we2.pem" >"$root/certs/google.pem"
cat "$temp/r36.pem" "$temp/e36.pem" >"$root/certs/github.pem"
for name in wr2 we2 r36 e36; do
  openssl x509 -in "$temp/$name.pem" -noout -subject -enddate -fingerprint -sha256
done
echo "Wrote $root/certs/google.pem and $root/certs/github.pem"
