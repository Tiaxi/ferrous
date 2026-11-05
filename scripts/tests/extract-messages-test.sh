#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks that scripts/extract-messages.sh keeps xgettext from running git, which
# gettext 1.0 does with a warning for every file when the sources are in a git
# checkout, as they are, untracked, in an RPM build by scripts/build-rpm.sh.
# Runs the script with stand-ins for a gettext that has --no-git and one that
# does not, then, when git and xgettext are installed, with the real xgettext
# on sources in an untracked directory of a scratch checkout.
# Usage: extract-messages-test.sh <absolute scratch work dir>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
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
mkdir -p "${WORK_DIR}"

# Makes a copy of the extraction scripts in $1, with one C++ and one QML
# source to extract.
make_sources() {
    local root="$1"
    mkdir -p "${root}/scripts" "${root}/ui/src" "${root}/ui/qml"
    cp "${REPO_ROOT}/scripts/extract-messages.sh" "${root}/scripts/"
    cp "${REPO_ROOT}/ui/Messages.sh" "${root}/ui/"
    cat >"${root}/ui/src/Example.cpp" <<'CPP'
#include <KLocalizedString>
QString title() { return i18nc("@title:window", "Example Window"); }
CPP
    cat >"${root}/ui/qml/Example.qml" <<'QML'
import QtQuick
Text { text: I18n.i18nc("@label", "Example Label") }
QML
}

# A stand-in xgettext. With FAKE_HAS_NO_GIT=1 it is a gettext that has
# --no-git and, without it, warns as gettext 1.0 does in a git checkout;
# otherwise it is an older gettext that rejects the option. It records each
# call's arguments on one line and writes the -o file.
mkdir -p "${WORK_DIR}/bin"
cat >"${WORK_DIR}/bin/xgettext" <<'XGETTEXT'
#!/usr/bin/env bash
if [[ "${1:-}" == "--help" ]]; then
    echo "Usage: xgettext [OPTION] [INPUTFILE]..."
    if [[ "${FAKE_HAS_NO_GIT}" == 1 ]]; then
        echo "      --no-git                don't use the git program to produce a"
    fi
    exit 0
fi
printf '%s\n' "$*" >>"${FAKE_XGETTEXT_LOG}"
no_git=0
out=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-git) no_git=1 ;;
        -o) shift; out="$1" ;;
    esac
    shift
done
if [[ ${no_git} -eq 1 && "${FAKE_HAS_NO_GIT}" != 1 ]]; then
    echo "xgettext: unrecognized option '--no-git'" >&2
    exit 1
fi
if [[ ${no_git} -eq 0 && "${FAKE_HAS_NO_GIT}" == 1 ]]; then
    echo "vc-mtime: failed to read git log output" >&2
fi
echo 'msgid ""' >"${out}"
XGETTEXT
chmod +x "${WORK_DIR}/bin/xgettext"

# Runs the copy of the script in $1 with the stand-in, writing into $1/pot.
run_with_stand_in() {
    local root="$1" has_no_git="$2"
    : >"${root}/xgettext.log"
    PATH="${WORK_DIR}/bin:${PATH}" \
        FAKE_HAS_NO_GIT="${has_no_git}" \
        FAKE_XGETTEXT_LOG="${root}/xgettext.log" \
        bash "${root}/scripts/extract-messages.sh" "${root}/pot" >/dev/null 2>"${root}/err"
}

# A gettext with --no-git gets it on both calls and warns about nothing.
root="${WORK_DIR}/with-no-git"
make_sources "${root}"
run_with_stand_in "${root}" 1 || fail "extraction failed: $(cat "${root}/err")"
[[ ! -s "${root}/err" ]] || fail "extraction warned: $(cat "${root}/err")"
[[ "$(wc -l <"${root}/xgettext.log")" -eq 2 ]] \
    || fail "xgettext did not run twice: $(cat "${root}/xgettext.log")"
if grep -vqE '(^| )--no-git( |$)' "${root}/xgettext.log"; then
    fail "xgettext ran without --no-git: $(cat "${root}/xgettext.log")"
fi

# An older gettext does not get the option it does not know.
root="${WORK_DIR}/without-no-git"
make_sources "${root}"
run_with_stand_in "${root}" 0 || fail "extraction with an older gettext failed: $(cat "${root}/err")"
[[ ! -s "${root}/err" ]] || fail "extraction with an older gettext warned: $(cat "${root}/err")"
if grep -q -- '--no-git' "${root}/xgettext.log"; then
    fail "an older xgettext got --no-git: $(cat "${root}/xgettext.log")"
fi

# The real xgettext, on sources in an untracked directory of a checkout, as
# in an RPM build from scripts/build-rpm.sh.
if ! command -v git >/dev/null 2>&1 || ! command -v xgettext >/dev/null 2>&1; then
    echo "git or xgettext is not installed; skipped the extraction in a checkout."
    echo "extract-messages tests passed"
    exit 0
fi
checkout="${WORK_DIR}/checkout"
mkdir -p "${checkout}"
# Keep the user's and the system's git configuration out of the checkout.
git_env=(env HOME="${WORK_DIR}" GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null)
"${git_env[@]}" git -C "${checkout}" init --quiet
echo "dist/" >"${checkout}/.gitignore"
"${git_env[@]}" git -C "${checkout}" add .gitignore
"${git_env[@]}" git -C "${checkout}" -c user.name=Test -c user.email=test@example.invalid \
    commit --quiet -m "test checkout"
root="${checkout}/dist/rpm/BUILD/ferrous-1.0.0"
make_sources "${root}"
"${git_env[@]}" bash "${root}/scripts/extract-messages.sh" "${root}/pot" >/dev/null 2>"${root}/err" \
    || fail "extraction in a checkout failed: $(cat "${root}/err")"
[[ ! -s "${root}/err" ]] || fail "extraction in a checkout warned: $(cat "${root}/err")"
grep -qF 'msgid "Example Window"' "${root}/pot/ferrous.pot" \
    || fail "the template lacks the C++ string"
grep -qF 'msgid "Example Label"' "${root}/pot/ferrous.pot" \
    || fail "the template lacks the QML string"

echo "extract-messages tests passed"
