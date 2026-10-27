#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs scripts/bump-version.sh on a copy of the files it edits and checks that
# it changes only the [package] version in Cargo.toml.
# Usage: bump-version-test.sh <absolute empty-or-scratch work dir>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="${1:?usage: $(basename "$0") <work dir>}"
if [[ "${WORK_DIR}" != /* ]]; then
    echo "The work dir must be an absolute path." >&2
    exit 1
fi

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

rm -rf -- "${WORK_DIR}"
repo="${WORK_DIR}/repo"
mkdir -p "${repo}/scripts" "${repo}/packaging/rpm" "${repo}/ui" "${WORK_DIR}/bin"
cp "${SCRIPT_DIR}/../bump-version.sh" "${SCRIPT_DIR}/../cargo-package-version.sh" "${repo}/scripts/"

# The dependency tables share the package's old version, so a script that
# rewrites every version line changes them too.
cat >"${repo}/Cargo.toml" <<'TOML'
[package]
name = "ferrous"
keywords = ["music", "player"]
version = "0.2.2" # the app version
edition = "2021"

[dependencies]
anyhow = "1"

[dependencies.extra]
version = "0.2.2"
features = ["x"]

[target.'cfg(unix)'.dev-dependencies.helper]
version = "0.2.2"

[package.metadata.notes]
version = "0.2.2"
TOML
expected_toml="$(sed 's/^version = "0.2.2" # the app version$/version = "1.4.0" # the app version/' \
    "${repo}/Cargo.toml")"

cat >"${repo}/packaging/rpm/ferrous.spec" <<'SPEC'
%{!?ferrous_version:%global ferrous_version 0.2.2}
Name: ferrous
SPEC
cat >"${repo}/ui/io.github.tiaxi.ferrous.metainfo.xml" <<'XML'
<component>
  <releases>
    <release version="0.2.2" date="2026-01-01"/>
  </releases>
</component>
XML

# A stand-in for Cargo that records how the script calls it.
cat >"${WORK_DIR}/bin/cargo" <<'CARGO'
#!/usr/bin/env bash
printf '%s\n' "$PWD $*" >>"${FAKE_CARGO_LOG}"
CARGO
chmod +x "${WORK_DIR}/bin/cargo"
export FAKE_CARGO_LOG="${WORK_DIR}/cargo.log"
export PATH="${WORK_DIR}/bin:${PATH}"

bash "${repo}/scripts/bump-version.sh" 1.4.0 >/dev/null

[[ "$(cat "${repo}/Cargo.toml")" == "${expected_toml}" ]] \
    || fail "Cargo.toml changed beyond the package version: $(diff <(echo "${expected_toml}") "${repo}/Cargo.toml")"
[[ "$(cat "${FAKE_CARGO_LOG}")" == "${repo} update -p ferrous --quiet" ]] \
    || fail "unexpected Cargo call: $(cat "${FAKE_CARGO_LOG}")"
grep -qxF '%{!?ferrous_version:%global ferrous_version 1.4.0}' "${repo}/packaging/rpm/ferrous.spec" \
    || fail "the RPM spec fallback was not bumped"
[[ "$(grep -c '<release version=' "${repo}/ui/io.github.tiaxi.ferrous.metainfo.xml")" -eq 2 ]] \
    || fail "the AppStream metadata does not have one new release"
sed -n '3p' "${repo}/ui/io.github.tiaxi.ferrous.metainfo.xml" | grep -q '^    <release version="1.4.0" date="' \
    || fail "the new release is not the newest"

# Bumping again to the same version adds no second release.
bash "${repo}/scripts/bump-version.sh" 1.4.0 >/dev/null
[[ "$(grep -c '<release version="1.4.0"' "${repo}/ui/io.github.tiaxi.ferrous.metainfo.xml")" -eq 1 ]] \
    || fail "a repeated bump added a second release"

# Single quotes, a comment with a quoted version, a header with spaces in its
# brackets and CRLF line endings: only the value changes, and nothing else.
printf '%s\r\n' '[dependencies.extra]' 'version = "1.4.0"' '' '[ package ] # the app' \
    "name = 'ferrous'" "version = '1.4.0' # next \"2.0.0\"" 'edition = "2021"' >"${repo}/Cargo.toml"
expected_toml="$(sed "s/^version = '1.4.0' # next/version = '1.5.0' # next/" "${repo}/Cargo.toml")"
bash "${repo}/scripts/bump-version.sh" 1.5.0 >/dev/null
[[ "$(cat "${repo}/Cargo.toml")" == "${expected_toml}" ]] \
    || fail "the TOML variants changed beyond the package version: $(diff <(echo "${expected_toml}") "${repo}/Cargo.toml" | od -c | head -20)"
[[ "$(bash "${repo}/scripts/cargo-package-version.sh" "${repo}/Cargo.toml")" == 1.5.0 ]] \
    || fail "the bumped version does not read back"

# Without a [package] version, the script fails and leaves Cargo.toml alone.
printf '[workspace]\nmembers = []\n\n[dependencies.extra]\nversion = "0.2.2"\n' >"${repo}/Cargo.toml"
before="$(cat "${repo}/Cargo.toml")"
if bash "${repo}/scripts/bump-version.sh" 1.6.0 >/dev/null 2>&1; then
    fail "the script succeeded without a [package] version"
fi
[[ "$(cat "${repo}/Cargo.toml")" == "${before}" ]] || fail "a failed bump changed Cargo.toml"
leftovers="$(find "${repo}" -maxdepth 1 -name 'Cargo.toml.*')"
[[ -z "${leftovers}" ]] || fail "a failed bump left ${leftovers}"

rm -rf -- "${WORK_DIR}"
echo "bump-version.sh changes only the package version"
