#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks that scripts/install-rpm.sh passes an RPM path with quotes and shell
# syntax to the install command unchanged and without running any of it.
# Usage: install-rpm-test.sh <absolute scratch work dir>

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
mkdir -p "${WORK_DIR}/bin" "${WORK_DIR}/home" "${WORK_DIR}/rpms"

# A stand-in for dnf that records its arguments, one per line.
cat >"${WORK_DIR}/bin/dnf" <<'DNF'
#!/usr/bin/env bash
printf '%s\n' "$@" >"${FAKE_DNF_LOG}"
DNF
chmod +x "${WORK_DIR}/bin/dnf"

# Run from the work dir, where the commands in the name would create "ran".
cd "${WORK_DIR}"
rpm_path="${WORK_DIR}/rpms/ferrous \"quoted\" 'single' \$(touch ran) \`touch ran\`.rpm"
touch "${rpm_path}"

# The install command runs in a login shell; a scratch HOME keeps the
# user's profile out of it.
HOME="${WORK_DIR}/home" \
    PATH="${WORK_DIR}/bin:${PATH}" \
    FAKE_DNF_LOG="${WORK_DIR}/dnf.log" \
    FERROUS_DNF_CMD="${WORK_DIR}/bin/dnf --assumeyes-ignored" \
    bash "${SCRIPT_DIR}/../install-rpm.sh" "${rpm_path}" >/dev/null

[[ ! -e "${WORK_DIR}/ran" ]] || fail "the RPM path ran as shell code"
expected="$(printf '%s\n' --assumeyes-ignored install -y "${rpm_path}")"
[[ "$(cat "${WORK_DIR}/dnf.log")" == "${expected}" ]] \
    || fail "dnf got other arguments: $(cat "${WORK_DIR}/dnf.log")"

rm -rf -- "${WORK_DIR}"
echo "install-rpm.sh passes the RPM path unchanged"
