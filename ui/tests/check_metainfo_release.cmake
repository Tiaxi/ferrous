# SPDX-License-Identifier: GPL-3.0-or-later
# Fails unless the newest AppStream release in METAINFO_FILE is EXPECTED_VERSION,
# so software centers never show an older version than the one installed.

if(NOT METAINFO_FILE OR NOT EXPECTED_VERSION)
    message(FATAL_ERROR "Pass -DMETAINFO_FILE=<path> and -DEXPECTED_VERSION=<version>.")
endif()

file(READ "${METAINFO_FILE}" metainfo)
string(REGEX MATCH "<release[ \t\r\n]+version=\"([^\"]+)\"" release_tag "${metainfo}")
if(NOT release_tag)
    message(FATAL_ERROR "${METAINFO_FILE} has no <release version=\"...\"> entry.")
endif()

if(NOT CMAKE_MATCH_1 STREQUAL EXPECTED_VERSION)
    message(FATAL_ERROR
        "The newest release in ${METAINFO_FILE} is ${CMAKE_MATCH_1}, but the app version is "
        "${EXPECTED_VERSION}. Add a release entry with scripts/bump-version.sh.")
endif()
