#!/usr/bin/env python3
"""Unit tests of focus_select.py: what a focus capture shoots of each session, on a small state table of its own,
and on the real one that nothing a scope's pack reads lies after a cut.

Needs no KWin, app or shots. Run with `python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import os
import shutil
import sys
import tempfile
import types
import unittest

_TMP = tempfile.mkdtemp(prefix="ui-capture-test-")
# (before the harness's modules load: shotlib reads the round's folder when imported)
os.environ.setdefault("SHOT_WORK", os.path.join(_TMP, "work"))
os.environ.setdefault("SHOT_OUT", os.path.join(_TMP, "round", "shots"))
os.environ.setdefault("SHOT_APP", os.path.join(_TMP, "no-app"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import contrast  # noqa: E402
import focus  # noqa: E402
import focus_select  # noqa: E402
import shotlib  # noqa: E402
import states  # noqa: E402

SOURCE = '''
def _fact(r, key, text, **data):
    with open("facts.jsonl", "a") as f:
        f.write(key)

def _save_shot(r, img, area, state, desc):
    r.s.index.append(dict(file=area, area=area, state=state))

def helper(r, arg):
    _fact(r, f"deep-{arg}", "reached through a helper")

def m_deep(r, arg):
    helper(r, arg)

def m_fixed(r, arg):
    _fact(r, "fixed", "a literal key")

def m_unknown(r, arg):
    key = arg
    _fact(r, key, "a key known only as it runs")

def m_shot(r, arg):
    _save_shot(r, None, "details", f"made-{arg}", "a composite")

def m_direct(r, arg):
    r.s.index.append(dict(file="x", area="closeup", state="controls"))

def m_floor(r, arg):
    open("floors.jsonl", "a")

def m_events(r, arg):
    open("a11y-events")

def m_measure(r, arg):
    r.pending_measures = [arg]

def m_idle(r, arg):
    pass
'''


def table(*lines, **kw):
    names = ("deep", "fixed", "unknown", "shot", "direct", "floor", "events", "measure", "idle")
    return types.SimpleNamespace(
        STATES=[("s",) + line + ("",) for line in lines],
        SESSIONS=dict(s=kw.pop("session", {})),
        MACROS={n: types.SimpleNamespace(__name__="m_" + n) for n in names},
        STRIP_BOTH_THEMES=kw.pop("both", ()),
        **kw)


def suffix(theme, kw):
    return shotlib.session_suffix(theme, kw.get("scale", 1), kw.get("size", (1600, 1000)), kw.get("suffix_extra", ""))


def tile_sheet(*tiles):
    return [("sheet-a", "A sheet", "dark", [[[(*t, "label", {}) if len(t) == 3 else t for t in tiles]]])]


class Selection(unittest.TestCase):
    def need(self, states_mod, scope, themes=("dark",), source=SOURCE):
        scope = dict(dict(images=[], sheets=[], facts=[], floors=[]), **scope)
        jobs = [("s", t) for t in themes]
        return focus_select.prefixes(scope, jobs, states_mod, {"row-text": "Queue title"}, suffix, source)

    def cut(self, states_mod, scope, **kw):
        out, problems = self.need(states_mod, scope, **kw)
        self.assertEqual(problems, [])
        return out[("s", "dark")]

    def test_up_to_the_last_line_that_makes_a_tile(self):
        t = table(("main", "a", "wait 1"), ("main", "b", "wait 1"), ("main", "c", "wait 1"))
        self.assertEqual(self.cut(t, dict(sheets=tile_sheet(("main", "b", None)))), 2)

    def test_every_maker_of_a_tile_counts(self):
        t = table(("main", "a", "wait 1"), ("main", "b", "wait 1"), ("main", "a", "wait 2"), ("main", "c", "wait 1"))
        self.assertEqual(self.cut(t, dict(sheets=tile_sheet(("main", "a", None)))), 3)

    def test_a_tile_of_another_variant_is_not_this_sessions(self):
        big = ("main", "a", None, "label", dict(variant="font24"))
        t = table(("main", "a", "wait 1"))
        self.assertTrue(self.need(t, dict(sheets=tile_sheet(big)))[1])  # (nothing at all: whole sessions)
        t = table(("main", "a", "wait 1"), session=dict(suffix_extra="_font24"))
        self.assertEqual(self.cut(t, dict(sheets=tile_sheet(big))), 1)

    def test_a_setup_line_of_a_tiles_name_is_not_its_shot(self):
        t = table(("main", "a", "wait 1"), ("main", "_a", "wait 1"))
        self.assertEqual(self.cut(t, dict(sheets=tile_sheet(("main", "a", None)))), 1)

    def test_facts_by_prefix_through_helpers(self):
        t = table(("_", "_d", "macro deep x"), ("_", "_f", "macro fixed"), ("_", "_i", "macro idle"))
        self.assertEqual(self.cut(t, dict(facts=["deep-x"])), 1)
        self.assertEqual(self.cut(t, dict(facts=["fix"])), 2)

    def test_a_key_known_only_at_run_time_counts_as_any(self):
        t = table(("_", "_f", "macro fixed"), ("_", "_u", "macro unknown card-x"), ("_", "_i", "macro idle"))
        self.assertEqual(self.cut(t, dict(facts=["fixed"])), 2)

    def test_the_facts_the_pack_reads_besides_the_scopes(self):
        src = SOURCE.replace('"fixed", "a literal', '"dlg-about", "a literal')
        t = table(("main", "a", "wait 1"), ("_", "_f", "macro fixed"))
        sheets = tile_sheet(("main", "a", None))
        self.assertEqual(self.cut(t, dict(sheets=sheets), source=src), 1)
        self.assertEqual(self.cut(t, dict(sheets=sheets, fact_table=True), source=src), 2)
        self.assertEqual(self.cut(t, dict(lead_table=dict(prefix="dlg-")), source=src), 2)
        card = SOURCE.replace('"fixed", "a literal', '"card-wide", "a literal')
        self.assertEqual(self.cut(t, dict(sheets=sheets), source=card), 2)  # (every scope's note reads cards)
        plate = tile_sheet(("main", "a", None, "label", dict(plate=True)))
        src = SOURCE.replace('"fixed", "a literal', '"plate-a-dark", "a literal')
        self.assertEqual(self.cut(t, dict(sheets=plate), source=src), 2)

    def test_composites_and_direct_index_entries(self):
        t = table(("_", "_s", "macro shot one"), ("_", "_d", "macro direct"), ("_", "_i", "macro idle"))
        self.assertEqual(self.cut(t, dict(sheets=tile_sheet(("details", "made-one", None)))), 1)
        self.assertEqual(self.cut(t, dict(sheets=tile_sheet(("closeup", "controls", None)))), 2)

    def test_floors_named_or_all(self):
        t = table(("_", "_a", "macro floor left:left"), ("_", "_b", "macro floor right:right"), ("_", "_c", "macro floor restore"))
        self.assertEqual(self.cut(t, dict(floors=["left"])), 1)
        self.assertEqual(self.cut(t, dict(floors=None)), 2)

    def test_accessibility_events_when_the_scope_lists_any(self):
        t = table(("main", "a", "wait 1"), ("_", "_e", "macro events press"))
        sheets = tile_sheet(("main", "a", None))
        self.assertEqual(self.cut(t, dict(sheets=sheets)), 1)
        self.assertEqual(self.cut(t, dict(sheets=sheets, a11y_labels=["press"])), 2)

    def test_contrast_rows_by_their_shown_names_on_lines_that_grab(self):
        t = table(("contrast", "x", "macro measure row-text text it"), ("_", "_y", "macro measure row-text text it"))
        self.assertEqual(self.cut(t, dict(contrast={"sheet-a": ["queue title"]})), 1)

    def test_macros_in_a_strips_action(self):
        t = table(("motion", "move", "strip 1 0.1 - :: click x ; macro fixed"), ("_", "_i", "macro idle"))
        self.assertEqual(self.cut(t, dict(facts=["fixed"])), 1)

    def test_recordings_and_the_themes_that_have_them(self):
        lines = (("motion", "move", "strip 1 0.1 - :: click x"), ("_", "_i", "macro idle"))
        scope = dict(images=[("strip", "move", "dark", "", "")])
        out, problems = self.need(table(*lines), scope, themes=("dark", "light"))
        self.assertEqual((out, problems), ({("s", "dark"): 1}, []))  # (light only plays the actions)
        out, problems = self.need(table(*lines, both=("move",)), dict(images=[("strip", "move", "light", "", "")]),
                                  themes=("dark", "light"))
        self.assertEqual((out, problems), ({("s", "light"): 1}, []))

    def test_pairs_perceived_sheets_and_views_lead_to_their_recordings(self):
        lines = (("motion", "one", "strip 1 0.1 - :: x"), ("motion", "two", "strip 1 0.1 - :: x"), ("main", "c", "x"))
        kw = dict(STRIP_PAIRS=[("both", "one", "two", "Both", ("dark",))], EXTRA_VIEWS={"two": [("window", None)]},
                  PERCEIVED_SHEETS=[("seen", "Seen", [("two", "dark", "Two")])])
        for image in ("pair:both", "seen", "two-window"):
            self.assertEqual(self.cut(table(*lines, **kw), dict(images=[("strip", image, "dark", "", "")])), 2)

    def test_what_cannot_be_told_shoots_whole_sessions(self):
        t = table(("main", "a", "wait 1"))
        sheets = tile_sheet(("main", "a", None))
        for scope in (dict(sheets=sheets, images=[("sheet", "main", "dark", "", "")]),  # a sheet of the whole pack
                      dict(sheets=sheets, images=[("strip", "no-such", "dark", "", "")])):
            out, problems = self.need(t, scope)
            self.assertEqual(out, {})
            self.assertTrue(problems)
        unreadable = {
            "a helper passed around": "def m_x(r, arg):\n    f = _fact\n",
            "an index written its own way": "def m_x(r, arg):\n    r.s.index.extend([])\n",
            "an index entry without literal fields": "def m_x(r, arg):\n    r.s.index.append(arg)\n",
            "facts written itself": "def m_x(r, arg):\n    open('facts.jsonl', 'a')\n",
            "measures added itself": "def m_x(r, arg):\n    r.pending_measures = []\n",
            "steps run": "def m_x(r, arg):\n    r.run_steps(arg)\n",
            "a recording made itself": "def m_x(r, arg):\n    r.s.strip('n', None)\n",
        }
        for why, extra in unreadable.items():
            out, problems = self.need(t, dict(sheets=sheets), source=SOURCE + extra)
            self.assertEqual(out, {}, why)
            self.assertTrue(problems, why)


class RealTable(unittest.TestCase):
    def test_every_scope_can_be_cut_and_keeps_what_it_shows(self):
        for name, scope in focus.SCOPES.items():
            jobs = [(s, t) for s in scope["sessions"] for t in (states.SESSIONS[s].get("themes") or ("dark", "light"))]
            out, problems = focus_select.prefixes(scope, jobs, states, contrast.WHAT, suffix)
            self.assertEqual(problems, [], name)
            need = focus_select.Needs(scope, states)
            for session, theme in jobs:
                kw, n = states.SESSIONS[session], out.get((session, theme), 0)
                sfx = suffix(theme, kw)
                w, h = kw.get("size", (1600, 1000))
                after = [l for l in states.STATES if l[0] == session][n:]
                for _s, area, state, steps, _d in after:
                    e = dict(area=area, state=state, theme=theme, scale=kw.get("scale", 1), size=f"{w}x{h}",
                             file=f"{area}_{state}_{sfx}.png")
                    if not state.startswith("_") and area != "motion":
                        self.assertFalse(any(focus_select.tile_matches(e, *t) for t in need.tiles),
                                         f"{name}: the tile {area}/{state} is after {session}/{theme}'s cut")
                    if focus_select.recorded(area, state, theme, states) and "strip " in steps:
                        self.assertNotIn((state, theme), need.recordings, f"{name}: {session}/{theme}")

    def test_each_scope_saves_at_least_one_line(self):
        for name, scope in focus.SCOPES.items():
            jobs = [(s, t) for s in scope["sessions"] for t in (states.SESSIONS[s].get("themes") or ("dark", "light"))]
            out, _ = focus_select.prefixes(scope, jobs, states, contrast.WHAT, suffix)
            whole = sum(sum(1 for l in states.STATES if l[0] == s) for s, _ in jobs)
            self.assertLess(sum(out.values()), whole, name)


def tearDownModule():
    shutil.rmtree(_TMP)


if __name__ == "__main__":
    unittest.main()
