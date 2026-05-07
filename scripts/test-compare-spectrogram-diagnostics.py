#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

"""Smoke tests for compare-spectrogram-diagnostics.py."""

from __future__ import annotations

import importlib.util
import tempfile
from pathlib import Path


SCRIPT = Path(__file__).with_name("compare-spectrogram-diagnostics.py")


def load_compare_module():
    spec = importlib.util.spec_from_file_location(
        "compare_spectrogram_diagnostics", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def test_parse_lod_diagnostics() -> None:
    module = load_compare_module()
    log_line = (
        "[ui-spectrogram] paint_spike path=gpu-full fallback=none ms=6.000 "
        "cache_ms=0.100 display_ms=0.200 retained_upload_ms=0.300 "
        "overlay_build_ms=0.000 tex_ms=0.400 lod_tex_upload_ms=1.250 "
        "overlay_tex_ms=0.000 lod_primary=3 lod_secondary=4 lod_blend=0.500 "
        "lod_available=1 lod_missing_reason=none cpu_rebuild=0 retained_reset=0 "
        "retained_reset_reason=none retained_spans=0 retained_cols=0 "
        "full_upload=0 tail_upload=0 tail_w=0 ov=0 fg=0 tg=0 ch=0 "
        "pending=0 cols=1024 bins=513 size=1200x400\n"
        "[ui-prof] precomputed_frame ms=9.00 pop_ms=0.10 dispatch_ms=8.90 "
        "raw_kb=32.0 data_kb=30.0 cols=128 bins=513 channels=2 start=0 "
        "total=4096 hop=1024 reset=0 clear=0 complete=0 token=7 gen=2 "
        "valid=1 lod_level=3 lod_span=8 lod_planes=2\n"
    )
    with tempfile.TemporaryDirectory() as temp_dir:
        path = Path(temp_dir) / "spectrogram.log"
        path.write_text(log_line)
        summary = module.parse_log(path)

    assert summary["lod_primary_levels"][3] == 1
    assert summary["lod_secondary_levels"][4] == 1
    assert summary["lod_blend"] == [0.5]
    assert summary["lod_upload_ms"].max_value == 1.25
    assert summary["lod_bridge_levels"][3] == 1
    assert summary["lod_bridge_spans"][8] == 1
    assert summary["lod_bridge_planes"][2] == 1


def main() -> int:
    test_parse_lod_diagnostics()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
