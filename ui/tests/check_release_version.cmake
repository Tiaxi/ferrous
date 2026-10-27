# SPDX-License-Identifier: GPL-3.0-or-later
# Runs scripts/check-release-version.sh on fixture trees in WORK_DIR: a tag
# that matches every version passes, and each mismatch fails.

if(NOT BASH OR NOT CHECK_SCRIPT OR NOT WORK_DIR)
    message(FATAL_ERROR "Pass -DBASH=<path>, -DCHECK_SCRIPT=<path> and -DWORK_DIR=<path>.")
endif()

function(write_fixture name cargo_version spec_version metainfo_version)
    set(root "${WORK_DIR}/${name}")
    file(REMOVE_RECURSE "${root}")
    file(WRITE "${root}/Cargo.toml"
        "[package]\nname = \"ferrous\"\nversion = \"${cargo_version}\"\n\n"
        "[dependencies]\nserde = { version = \"9.9.9\" }\n")
    file(WRITE "${root}/packaging/rpm/ferrous.spec"
        "%{!?ferrous_version:%global ferrous_version ${spec_version}}\n"
        "Name:           ferrous\nVersion:        %{ferrous_version}\n")
    file(WRITE "${root}/ui/io.github.tiaxi.ferrous.metainfo.xml"
        "<component>\n  <releases>\n"
        "    <release version=\"${metainfo_version}\" date=\"2026-01-02\"/>\n"
        "    <release version=\"1.2.2\" date=\"2026-01-01\"/>\n"
        "  </releases>\n</component>\n")
endfunction()

function(expect_check name tag expected_result expected_output)
    execute_process(
        COMMAND "${BASH}" "${CHECK_SCRIPT}" --source-dir "${WORK_DIR}/${name}" "${tag}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output
    )
    if(expected_result STREQUAL "pass" AND NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: ${tag} should pass but failed (${result}):\n${output}")
    endif()
    if(expected_result STREQUAL "fail" AND result EQUAL 0)
        message(FATAL_ERROR "${name}: ${tag} should fail but passed:\n${output}")
    endif()
    if(NOT output MATCHES "${expected_output}")
        message(FATAL_ERROR "${name}: expected output matching '${expected_output}':\n${output}")
    endif()
endfunction()

write_fixture(matching 1.2.3 1.2.3 1.2.3)
expect_check(matching v1.2.3 pass "Tag v1\\.2\\.3 matches")
expect_check(matching v1.2.4 fail "Cargo.toml version '1\\.2\\.3' does not match tag v1\\.2\\.4")
expect_check(matching 1.2.3 fail "not of the form vX\\.Y\\.Z")
expect_check(matching v1.2.3-rc1 fail "not of the form vX\\.Y\\.Z")

write_fixture(stale_cargo 1.2.2 1.2.3 1.2.3)
expect_check(stale_cargo v1.2.3 fail "Cargo.toml version '1\\.2\\.2' does not match")

write_fixture(stale_spec 1.2.3 1.2.2 1.2.3)
expect_check(stale_spec v1.2.3 fail "RPM spec version '1\\.2\\.2' does not match")

write_fixture(stale_metainfo 1.2.3 1.2.3 1.2.2)
expect_check(stale_metainfo v1.2.3 fail "AppStream metadata does not match")

file(REMOVE_RECURSE "${WORK_DIR}")
