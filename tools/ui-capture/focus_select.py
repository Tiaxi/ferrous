#!/usr/bin/env python3
"""What a focus-only capture must shoot: for each session and theme, how many of its lines of the state table.

A session's lines share one app, so each depends on those before it. A focus capture therefore shoots a prefix: every
line up to the last one that makes something the scope's pack reads, with exactly the history it has in a full
capture, and none after it. What the pack reads of a scope (pack.write_focus) is:

- the shots of its sheets' tiles (tile_matches, which pack.py uses to pick them);
- the recordings of its sheets, strips, pairs and facts_strips;
- the facts of its key prefixes, of its lead and at-a-glance tables and of its plate tiles;
- the floor regions, accessibility events and contrast rows it names.

What each line makes is read from the state table and, for macros, from states.py's source: every writer of those is
one of a few helpers (_fact, _save_shot, index.append, floors.jsonl, a11y-events, pending_measures), and a macro
makes what the helpers' calls it can reach say, by their literal keys or prefixes; a key known only as the macro
runs counts as any key. So a line counts as needed more often than it is, never less. Where that cannot be relied on
(a writer this cannot read, an image it cannot trace to lines), prefixes() says why and the capture shoots whole
sessions.
"""
import ast
import os

ANY = ("", False)  # a key known only as the macro runs: (prefix, exact)
# the helpers whose calls say what a macro makes; their own bodies only write what they are given
HELPERS = ("_fact", "_save_shot")


def tile_matches(e, area, state, theme, variant="", scale=1):
    """Whether the shot of index entry `e` is the custom sheet tile (area, state) in that theme, variant (e.g.
    "font24": that variant, at any window size it was shot at) and scale."""
    if not (e["area"] == area and e["state"] == state and e["theme"] == theme and e["scale"] == scale):
        return False
    if variant:
        return variant in e["file"] and ("font24" in variant or "font24" not in e["file"])
    return e.get("size") == "1600x1000" and "font24" not in e["file"]


def may_be(key, want):
    """Whether a key (prefix, exact) can be the key `want`."""
    prefix, exact = key
    return want == prefix if exact else want.startswith(prefix)


def may_start(key, want):
    """Whether a key (prefix, exact) can start with `want`."""
    prefix, exact = key
    return prefix.startswith(want) or (not exact and want.startswith(prefix))


def literal_keys(node):
    """The (prefix, exact) of each value a string expression can have, as far as the source says."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return [(node.value, True)]
    if isinstance(node, ast.JoinedStr):
        head = ""
        for part in node.values:
            if not isinstance(part, ast.Constant):
                return [(head, False)]
            head += part.value
        return [(head, True)]
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        return [(prefix, False) for prefix, _ in literal_keys(node.left)]
    if isinstance(node, ast.IfExp):
        return literal_keys(node.body) + literal_keys(node.orelse)
    return [ANY]


def _index_append(call):
    """The dict(...) of an `<x>.index.append(dict(...))` call, else None."""
    f = call.func
    if (isinstance(f, ast.Attribute) and f.attr == "append" and isinstance(f.value, ast.Attribute)
            and f.value.attr == "index" and len(call.args) == 1):
        arg = call.args[0]
        if isinstance(arg, ast.Call) and isinstance(arg.func, ast.Name) and arg.func.id == "dict":
            if {"area", "state"} <= {k.arg for k in arg.keywords}:
                return arg
    return None


class Writers:
    """What the functions of a states.py source write."""

    def __init__(self, source):
        tree = ast.parse(source)
        self.defs = {}  # a module-level name -> the nodes that define it
        for node in tree.body:
            names = ([node.name] if isinstance(node, (ast.FunctionDef, ast.ClassDef)) else
                     [t.id for tg in node.targets for t in ast.walk(tg) if isinstance(t, ast.Name)]
                     if isinstance(node, ast.Assign) else [])
            for name in names:
                self.defs.setdefault(name, []).append(node)
        self._of = {}
        self.problems = self._problems()

    def _problems(self):
        """The writes this cannot read: with any, no prefix is safe."""
        out = []
        for name, nodes in self.defs.items():
            for top in nodes:
                if not isinstance(top, (ast.FunctionDef, ast.ClassDef)):
                    continue
                read = set()  # the references understood
                for n in ast.walk(top):
                    if isinstance(n, ast.Call):
                        f = n.func
                        if isinstance(f, ast.Name) and f.id in HELPERS:
                            if len(n.args) >= (3 if f.id == "_fact" else 5):
                                read.add(id(f))
                        elif isinstance(f, ast.Attribute):
                            if _index_append(n):
                                read.add(id(f.value))
                            elif f.attr == "index":  # list.index(), str.index()
                                read.add(id(f))
                            elif f.attr in ("run_steps", "step_strip", "step_macro"):
                                out.append(f"{name} calls {f.attr}: what it makes depends on the steps it is given")
                            elif f.attr in ("strip", "grab") and isinstance(f.value, ast.Attribute) and f.value.attr == "s":
                                out.append(f"{name} calls the session's {f.attr}() itself")
                        elif isinstance(f, ast.Name) and f.id == "reversed" and n.args:  # (the index read back)
                            read.add(id(n.args[0]))
                for n in ast.walk(top):
                    if isinstance(n, ast.Name) and n.id in HELPERS and id(n) not in read:
                        out.append(f"{name} uses {n.id} other than by calling it with its key (line {n.lineno})")
                    elif isinstance(n, ast.Attribute) and n.attr == "index" and id(n) not in read:
                        out.append(f"{name} writes the shot index in a way this cannot read (line {n.lineno})")
                    elif isinstance(n, ast.Constant) and n.value == "facts.jsonl" and name != "_fact":
                        out.append(f"{name} writes facts.jsonl itself")
                    elif isinstance(n, ast.Attribute) and n.attr == "pending_measures" and name not in (
                            "m_measure", "m_measure_na"):
                        out.append(f"{name} adds contrast measures itself")
        return out

    def of(self, name):
        """What the module-level function `name` can write, itself or through what it refers to: ("fact", key),
        ("shot", area key, state key), ("floor",), ("a11y-events",)."""
        if name in self._of:
            return self._of[name]
        if name not in self.defs:  # (not this module's: nothing known of it)
            self._of[name] = {("fact", ANY), ("shot", ANY, ANY), ("floor",), ("a11y-events",)}
            return self._of[name]
        out, seen, todo = set(), {name}, list(self.defs[name])
        while todo:
            for n in ast.walk(todo.pop()):
                if isinstance(n, ast.Name) and n.id in self.defs and n.id not in seen:
                    seen.add(n.id)
                    if n.id not in HELPERS:
                        todo += self.defs[n.id]
                elif isinstance(n, ast.Constant) and n.value == "floors.jsonl":
                    out.add(("floor",))
                elif isinstance(n, ast.Constant) and n.value == "a11y-events":
                    out.add(("a11y-events",))
                elif isinstance(n, ast.Call):
                    if isinstance(n.func, ast.Name) and n.func.id == "_fact" and len(n.args) > 1:
                        out |= {("fact", k) for k in literal_keys(n.args[1])}
                    elif isinstance(n.func, ast.Name) and n.func.id == "_save_shot" and len(n.args) > 3:
                        out |= {("shot", a, s) for a in literal_keys(n.args[2]) for s in literal_keys(n.args[3])}
                    else:
                        entry = _index_append(n)
                        if entry:
                            kw = {k.arg: k.value for k in entry.keywords}
                            out |= {("shot", a, s) for a in literal_keys(kw["area"]) for s in literal_keys(kw["state"])}
        self._of[name] = out
        return out


class Needs:
    """What a scope's pack reads (pack.write_focus and what it calls)."""

    def __init__(self, scope, states_mod):
        self.problems = []
        self.tiles = set()  # (area, state, theme, variant, scale)
        self.recordings = set()  # (name, theme)
        self.traced = set()  # those of the images: lines must make them, or whole sessions are shot
        facts = scope.get("facts")
        facts = [""] if facts is None else list(facts)  # (no list: every fact)
        # (write_focus notes any card's details line without its "of M", whatever the scope's facts)
        facts.append("card-")
        if scope.get("fact_table"):
            facts.append("dlg-")
        if scope.get("lead_table"):
            facts.append(scope["lead_table"]["prefix"])
        for _name, _title, theme, rows in scope.get("sheets", []):
            for row in rows:
                for cell in row:
                    column = [cell] if isinstance(cell, dict) or isinstance(cell[0], str) else cell
                    for c in column:
                        if isinstance(c, dict):
                            self.recordings.add((c["strip"], c.get("theme", theme)))
                            continue
                        opts = (c[4] if len(c) > 4 else None) or {}
                        self.tiles.add((c[0], c[1], opts.get("theme", theme), opts.get("variant", ""), opts.get("scale", 1)))
                        if opts.get("plate"):
                            facts.append(f"plate-{c[1]}-")
        self.facts = tuple(set(facts))
        pairs = {f"pair:{nm}": (a, b) for nm, a, b, *_ in getattr(states_mod, "STRIP_PAIRS", [])}
        perceived = {nm: rows for nm, _title, rows in getattr(states_mod, "PERCEIVED_SHEETS", [])}
        views = {f"{nm}-{suffix}": nm for nm, vs in getattr(states_mod, "EXTRA_VIEWS", {}).items() for suffix, _ in vs}
        views.update({f"{nm}-1to1": nm for nm in getattr(states_mod, "FRAMES11", {})})
        own = {name for name, *_ in scope.get("sheets", [])}
        for kind, name, theme, *_ in scope["images"]:
            if kind == "sheet":
                if name not in own:
                    self.problems.append(f"the image {name} is a sheet of the whole pack")
                continue
            if name in pairs:
                found = [(n, t or theme) for n, _, t in (half.partition("@") for half in pairs[name])]
            elif name in perceived:
                found = [(row[0], row[1]) for row in perceived[name]]
            else:
                found = [(views.get(name, name), theme)]
            self.traced.update(found)
        self.recordings |= self.traced | {(nm, th) for nm, th, *_ in scope.get("facts_strips", [])}
        self.floors = scope.get("floors")  # None: all
        self.a11y = scope.get("a11y_labels") or []
        self.contrast = {p for ps in (scope.get("contrast") or {}).values() for p in ps}


def steps_of(text):
    """A line's (verb, argument)s, a strip's action steps included, as runner.Runner.run_steps runs them."""
    for step in [t.strip() for t in text.split(" | ") if t.strip()]:
        verb, _, rest = step.partition(" ")
        yield verb, rest.strip()
        if verb == "strip":
            yield from steps_of(rest.partition("::")[2].replace(" ; ", " | "))


def pack_needs(scope, states_mod):
    """What a focus-only pack builds of a round: (the names of the recordings it analyses, those its images and
    facts_strips read; the names of the strip images it shows or quotes, of which the pairs, perceived and frame
    sheets are made)."""
    analysed = {name for name, _ in Needs(scope, states_mod).traced}
    quoted = {name for name, *_ in scope.get("facts_strips", [])}
    return analysed | quoted, quoted | {name for kind, name, *_ in scope.get("images", []) if kind == "strip"}


def recorded(area, state, theme, states_mod):
    """Whether a strip step of this line records in this theme (runner.run_session's no_record)."""
    return not (area == "motion" and theme != "dark" and state not in states_mod.STRIP_BOTH_THEMES)


def prefixes(scope, jobs, states_mod, what, suffix_of, source=None):
    """({(session, theme): lines to shoot}, problems) for a scope's (session, theme) `jobs`: a job without lines is
    not needed at all. With problems, the dict is empty: shoot whole sessions. `what` is contrast.WHAT (a measure's
    label as CONTRAST.md names it), `suffix_of(theme, session's keywords)` the shots' file suffix."""
    if source is None:
        with open(os.path.splitext(states_mod.__file__)[0] + ".py", encoding="utf-8") as f:
            source = f.read()
    writers = Writers(source)
    need = Needs(scope, states_mod)
    problems = writers.problems + need.problems
    makers = {name: fn.__name__ for name, fn in states_mod.MACROS.items()}
    out, made = {}, set()
    for session, theme in jobs:
        kw = states_mod.SESSIONS[session]
        w, h = kw.get("size", (1600, 1000))
        scale, suffix = kw.get("scale", 1), suffix_of(theme, kw)
        base = dict(theme=theme, scale=scale, size=f"{w}x{h}")

        def shot(area_key, state_key):
            for area, state, th, variant, sc in need.tiles:
                if may_be(area_key, area) and may_be(state_key, state):
                    e = dict(base, area=area, state=state, file=f"{area}_{state}_{suffix}.png")
                    if tile_matches(e, area, state, th, variant, sc):
                        return True
            return False

        def macro(name, arg, grabs):
            if name not in makers:
                return True  # (it will fail as it does in a full capture)
            if grabs and name in ("measure", "measure_na"):
                label = arg.split(" ", 1)[0]
                if any(p in what.get(label, label).lower() for p in need.contrast):
                    return True
            for kind, *key in writers.of(makers[name]):
                if kind == "fact" and any(may_start(key[0], p) for p in need.facts):
                    return True
                if kind == "shot" and shot(*key):
                    return True
                if kind == "floor" and arg not in ("save", "restore"):
                    if need.floors is None or arg.partition(":")[0] in need.floors:
                        return True
                if kind == "a11y-events" and need.a11y:
                    return True
            return False

        last = 0
        for n, (_session, area, state, steps, _desc) in enumerate([x for x in states_mod.STATES if x[0] == session], 1):
            grabs = not state.startswith("_") and area != "motion"
            needed = grabs and shot((area, True), (state, True))
            for verb, rest in steps_of(steps):
                if verb == "strip" and recorded(area, state, theme, states_mod):
                    for rec in need.recordings:
                        if f"{state}_{suffix}".startswith(f"{rec[0]}_{rec[1]}_"):  # (as pack.py finds recordings)
                            needed = True
                            made.add(rec)
                elif verb == "macro":
                    name, _, arg = rest.partition(" ")
                    needed = macro(name, arg.strip(), grabs) or needed
            if needed:
                last = n
        if last:
            out[(session, theme)] = last
    problems += [f"no line records {name} ({theme})" for name, theme in sorted(need.traced - made)]
    if not out:
        problems.append("no line makes anything the scope reads")
    return ({}, problems) if problems else (out, [])
