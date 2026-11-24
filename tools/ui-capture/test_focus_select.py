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

def m_walk(r, arg):
    open(f"a11y_{r.s.suffix}.jsonl", "a")

def m_idle(r, arg):
    pass
'''


def table(*lines, **kw):
    names = ("deep", "fixed", "unknown", "shot", "direct", "floor", "events", "measure", "walk", "idle")
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
        scopes = [dict(dict(images=[], sheets=[], facts=[], floors=[]), **x)
                  for x in (scope if isinstance(scope, list) else [scope])]
        jobs = [("s", t) for t in themes]
        return focus_select.prefixes(scopes, jobs, states_mod, suffix, source)

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

    def test_the_shot_a_frames_crop_takes_an_item_from(self):
        t = table(("motion", "rec", "strip 1 0.1 - :: wait 1"), ("dialog", "a", "wait 1"), ("dialog", "b", "wait 1"))
        cell = dict(strip="rec", include=("dialog", "a", "button"))
        self.assertEqual(self.cut(t, dict(sheets=[("sheet-a", "A sheet", "dark", [[cell]])])), 2)

    def test_composites_and_direct_index_entries(self):
        t = table(("_", "_s", "macro shot one"), ("_", "_d", "macro direct"), ("_", "_i", "macro idle"))
        self.assertEqual(self.cut(t, dict(sheets=tile_sheet(("details", "made-one", None)))), 1)
        self.assertEqual(self.cut(t, dict(sheets=tile_sheet(("closeup", "controls", None)))), 2)

    def test_scopes_together_need_the_furthest_line_any_of_them_needs(self):
        t = table(("main", "a", "x"), ("main", "b", "x"), ("main", "c", "x"), ("main", "d", "x"))
        a, c = dict(sheets=tile_sheet(("main", "a", None))), dict(sheets=tile_sheet(("main", "c", None)))
        self.assertEqual((self.cut(t, a), self.cut(t, c), self.cut(t, [a, c]), self.cut(t, [c, a, a])), (1, 3, 3, 3))

    def test_one_scope_that_reads_every_floor_keeps_them_for_all(self):
        t = table(("_", "_a", "macro floor left:left"), ("_", "_b", "macro floor right:right"))
        self.assertEqual(self.cut(t, [dict(floors=["left"]), dict(floors=None)]), 2)

    def test_system_facts_read_every_shot_and_the_walk(self):
        sheets = tile_sheet(("main", "a", None))
        t = table(("main", "a", "x"), ("main", "b", "x"), ("_", "_w", "macro walk"), ("_", "_i", "macro idle"))
        self.assertEqual(self.cut(t, dict(sheets=sheets)), 1)
        self.assertEqual(self.cut(t, dict(sheets=sheets, system_facts=True)), 3)
        t = table(("main", "a", "x"), ("main", "b", "x"), ("_", "_i", "macro idle"))
        # (b is in no tile, but its layout's values are in "worst in the round")
        self.assertEqual(self.cut(t, dict(sheets=sheets, system_facts=True)), 2)

    def test_floors_named_or_all(self):
        t = table(("_", "_a", "macro floor left:left"), ("_", "_b", "macro floor right:right"), ("_", "_c", "macro floor restore"))
        self.assertEqual(self.cut(t, dict(floors=["left"])), 1)
        self.assertEqual(self.cut(t, dict(floors=None)), 2)

    def test_accessibility_events_when_the_scope_lists_any(self):
        t = table(("main", "a", "wait 1"), ("_", "_e", "macro events press"))
        sheets = tile_sheet(("main", "a", None))
        self.assertEqual(self.cut(t, dict(sheets=sheets)), 1)
        self.assertEqual(self.cut(t, dict(sheets=sheets, a11y_labels=["press"])), 2)

    def test_every_measure_on_a_line_that_grabs(self):
        t = table(("contrast", "x", "macro measure row text it"), ("contrast", "y", "macro measure bar bar it"),
                  ("_", "_z", "macro measure row text it"))
        # (the scope quotes only the row, yet a failed bar measurement must still fail the run)
        self.assertEqual(self.cut(t, dict(contrast={"sheet-a": ["row"]})), 2)

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

    def test_packs_that_share_a_file_are_not_written_at_the_same_time(self):
        t = table(STRIP_PAIRS=[("both", "one", "two", "Both", ("dark",))])
        sheet = dict(sheets=tile_sheet(("main", "a", None)))
        pair = dict(images=[("strip", "pair:both", "dark", "", "")])
        one, three = (dict(images=[("strip", nm, "dark", "", "")]) for nm in ("one", "three"))
        self.assertFalse(focus_select.packs_apart([sheet, sheet], t))
        self.assertFalse(focus_select.packs_apart([pair, one], t))  # (a pair's half's neither-frame sheet)
        self.assertTrue(focus_select.packs_apart([sheet, pair, three], t))

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
    def selected(self, names):
        jobs = [(s, t) for s in focus.sessions_of(names) for t in (states.SESSIONS[s].get("themes") or ("dark", "light"))]
        out, problems = focus_select.prefixes([focus.SCOPES[nm] for nm in names], jobs, states, suffix)
        self.assertEqual(problems, [], names)
        return jobs, out

    def check_kept(self, names):
        """Nothing the scopes show is after a cut, and with system_facts no shot at all."""
        jobs, out = self.selected(names)
        need = focus_select.Needs([focus.SCOPES[nm] for nm in names], states)
        for session, theme in jobs:
            kw, n = states.SESSIONS[session], out.get((session, theme), 0)
            sfx = suffix(theme, kw)
            w, h = kw.get("size", (1600, 1000))
            for _s, area, state, steps, _d in [l for l in states.STATES if l[0] == session][n:]:
                e = dict(area=area, state=state, theme=theme, scale=kw.get("scale", 1), size=f"{w}x{h}",
                         file=f"{area}_{state}_{sfx}.png")
                where = f"{','.join(names)}: {area}/{state} is after {session}/{theme}'s cut"
                if not state.startswith("_") and area != "motion":
                    self.assertFalse(need.system, where)
                    self.assertFalse(any(focus_select.tile_matches(e, *t) for t in need.tiles), where)
                if focus_select.recorded(area, state, theme, states) and "strip " in steps:
                    self.assertNotIn((state, theme), need.recordings, where)

    def test_every_scope_alone_keeps_what_it_shows(self):
        for name in focus.SCOPES:
            self.check_kept([name])

    def test_the_census_keeps_what_its_seven_packs_show(self):
        self.assertEqual(len(focus.resolve("census")), 7)
        self.check_kept(focus.resolve("census"))

    def test_the_census_keeps_the_focus_walk(self):
        _, out = self.selected(focus.resolve("census"))
        lines = [l for l in states.STATES if l[0] == "focus"]
        walk = [k for k, line in enumerate(lines, 1) if "macro tab " in line[3] or "macro launch_focus" in line[3]]
        self.assertTrue(walk)
        self.assertGreaterEqual(out[("focus", "dark")], walk[-1])

    def test_the_union_is_no_shorter_than_any_of_its_scopes(self):
        _, union = self.selected(focus.resolve("census"))
        for name in focus.resolve("census"):
            for job, n in self.selected([name])[1].items():
                self.assertGreaterEqual(union.get(job, 0), n, (name, job))

    def test_the_census_packs_can_be_written_at_the_same_time(self):
        self.assertTrue(focus_select.packs_apart([focus.SCOPES[nm] for nm in focus.resolve("census")], states))

    def test_a_list_shoots_each_shared_state_once(self):
        jobs, union = self.selected(["dialogs", "viewers"])
        self.assertEqual(len(jobs), len(set(jobs)))
        apart = sum(sum(self.selected([name])[1].values()) for name in ("dialogs", "viewers"))
        self.assertLess(sum(union.values()), apart)

    def test_a_scope_without_system_facts_is_cut(self):
        for name, scope in focus.SCOPES.items():
            if scope.get("system_facts"):
                continue
            jobs, out = self.selected([name])
            whole = sum(sum(1 for l in states.STATES if l[0] == s) for s, _ in jobs)
            self.assertLess(sum(out.values()), whole, name)


def tearDownModule():
    shutil.rmtree(_TMP)


if __name__ == "__main__":
    unittest.main()
