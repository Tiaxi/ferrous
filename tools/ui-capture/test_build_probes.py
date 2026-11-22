#!/usr/bin/env python3
"""Unit tests of build_probes.sh's cache: the probes are rebuilt for other source, flags, compiler or Qt and when
the binary is no longer the one recorded, and not for the same content in another checkout.

A stand-in g++ and pkg-config count the builds, in a temporary folder.
Run with `python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import os
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build_probes.sh")
GXX = """#!/bin/sh
if [ "$1" = --version ]; then echo "g++ ${STUB_GXX:-1}"; exit 0; fi
while [ $# -gt 0 ]; do [ "$1" = -o ] && out=$2; shift; done
echo x >> "$STUB_BUILDS"
echo binary > "$out"
"""
PKG = """#!/bin/sh
if [ "$1" = --modversion ]; then echo "${STUB_QT:-6.8}"; else echo "${STUB_FLAGS:--lQt6}"; fi
"""


def write(path, text, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, mode)


class ProbeCache(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="probes-test-")
        self.addCleanup(shutil.rmtree, self.tmp)
        write(os.path.join(self.tmp, "bin", "g++"), GXX, 0o755)
        write(os.path.join(self.tmp, "bin", "pkg-config"), PKG, 0o755)
        self.log = os.path.join(self.tmp, "builds")
        write(self.log, "")
        self.out = os.path.join(self.tmp, "probe")
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("SHOT_")}
        self.env.update(PATH=os.path.join(self.tmp, "bin") + os.pathsep + os.environ.get("PATH", ""),
                        SHOT_WORK=os.path.join(self.tmp, "work"), STUB_BUILDS=self.log)
        self.a = self.checkout("a")

    def checkout(self, name, shotprobe="// probe\n"):
        d = os.path.join(self.tmp, name)
        os.makedirs(d)
        shutil.copy(SCRIPT, d)
        write(os.path.join(d, "shotprobe.cpp"), shotprobe)
        write(os.path.join(d, "iconprobe.cpp"), "// icons\n")
        return d

    def builds(self, checkout, **env):
        """The binaries built by one run from that checkout."""
        before = os.path.getsize(self.log)
        subprocess.run(["bash", os.path.join(checkout, "build_probes.sh"), self.out], env={**self.env, **env},
                       check=True, capture_output=True)
        return (os.path.getsize(self.log) - before) // 2

    def test_built_once(self):
        self.assertEqual([self.builds(self.a), self.builds(self.a)], [2, 0])

    def test_another_checkout_of_the_same_source_needs_no_build(self):
        self.builds(self.a)
        self.assertEqual(self.builds(self.checkout("b")), 0)  # (newer files, the same content)

    def test_other_source_rebuilds_that_probe(self):
        self.builds(self.a)
        self.assertEqual(self.builds(self.checkout("c", "// changed\n")), 1)
        self.assertEqual(self.builds(self.a), 1)

    def test_qt_flags_or_compiler_rebuild(self):
        self.builds(self.a)
        for change in (dict(STUB_QT="6.9"), dict(STUB_FLAGS="-lQt6Other"), dict(STUB_GXX="2")):
            self.assertEqual(self.builds(self.a, **change), 2, change)
            self.builds(self.a)

    def test_a_binary_replaced_behind_its_key_is_rebuilt(self):
        self.builds(self.a)
        write(os.path.join(self.out, "shotprobe.so"), "an older harness's build\n")
        self.assertEqual(self.builds(self.a), 1)


if __name__ == "__main__":
    unittest.main()
