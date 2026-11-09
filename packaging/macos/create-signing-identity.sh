#!/bin/bash
# Creates a self-signed code signing certificate in a keychain, once per Mac
# that builds rdmachine. Without an Apple Developer account this is how the
# apps get a signature that stays the same from build to build: macOS ties
# the Screen Recording and Accessibility permissions to the app's signing
# certificate, so with a fixed certificate a rebuilt app keeps them, while an
# ad hoc signature (codesign -s -) changes with every build and loses them.
#
#   packaging/macos/create-signing-identity.sh [--name NAME] [--keychain PATH]
#                                              [--trust user|admin|none]
#
#   --name      certificate name (default: "rdmachine Local Signing"; give the
#               same name to package.sh --identity)
#   --keychain  keychain to import into (default: the login keychain)
#   --trust     how to trust the certificate for code signing: "user" (default,
#               asks for your password in a dialog), "admin" (system-wide, uses
#               sudo, works without a GUI, e.g. over SSH) or "none"
#
# Running it again with the same name does nothing. Keep the certificate:
# deleting it and making a new one means granting the permissions again. To
# move it to another Mac, export it with its private key from Keychain Access.
set -euo pipefail

name="rdmachine Local Signing"
keychain=""
trust="user"
while [ $# -gt 0 ]; do
  case "$1" in
    --name) name=${2:?}; shift 2 ;;
    --name=*) name=${1#*=}; shift ;;
    --keychain) keychain=${2:?}; shift 2 ;;
    --keychain=*) keychain=${1#*=}; shift ;;
    --trust) trust=${2:?}; shift 2 ;;
    --trust=*) trust=${1#*=}; shift ;;
    -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
case "$trust" in user|admin|none) ;; *) echo "--trust must be user, admin or none" >&2; exit 2 ;; esac

[ "$(uname -s)" = Darwin ] || { echo "this script runs on macOS" >&2; exit 1; }
if [ -z "$keychain" ]; then
  keychain=$(security default-keychain -d user | sed 's/^[[:space:]]*"\(.*\)"$/\1/')
fi

if security find-certificate -c "$name" "$keychain" >/dev/null 2>&1; then
  echo "\"$name\" already exists in $keychain:"
  security find-identity -p codesigning "$keychain" | grep -F "\"$name\"" || true
  exit 0
fi

# The system LibreSSL writes a PKCS#12 file that `security import` reads;
# OpenSSL 3 from Homebrew would need -legacy.
openssl=/usr/bin/openssl
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cat >"$work/cert.cnf" <<EOF
[req]
distinguished_name = dn
prompt = no
x509_extensions = ext
[dn]
CN = $name
[ext]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
subjectKeyIdentifier = hash
EOF

# Ten years: the certificate only has to outlive the builds signed with it.
"$openssl" req -new -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
  -config "$work/cert.cnf" -keyout "$work/key.pem" -out "$work/cert.pem" 2>/dev/null
pass=$("$openssl" rand -hex 16)
"$openssl" pkcs12 -export -inkey "$work/key.pem" -in "$work/cert.pem" -name "$name" \
  -passout "pass:$pass" -out "$work/identity.p12"

# -T lets codesign use the private key; macOS may still ask once for the
# keychain password ("Always Allow" makes that the last time).
security import "$work/identity.p12" -k "$keychain" -P "$pass" -f pkcs12 -T /usr/bin/codesign

case "$trust" in
  user)
    echo "Trusting the certificate for code signing (macOS asks for your password)..."
    security add-trusted-cert -r trustRoot -p codeSign -k "$keychain" "$work/cert.pem"
    ;;
  admin)
    sudo security add-trusted-cert -d -r trustRoot -p codeSign -k /Library/Keychains/System.keychain "$work/cert.pem"
    ;;
  none) ;;
esac

sha1=$("$openssl" x509 -in "$work/cert.pem" -noout -fingerprint -sha1 | sed 's/.*=//; s/://g')
echo
echo "Created \"$name\" (SHA-1 $sha1) in $keychain."
security find-identity -p codesigning "$keychain" | grep -F "$sha1" ||
  echo "note: not listed as a valid code signing identity yet (trust: $trust)"
echo "Package with: packaging/macos/package.sh --identity \"$name\""
