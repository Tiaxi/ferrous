#!/usr/bin/env python3
"""The screenshot pack as data: sessions, states (one line each), macros, and what is
not captured. Read by runner.py; see runner.py's docstring for the step syntax.

A line: (session, area, state, steps, description).
- state "_name": setup only, no grab (always runs, also under --only).
- area "motion": the steps end with `strip DUR PRE CROP :: action ; action`, which records every
  frame the window renders while the action runs; strips.py turns it into a film strip.
- Steps run in order, in one app per (session, theme); later lines see the state earlier ones left.
"""
import contextlib
import fcntl
import functools
import glob
import json
import re
import os
import time

from driver import ICONPROBE
from shotlib import FIND, KEY, LIB, LIB2, album_paths
from colours import unique_colours

KEY_TAB, KEY_ESC, KEY_SPACE = KEY["Tab"], KEY["Escape"], KEY["Space"]

# Session kinds: shotlib.Session keyword arguments, plus the themes to run.
SESSIONS = {
    "empty": dict(db=False),
    "firstrun-defaults": dict(db=False, app_defaults=True),  # (a new install's first window; both themes)
    "main": dict(),
    "playing": dict(),
    "transport": dict(),
    "statusbar": dict(),
    "menus": dict(),
    "dialogs": dict(),
    "viewers": dict(),
    "focus": dict(),
    "roots": dict(),
    "motion": dict(),
    "motion-ui": dict(),
    "narrow-empty": dict(size=(1000, 720), db=False),
    "narrow": dict(size=(1000, 720)),
    "hidpi": dict(scale=2),
    "bigfont": dict(font_pt=24, suffix_extra="_font24"),
    "narrow-bigfont": dict(size=(1000, 720), font_pt=24, suffix_extra="_font24"),
    "wide-bigfont": dict(size=(1920, 1200), font_pt=24, suffix_extra="_font24", themes=("dark",)),
    "tall-bigfont": dict(size=(1600, 1400), font_pt=24, suffix_extra="_font24", themes=("dark",)),
    "animoff": dict(anim_off=True, suffix_extra="_animoff", themes=("dark",)),
    "density": dict(db=False),  # (both themes)
    "firstplay-noart": dict(),  # (both themes)  # idle with the library, then a track without art  # the large invented library (make_large_library.py)
    "contrast": dict(),
    "queue": dict(),  # (the queue's row states, its focus with and without a scrollbar, an unreadable row)
    "icons": dict(db=False),  # (both themes, for the app-icon scope)
    "icon-noart24": dict(font_pt=24, suffix_extra="_font24"),  # (the 24 pt compact no-art mark, 152 px)
    # zoom, the seek bar's view band, middle-drag pan, middle-click autoscroll and the way back to the playhead
    "viz-interaction": dict(),
}


# Fake Last.fm state for the status bar's scrobble indicator: the backend has no network here, so
# the indicator gets a stand-in bridge whose Last.fm fields are set; everything else is the real one.
FAKE_LASTFM = ("(function(over){var real=root.uiBridge;var o={lastFmShowScrobbleProgress:true,"
               "lastFmScrobblingEnabled:true,lastFmAuthState:2,lastFmScrobblingPaused:false,lastFmScrobbleState:1,"
               "lastFmScrobbleIneligibility:0,lastFmScrobbleListenedMs:0,lastFmScrobbleThresholdMs:120000,"
               "lastFmPendingScrobbleCount:0,lastFmScrobbleMessage:''};for(var k in over)o[k]=over[k];"
               "var p=new Proxy(real,{get:function(t,k){if(k in o)return o[k];var v=t[k];"
               "return typeof v==='function'?v.bind(t):v}});"
               "var s=[root.footer];var ind=null;while(s.length){var it=s.shift();"
               "if(it.objectName==='scrobbleIndicator'){ind=it;break}var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}"
               "ind.uiBridge=p;return true})")

FAKE_ITUNES = ("(function(rows, loading, status){var real=root.uiBridge;"
               "var o={itunesArtworkResults:rows,itunesArtworkLoading:loading,itunesArtworkStatusText:status,"
               "itunesArtworkResultAt:function(i){return rows[i]||({})},"
               "searchCurrentTrackArtworkSuggestions:function(){},clearItunesArtworkSuggestions:function(){},"
               "prepareItunesArtworkSuggestion:function(i){},"
               # the target the real search would set (BridgeClient::searchCurrentTrackArtworkSuggestions: the
               # current track's album, artist and cover), so Apply asks as the app does; applying never writes here
               "itunesArtworkTarget:{trackPath:real.currentTrackPath,album:real.currentTrackAlbum.trim(),"
               "artist:real.currentTrackArtist.trim(),coverPath:real.currentTrackCoverPath},"
               "applyItunesArtworkSuggestion:function(i){}};"
               "var p=new Proxy(real,{get:function(t,k){if(k in o)return o[k];var v=t[k];"
               "return typeof v==='function'?v.bind(t):v}});itunesArtworkDialog.uiBridge=p;return true})")

# @NAME targets for steps and crops: JS expressions that return an item (`point`/`rect_spec` in runner.py).
TARGETS = {
    # The first on-screen queue row below the top two that is neither selected nor playing.
    "plainQueueRow": "(function(){var o=[];var s=[root.Overlay.overlay.parent];while(s.length){var it=s.shift();"
                     "if(!it.visible)continue;if(it.objectName==='queueRow'&&!it.rowSelected&&!it.isCurrentQueueRow){"
                     "var p=it.mapToItem(null,0,0);var v=" + FIND + "('playlistView');var q=v.mapToItem(null,0,0);"
                     "if(p.y>=q.y+2*it.height&&p.y+it.height<=q.y+v.height)o.push([p.y,it])}var c=it.children;"
                     "for(var i=0;i<c.length;i++)s.push(c[i]);}o.sort(function(a,b){return a[0]-b[0]});"
                     "return o.length?o[0][1]:null})()",
    # The top search result that has a tooltip: one only where it adds to the row (toolTipAdds), where the app says so.
    "tooltipResultRow": "(function(){var o=[];var s=[root.Overlay.overlay.parent];while(s.length){var it=s.shift();"
                        "if(!it.visible||it.opacity===0)continue;if(typeof it.toolTipText==='function'&&it.isResult&&"
                        "(typeof it.toolTipAdds!=='function'||it.toolTipAdds()))o.push([it.mapToItem(null,0,0).y,it]);"
                        "var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}o.sort(function(a,b){return a[0]-b[0]});"
                        "return o.length?o[0][1]:null})()",
}

TARGETS_PLAIN = TARGETS["plainQueueRow"]

# The k-th visible, on-screen item named NAME, top to bottom (as shotlib.visible_rects).
FIND_VIS = ("(function(){{var o=[];var s=[root.Overlay.overlay.parent];while(s.length){{var it=s.shift();"
            "if(!it.visible||it.opacity===0)continue;if(it.objectName==='{name}'){{var p=it.mapToItem(null,0,0);"
            "if(p.y+it.height/2>=0&&p.y+it.height/2<root.height)o.push([p.y,p.x,it])}}var c=it.children;"
            "for(var i=0;i<c.length;i++)s.push(c[i]);}}o.sort(function(a,b){{return a[0]-b[0]||a[1]-b[1]}});"
            "return o.length>{k}?o[{k}][2]:null}})()")

# Walks every object (items, popups, data) from the window for an objectName.
FIND_ANY = ("(function(n){var seen=[];var s=[root];while(s.length){var it=s.shift();if(!it||seen.indexOf(it)>=0)continue;"
            "seen.push(it);if(it.objectName===n)return it;var lists=[it.data,it.contentData,it.children];"
            "for(var l=0;l<lists.length;l++){var c=lists[l];if(!c||c.length===undefined)continue;"
            "for(var i=0;i<c.length;i++)if(c[i])s.push(c[i]);}}return null})")


def m_lastfm(r, arg):
    r.s.ev(f"{FAKE_LASTFM}({arg or '{}'})")
    time.sleep(0.15)


def itunes_rows():
    jpgs = sorted(glob.glob(f"{LIB}/*/*/cover.jpg"))[:5]
    if len(jpgs) < 5:  # loudly: never a results list shorter than its caption says
        raise FileNotFoundError(f"itunes: {len(jpgs)} fixture covers in {LIB}, need 5")
    titles = ["Small Hours", "Small Hours (Deluxe Edition)", "Small Hours – Live", "Hours, Small",
              "The Small Hours Sessions"]
    res = ["1200 × 1200", "3000 × 3000", "600 × 600", "1400 × 1400", "1000 × 1000"]
    sizes = ["412 KiB", "2.1 MiB", "88 KiB", "530 KiB", "301 KiB"]
    return [dict(albumTitle=titles[k], artistName="The Quiet Meridian", resolutionText=res[k], fileType="JPEG",
                 fileSizeText=sizes[k], mimeType="image/jpeg", previewSource="file://" + j, normalizedPath=j,
                 normalizedUrl="file://" + j, assetLoading=(k == 4), assetError="") for k, j in enumerate(jpgs)]


def itunes_status(kind, n=5):
    """The app's own iTunes status text (BridgeClient.cpp in the round's source), never a stand-in: the empty,
    searching and found strings, the plural form for N. Fails loudly when the source has no such string."""
    q = os.path.join(os.environ.get("SHOT_SRC", ""), "ui/src/BridgeClient.cpp")
    txt = open(q, encoding="utf-8").read()
    if kind == "found":
        m = re.search(r'i18ncp\("@info:status",\s*"(Found %1 suggestion\.[^"]*)",\s*"(Found %1 suggestions\.[^"]*)"', txt)
        if m:
            return m.group(1 if n == 1 else 2).replace("%1", str(n))
    else:
        pat = {"empty": r'"(No iTunes artwork suggestions found\.)"', "searching": r'"(Searching iTunes[^"]*)"',
               "failed": r'"(iTunes search failed: %1)"'}[kind]
        m = re.search(r'i18nc\("@info:status",\s*' + pat, txt)
        if m:  # a failed search: Qt's own wording for a host lookup that fails, as with no network
            return m.group(1).replace("%1", "Host itunes.apple.com not found")
    raise LookupError(f"itunes: no '{kind}' status string in {q}")


def m_itunes(r, arg):
    """The iTunes dialog over a stand-in bridge (no network): its rows invented, its status text the app's."""
    if arg == "empty":
        r.s.ev(f"{FAKE_ITUNES}([], false, {json.dumps(itunes_status('empty'))})")
    elif arg == "searching":
        r.s.ev(f"{FAKE_ITUNES}([], true, {json.dumps(itunes_status('searching'))})")
    elif arg == "error":
        r.s.ev(f"{FAKE_ITUNES}([], false, {json.dumps(itunes_status('failed'))})")
    elif arg in ("results", "loading"):
        rows = itunes_rows()
        if arg == "loading":  # as the app marks a row while its full-size image downloads (assetLoading)
            rows[0] = dict(rows[0], assetLoading=True)
        r.s.ev(f"{FAKE_ITUNES}({json.dumps(rows)}, false, {json.dumps(itunes_status('found', len(rows)))})")
    else:
        raise ValueError(f"itunes: unknown state '{arg}'")
    time.sleep(0.3)


def m_fsitem(r, arg):
    """fsitem hover|focus|reset NAME: an item of the fullscreen album art viewer's own window (its strip's info
    button): `hover` moves the pointer onto it in that window and waits for its tooltip's delay; `focus` gives it
    keyboard focus (Tab reason: its ring); `reset` moves the pointer to the window's middle and the focus back to the
    view (NAME: the view's focus item), so no tooltip or ring is left for the next state."""
    verb, _, name = arg.partition(" ")
    name, _, kind = name.partition(" ")
    kind = kind or "albumArt"  # or "spectrogram": the visualization viewer's fullscreen window
    win = f"{kind}FullscreenWindow"
    js = ("(function(){var l=" + FIND_ANY + f"('{kind}FullscreenWindowLoader');var w=l&&l.item;if(!w)return '';"
          "var s=[w.contentItem];while(s.length){var it=s.shift();if(!it)continue;if(it.objectName===" + json.dumps(name)
          + "&&it.visible){" + ("it.forceActiveFocus(Qt.TabFocusReason);" if verb == "focus" else "")
          + ("it.forceActiveFocus(Qt.OtherFocusReason);" if verb == "reset" else "")
          + "var p=it.mapToItem(null,0,0);return [p.x,p.y,it.width,it.height,w.width,w.height].join(' ')}"
          "var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}return ''})()")
    v = r.s.ev(js)
    if not v:
        raise LookupError(f"fsitem: no {name} shown in {win}")
    x, y, w, h, ww, wh = map(float, v.split())
    if verb == "hover":
        r.s.cmd(f"move@{win} {x + w / 2:.1f} {y + h / 2:.1f}")
        time.sleep(1.5)
    elif verb == "reset":
        r.s.cmd(f"move@{win} {ww / 2:.1f} {wh / 2:.1f}")
        time.sleep(0.6)
    else:
        time.sleep(0.4)


def m_screenfacts(r, arg):
    """screenfacts: the session's screen (KWin's virtual output), its work area and the main window's size:
    fact screen-SUFFIX."""
    v = r.s.ev("JSON.stringify([root.screen.width,root.screen.height,root.screen.devicePixelRatio,"
               "root.screen.desktopAvailableWidth,root.screen.desktopAvailableHeight,root.width,root.height])")
    sw, sh, dpr, aw, ah, mw, mh = json.loads(v)
    gu = r.s.ev("(function(){try{return JSON.stringify([Kirigami.Units.gridUnit,Kirigami.Units.largeSpacing,"
                "Qt.application.font.family,Qt.application.font.pointSize,Qt.application.font.pixelSize])}"
                "catch(e){return JSON.stringify([null,null,'',0,0])}})()")
    g_u, l_s, fam, fpt, fpx = json.loads(gu or "[null,null,\"\",0,0]")
    _fact(r, f"screen-{r.s.suffix}", f"Screen ({r.s.suffix.replace('_', ' ')}): {sw}x{sh} logical px at scale {dpr:g} "
          f"(KWin's virtual output; no panel: work area {aw}x{ah}); the main window {mw}x{mh}; a fullscreen viewer "
          f"fills the screen; Kirigami.Units.gridUnit {g_u}, largeSpacing {l_s}; the application font {fam} "
          f"{fpt} pt ({fpx} px)")


def m_itunesfail(r, arg):
    """itunesfail: a real failed iTunes search, through the app's own path: Qt's application proxy set to
    127.0.0.1:9 in the app (probe "netproxy": every request refused at once; nothing leaves the machine, as in the
    app's own test), the dialog given the real bridge back (not the stand-in), the real search for the current track;
    BridgeClient sets itunesArtworkFailed and its texts. Loud when the proxy is not in place (then no search runs) or
    the search does not fail within 15 s."""
    got = str(r.s.cmd("netproxy 127.0.0.1 9")).split()
    if got[-2:] != ["127.0.0.1", "9"]:
        raise RuntimeError(f"itunesfail: the application proxy is not 127.0.0.1:9 ({got}); no search was started")
    # the real bridge and its search in one evaluation: as two, a frame between them drew the bridge's earlier
    # (stand-in) results for about 30 ms, a flash the app never shows (the strip itunes-search-fails caught it)
    r.s.ev("(function(){itunesArtworkDialog.uiBridge = root.uiBridge; root.uiBridge.searchCurrentTrackArtworkSuggestions(); "
           "return root.uiBridge.itunesArtworkLoading})()")
    if not r.s.a.wait_for("root.uiBridge.itunesArtworkFailed === true && !root.uiBridge.itunesArtworkLoading",
                          timeout=15, interval=0.1):
        raise RuntimeError("itunesfail: the search did not fail within 15 s")
    time.sleep(0.5)
    v = r.s.ev("JSON.stringify([root.uiBridge.itunesArtworkStatusText,root.uiBridge.itunesArtworkErrorText||''])")
    st, err = json.loads(v)
    _fact(r, f"itunes-failed-{r.s.tag}", f"iTunes search failed through the app's path (proxy 127.0.0.1:9 refused): "
          f"itunesArtworkFailed true; status \u201c{st}\u201d; error text \u201c{err}\u201d ({r.s.suffix.replace('_', ' ')})")


# The fixture macros' folders are the run's, not a session's: dialogs show their whole paths, which must not change
# with the worker that runs the session. Sessions in both themes and sizes run these macros at the same time, so a
# macro that changes its files runs one at a time, and the others only ever replace a file whole.
def one_at_a_time(macro):
    """The macro, run by one session of the run at a time."""
    @functools.wraps(macro)
    def locked(r, arg):
        with open(os.path.join(os.environ["TMPDIR"], f".{macro.__name__}.lock"), "w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            return macro(r, arg)
    return locked


def put_fixture(path, data):
    """Writes a fixture file that another session may be reading: replaced whole, never seen missing or cut short."""
    tmp = os.path.join(os.path.dirname(path), f".{os.path.basename(path)}.{os.getpid()}")
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


@one_at_a_time
def m_tagsavefail(r, arg):
    """tagsavefail: Tag Editor Details with real content: two fixture tracks copied into this run's own TMPDIR,
    one in a folder it cannot write to (the app writes atomically: a read-only file alone would still be replaced),
    opened in Edit Tags, a comment set on both, saved: one saved, one failed; the folder made writable again after."""
    import shutil
    base = os.path.join(os.environ["TMPDIR"], "ferrous-tagsave")
    if os.path.isdir(base):
        for root_, dirs, _f in os.walk(base):
            for d_ in dirs:
                os.chmod(os.path.join(root_, d_), 0o755)
        shutil.rmtree(base)
    ok_d, ro_d = os.path.join(base, "writable"), os.path.join(base, "read-only")
    os.makedirs(ok_d)
    os.makedirs(ro_d)
    src = album_paths("Orchid Static/2019 - Loose Ends")
    a_, b_ = os.path.join(ok_d, os.path.basename(src[0])), os.path.join(ro_d, os.path.basename(src[2]))
    shutil.copyfile(src[0], a_)
    shutil.copyfile(src[2], b_)
    os.chmod(ro_d, 0o555)
    try:
        r.s.ev("root.tagEditorApi.revertField('title')")  # (the earlier states' typed edit: nothing unsaved)
        if str(r.s.ev(f"root.tagEditorApi.openForPaths({json.dumps([a_, b_])}) && (tagEditorDialog.open(), true)")).lower() != "true":
            raise RuntimeError("tagsavefail: Edit Tags did not open the two copies")
        r.s.a.wait_for("!root.tagEditorApi.loading", timeout=5, interval=0.05)
        time.sleep(0.4)
        r.s.ev("(function(){root.tagEditorApi.setCell(0,'comment','Checked');root.tagEditorApi.setCell(1,'comment','Checked');return true})()")
        r.s.ev("root.tagEditorApi.save()")
        if not r.s.a.wait_for("!root.tagEditorApi.saving && root.tagEditorApi.statusDetails.length > 0", timeout=15, interval=0.1):
            raise RuntimeError("tagsavefail: the save gave no details within 15 s")
        time.sleep(0.4)
    finally:
        os.chmod(ro_d, 0o755)
    st = r.s.ev("root.tagEditorApi.statusText")
    _fact(r, f"tagsave-{r.s.tag}", f"Tag editor save with one folder not writable: status \u201c{st}\u201d "
          f"({r.s.suffix.replace('_', ' ')})")


# the Rename Files batch: twelve invented titles, long and short (five keep their names, seven are renamed); a
# file whose name already is the app's rule
# ("{track:02} - {title}.{ext}", src/tag_editor rename_target_path_for_row) keeps it, the others are renamed
RENAME_BATCH = [
    ("01 - Overture for Strings and a Distant Thunderstorm.flac", "Overture for Strings and a Distant Thunderstorm"),
    ("02 - Morning Tide Field.flac", "Morning Tide Field (Live at the Harbour Hall, Second Night)"),
    ("03 - Cinder Lumen.flac", "Cinder Lumen"),
    ("track04.flac", "The Extraordinarily Long-Named Interlude Between Two Rooms"),
    ("05 - Static.flac", "Static (Extended Version)"),
    ("06 - Moth Arc.flac", "Moth Arc"),
    ("07 - A Quiet Meridian.flac", "A Quiet Meridian, Seen from the Northbound Platform at Dawn"),
    ("08 - Shore.flac", "Shore"),
    ("Ember.flac", "Ember (Remastered)"),
    ("10 - Lantern Signal Reprise.flac", "Lantern Signal Reprise"),
    ("11 - Small Hours.flac", "Small Hours / Coda in Three Unhurried Parts"),
    ("12 - Afterward.flac", "Afterword"),
]


def m_renamebatch(r, arg):
    """renamebatch: Edit Tags on twelve copies of one fixture track in this run's own TMPDIR, named as in
    RENAME_BATCH; each row given its track number (of 12), title and disc (1 for the first six, 2 after: Track and Disc
    both mixed, so the form shows their placeholders) (unsaved: the preview reads the table), all rows selected.
    Five keep their names, seven are renamed (long titles, one with a slash the rule turns into a space)."""
    base = os.path.join(os.environ["TMPDIR"], "ferrous-renamebatch")
    os.makedirs(base, exist_ok=True)
    with open(album_paths("Orchid Static/2019 - Loose Ends")[0], "rb") as f:
        track = f.read()
    paths = []
    for name, _t in RENAME_BATCH:
        paths.append(os.path.join(base, name))
        put_fixture(paths[-1], track)
    if str(r.s.ev(f"root.tagEditorApi.openForPaths({json.dumps(paths)}) && (tagEditorDialog.open(), true)")).lower() != "true":
        raise RuntimeError("renamebatch: Edit Tags did not open the twelve copies")
    if not r.s.a.wait_for("!root.tagEditorApi.loading && root.tagEditorApi.loadedPaths().length === 12", timeout=8, interval=0.05):
        raise RuntimeError("renamebatch: the twelve copies did not load")
    spec = {name: [f"{i + 1}", title] for i, (name, title) in enumerate(RENAME_BATCH)}
    n = r.s.ev("(function(){var s=" + json.dumps(spec) + ";var ps=root.tagEditorApi.loadedPaths();var n=0;"
               "for(var i=0;i<ps.length;i++){var v=s[ps[i].split('/').pop()];if(!v)continue;"
               "root.tagEditorApi.setCell(i,'trackNo',v[0]);root.tagEditorApi.setCell(i,'totalTracks','12');"
               "root.tagEditorApi.setCell(i,'title',v[1]);root.tagEditorApi.setCell(i,'discNo',i<6?'1':'2');"
               "root.tagEditorApi.setCell(i,'totalDiscs','2');n++}root.tagEditorApi.selectAllRows();return n})()")
    if int(n or 0) != 12:
        raise RuntimeError(f"renamebatch: {n} of 12 rows were given their tags")
    time.sleep(0.4)
    kept = sum(1 for i, (nm, t) in enumerate(RENAME_BATCH) if nm == f"{i + 1:02d} - {t}.flac")
    _fact(r, f"renamebatch-{r.s.tag}", f"Rename Files batch: twelve copies of one fixture track in the run's TMPDIR, "
          f"{kept} named by the rule already (they keep their names), {12 - kept} renamed; the longest new name "
          f"{max(len(f'{i + 1:02d} - {t}.flac') for i, (_n, t) in enumerate(RENAME_BATCH))} characters "
          f"({r.s.suffix.replace('_', ' ')})")


def m_quitwait(r, arg):
    """quitwait: Finishing Tag Changes with files: the dialog given a stand-in of the tag editor whose
    operationFiles are the first two loaded files' names (a save here takes milliseconds, so a real wait cannot be
    caught), at its second stage (offerChoice: Keep Waiting, Quit Anyway); opened."""
    ok = r.s.ev("(function(){var d=" + FIND_ANY + "('tagEditorQuitWaitDialog');if(!d)return false;var real=root.tagEditorApi;"
                "var names=(real.loadedPaths()||[]).slice(0,2).map(function(p){return p.split('/').pop()});"
                "d.tagEditorApi=new Proxy(real,{get:function(t,k){if(k==='operationFiles')return names;var v=t[k];"
                "return typeof v==='function'?v.bind(t):v}});d.offerChoice=true;d.open();return names.length})()")
    if not ok:
        raise LookupError("quitwait: no tagEditorQuitWaitDialog or no loaded files")
    r.settle_or(0.8)


def m_dlgtab(r, arg):
    """dlgtab NAME INDEX: in the dialog window shown, the TabBar NAME at INDEX (About's tabs)."""
    name, idx = arg.split()
    dw = r.s.dialog_window(timeout=1.0)
    if not dw:
        raise LookupError("dlgtab: no dialog window")
    js = ("(function(){var x=contentItem;while(x.parent)x=x.parent;var s=[x];while(s.length){var it=s.shift();if(!it)continue;"
          "if(it.objectName===" + json.dumps(name) + "){it.setCurrentIndex(" + str(int(idx)) + ");return true}"
          "var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}return false})()")
    rr = r.s.a.cmd(f"eval@{dw['cls']} {js}")
    if "true" not in str(rr):
        raise LookupError(f"dlgtab: no {name} in {dw['title']} ({rr})")
    time.sleep(0.5)


def m_lastfmaccount(r, arg):
    """lastfmaccount: Configure's Last.fm page with an account, as a stand-in: the page reads all of it from its
    own uiBridge, which gets a proxy of the real bridge saying the build has Last.fm, connected as an invented user
    with two scrobbles waiting and twelve discarded as too old (#676; no network, no credentials; the app's own
    wording)."""
    dw = r.s.dialog_window(timeout=1.0)
    if not dw:
        raise LookupError("lastfmaccount: no Configure window")
    js = ("(function(){var x=contentItem;while(x.parent)x=x.parent;var s=[x];while(s.length){var it=s.shift();if(!it)continue;"
          "if(it.objectName==='preferencesLastFmPage'){var real=it.uiBridge;var o={lastFmBuildConfigured:true,lastFmAuthState:2,"
          "lastFmUsername:'harbour-listener',lastFmStatusText:'',lastFmPendingScrobbleCount:2,"
          "lastFmDroppedScrobbleCount:12};"
          "it.uiBridge=new Proxy(real,{get:function(t,k){if(k in o)return o[k];var v=t[k];"
          "return typeof v==='function'?v.bind(t):v}});return true}"
          "var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}return false})()")
    rr = r.s.a.cmd(f"eval@{dw['cls']} {js}")
    if "true" not in str(rr):
        raise LookupError(f"lastfmaccount: no Last.fm page in {dw['title']} ({rr})")
    time.sleep(0.5)


def m_platefact(r, arg):
    """platefact STATE NAME WHAT: a viewer's corner plate or strip (item NAME, in the main window or a
    fullscreen viewer's window) at the grab: waits until it is at rest (its effective opacity, its own times every
    ancestor's, unchanged over three samples 120 ms apart; at most 3 s), then records fact plate-STATE-SUFFIX: shown or
    auto-hidden, its own and effective opacity, and a button's in it (its glyph's), with the samples taken.
    Run as the state's last step (nothing changes until the grab)."""
    state, name, what = arg.split(" ", 2)
    js = ("(function(){var it=" + FIND_ANY + "(" + json.dumps(name) + ");"
          "if(!it){['albumArtFullscreenWindowLoader','spectrogramFullscreenWindowLoader'].forEach(function(n){"
          "var l=" + FIND_ANY + "(n);if(it||!l||!l.item||!l.item.visible)return;var x=l.item.contentItem;while(x.parent)x=x.parent;"
          "var s=[x];while(s.length){var c0=s.shift();if(!c0)continue;if(c0.objectName===" + json.dumps(name) + "){it=c0;return}"
          "var c=c0.children;for(var i=0;i<c.length;i++)s.push(c[i]);}})}"
          "if(!it)return '';function eff(x){var o=1;while(x){o*=(x.visible===false?0:x.opacity);x=x.parent}return o}"
          "var b=null,s=[it];while(s.length&&!b){var c0=s.shift();if(c0!==it&&c0.objectName&&/Button$/.test(c0.objectName)&&c0.visible)b=c0;"
          "var c=c0.children;for(var i=0;i<c.length;i++)s.push(c[i]);}"
          "return JSON.stringify([it.visible,it.opacity,eff(it),b?b.objectName:'',b?eff(b):null])})()")
    samples, t0 = [], time.time()
    while True:
        v = r.s.ev(js)
        if not v:
            raise LookupError(f"platefact: no {name}")
        samples.append(json.loads(v))
        last = [round(x[2], 2) for x in samples[-3:]]
        if len(last) == 3 and len(set(last)) == 1:
            break
        if time.time() - t0 > 3.0:
            raise RuntimeError(f"platefact: {name} not at rest within 3 s (effective opacity {[x[2] for x in samples]})")
        time.sleep(0.12)
    vis, op, effv, bname, beff = samples[-1]
    shown = "shown" if vis and effv > 0.9 else ("auto-hidden" if effv < 0.05 else f"part-shown ({effv:.2f})")
    _fact(r, f"plate-{state}-{r.s.suffix}", f"{what.capitalize()} {name} at the grab of {state}: {shown}, at rest after "
          f"{(time.time() - t0) * 1000:.0f} ms (visible {vis}, its own opacity {op:.2f}, effective {effv:.2f}"
          + (f"; {bname} effective {beff:.2f}" if bname else "") + f"; samples {[round(x[2], 2) for x in samples]}) "
          f"({r.s.suffix.replace('_', ' ')})", what=what, shown=shown, effective=effv)


def m_fillfact(r, arg):
    """fillfact AREA/STATE ITEM KIND: a button's fill against its surface in the shot just grabbed (run as the
    next line): the most frequent colour inside the item (inset 3 px; its glyph is the minority) against the most
    frequent in a ring 1-3 px outside it (its plate's or dialog's surface, not beyond the plate), and their contrast; fact fill-STATE-ITEM-SUFFIX (KIND: rest, hover, pressed,
    checked, default). The item in the main window, a fullscreen viewer's window or the grabbed dialog window."""
    from collections import Counter
    from PIL import Image
    shot, item, kind = arg.split(" ", 2)
    area, _, state = shot.partition("/")
    file = f"{area}_{state}_{r.s.suffix}"
    e = next((x for x in reversed(r.s.index) if x["file"] == file + ".png"), None)
    if not e:
        raise LookupError(f"fillfact: no grab {file}")
    g = e.get("grabbed") or {}
    rect_js = ("var p=it.mapToItem(null,0,0);return JSON.stringify([p.x,p.y,it.width,it.height])")
    if g.get("kind") == "dialog":
        js = ("(function(){var x=contentItem;while(x.parent)x=x.parent;var s=[x];while(s.length){var it=s.shift();if(!it)continue;"
              "if(it.objectName===" + json.dumps(item) + "&&it.visible){" + rect_js + "}"
              "var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}return ''})()")
        rr = r.s.a.cmd(f"eval@{g['cls']} {js}")
        got = json.loads(rr)
        got = got[0] if isinstance(got, list) and got else got
        off = g.get("image_offset", [0, 0])
    else:
        js = ("(function(){var it=" + FIND_ANY + "(" + json.dumps(item) + ");"
              "if(!it){['albumArtFullscreenWindowLoader','spectrogramFullscreenWindowLoader'].forEach(function(n){"
              "var l=" + FIND_ANY + "(n);if(it||!l||!l.item||!l.item.visible)return;var x=l.item.contentItem;while(x.parent)x=x.parent;"
              "var s=[x];while(s.length){var c0=s.shift();if(!c0)continue;if(c0.objectName===" + json.dumps(item) + "&&c0.visible){it=c0;return}"
              "var c=c0.children;for(var i=0;i<c.length;i++)s.push(c[i]);}})}"
              "if(!it)return '';" + rect_js + "})()")
        got = r.s.ev(js)
        off = g.get("image_offset", [0, 0]) if g.get("kind") == "window" else [0, 0]
    if not got:
        raise LookupError(f"fillfact: no {item} shown in {file}")
    x, y, w, h = json.loads(got) if isinstance(got, str) else got
    x, y = x + off[0], y + off[1]
    im = Image.open(os.path.join(os.environ["SHOT_OUT"], file + ".png")).convert("RGB")
    cnt = lambda box: Counter(getattr(im.crop(box), "get_flattened_data", im.crop(box).getdata)())
    inside = cnt((int(x + 3), int(y + 3), int(x + w - 3), int(y + h - 3))).most_common(1)[0][0]
    ring = Counter()
    for box in ((x - 3, y - 3, x + w + 3, y - 1), (x - 3, y + h + 1, x + w + 3, y + h + 3),
                (x - 3, y, x - 1, y + h), (x + w + 1, y, x + w + 3, y + h)):  # (the plate's or dialog's own surface)
        b_ = (int(max(0, box[0])), int(max(0, box[1])), int(min(im.width, box[2])), int(min(im.height, box[3])))
        if b_[2] > b_[0] and b_[3] > b_[1]:
            ring.update(cnt(b_))
    surf = ring.most_common(1)[0][0]
    _fact(r, f"fill-{state}-{item}-{r.s.suffix}", f"{item} {kind} ({state}, {r.s.suffix.replace('_', ' ')}): fill "
          f"{_hexc(inside)} on its surface {_hexc(surf)}: {_contrast(inside, surf):.2f}:1", kind=kind, item=item,
          fill=_hexc(inside), surface=_hexc(surf), ratio=round(_contrast(inside, surf), 2))


def m_notefact(r, arg):
    """notefact KEY TEXT: a fact for the focus pack, as stated (how a state was reached)."""
    key, text = arg.split(" ", 1)
    _fact(r, f"{key}-{r.s.tag}", f"{text} ({r.s.suffix.replace('_', ' ')})")


def m_itunesapply(r, arg):
    """itunesapply N: the Replace Album Art confirmation the way the app opens it: Replace from iTunes'
    applySuggestion(N), which sets its message from the target (the stand-in bridge's, the current track's). Loud
    when the target has no cover (the app would then apply at once, with no confirmation) or no dialog opens."""
    v = r.s.ev("JSON.stringify(itunesArtworkDialog.uiBridge.itunesArtworkTarget||{})")
    t = json.loads(v or "{}")
    if not t.get("coverPath"):
        raise RuntimeError(f"itunesapply: the target has no cover ({t}); the app would apply without asking")
    r.s.ev(f"itunesArtworkDialog.applySuggestion({int(arg)})")
    r.step_waitpopup("ConfirmDialog 1.5")
    msg = r.s.ev(f"String({FIND_ANY}('replaceCoverConfirmDialog').message)")
    if not msg:
        raise RuntimeError("itunesapply: the confirmation opened without a message")


def m_open_child(r, arg):
    parent, typename = arg.split()
    ok = r.s.ev(f"(function(){{var d = {parent}.contentData; for (var i = 0; i < d.length; ++i)"
                f" if (String(d[i]).indexOf('{typename}') === 0) {{ d[i].open(); return true }} return false}})()")
    if not ok:
        raise LookupError(f"no {typename} in {parent}")
    r.settle_or(0.8)


def m_open_named(r, arg):
    """open_named OBJECTNAME [js to run on it first, with `it`]"""
    name, _, js = arg.partition(" ")
    ok = r.s.ev(f"(function(){{var it={FIND_ANY}('{name}');if(!it)return false;{js};it.open();return true}})()")
    if not ok:
        raise LookupError(f"no {name}")
    r.settle_or(0.8)


def m_tagedit(r, arg):
    n = int(arg or 4)
    paths = [r.s.ev(f"root.uiBridge.queuePathAt({i})") for i in range(n)]
    r.s.ev(f"root.tagEditorApi.openForPaths({json.dumps(paths)}) && (tagEditorDialog.open(), true)")
    r.s.a.wait_for("!root.tagEditorApi.loading", timeout=5, interval=0.05)
    r.settle_or(1.0)
    time.sleep(0.3)


def _a11y_row(r, stop):
    """the accessibility interface's account of the focused item (probe "a11y"), one JSON line per focus stop
    in a11y_<variant>.jsonl (pack.py makes FOCUS-A11Y.md of them)."""
    res = r.s.cmd("a11y")
    try:
        row = json.loads(res)
    except (TypeError, ValueError):
        raise RuntimeError(f"a11y: the probe gave no transcript ({str(res)[:80]})")
    row["stop"] = stop
    p = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), f"a11y_{r.s.suffix}.jsonl")
    with open(p, "w" if stop in ("Launch", "Tab 1") and not getattr(r.s, "a11y_started", False) else "a") as f:
        f.write(json.dumps(row, ensure_ascii=False) + "\n")
    r.s.a11y_started = True


def m_tab(r, arg):
    r.step_key("Tab")
    time.sleep(0.12)
    o = r.s.focus_owner()
    _a11y_row(r, f"Tab {arg}")
    r.line_desc = f"Focus walk from the window start, Tab {arg}: {o}"
    p = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), f"focus_owners_{r.s.suffix}.txt")
    fresh = arg == "1" and not getattr(r.s, "launch_logged", False)  # a new file, unless the launch line is in it
    if arg == "1":
        r.s.tab_seen = []
    seen = getattr(r.s, "tab_seen", [])
    # the stop's identity: its name and place (a list's rows share a name), so a wrap to Tab 1's stop is seen
    key = o.split(" acc=")[0]
    note = f"   (wraps: the stop of Tab {seen.index(key) + 1} again)" if key in seen else ""
    seen.append(key)
    with open(p, "w" if fresh else "a") as f:
        f.write(f"Tab {arg}: {o}{note}\n")


def _a11y_path(r, kind):
    return os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), f"{kind}_{r.s.suffix}.jsonl")


def m_dialogwalk(r, arg):
    """dialogwalk LABEL N: the open dialog window's focus stops for FOCUS-A11Y.md: what the accessibility
    interface says of its focus item as opened, then after each of N Tab presses (keys and reads go to that window),
    into a11y-dialogs_<variant>.jsonl; Escape closes it after (and the dialog must then be gone)."""
    label, _, n = arg.rpartition(" ")
    dw = r.s.dialog_window(timeout=2.0)
    if not dw:
        raise LookupError(f"dialogwalk {label}: no dialog window open")
    cls = dw["cls"]
    first = not getattr(r.s, "dlg_a11y_started", False)
    with open(_a11y_path(r, "a11y-dialogs"), "w" if first else "a") as f:
        for k in range(int(n) + 1):
            if k:
                r.s.cmd(f"key@{cls} {KEY_TAB} 0")
                time.sleep(0.15)
            res = r.s.cmd(f"a11y@{cls}")
            try:
                row = json.loads(res)
            except (TypeError, ValueError):
                raise RuntimeError(f"dialogwalk {label}: the probe gave no transcript ({str(res)[:80]})")
            row.update(dialog=label, stop="Opened" if not k else f"Tab {k}", window_title=dw["title"])
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
    r.s.dlg_a11y_started = True
    r.s.cmd(f"key@{cls} {KEY_ESC} 0")
    time.sleep(0.6)
    if any(w["cls"] == cls and w["visible"] for w in r.s.windows()):
        raise RuntimeError(f"dialogwalk {label}: Escape left the dialog window open")


def m_checkann(r, arg):
    """checkann LABEL WINDOW OBJECT [BACK]: the accessibility events (probe `a11yevents`: state changes such as
    checked, focus, name and value changes) the app sends when OBJECT, given keyboard focus, is toggled with Space,
    and the item's states before and after, into a11y-events_<variant>.jsonl; WINDOW `main` or `dialog` (the open
    dialog window). A second Space puts the state back (recorded too), on BACK when given (a radio group's other
    button)."""
    label, win, obj, *back = arg.split(" ")
    target = ""
    if win == "dialog":
        dw = r.s.dialog_window(timeout=2.0)
        if not dw:
            raise LookupError(f"checkann {label}: no dialog window open")
        target = "@" + dw["cls"]
    find = ("(function(n){var r=contentItem;while(r.parent)r=r.parent;var s=[r];while(s.length){var it=s.shift();"
            "if(it.objectName===n)return it;var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}return null})"
            if target else FIND)
    def focus(o):
        ok = r.s.cmd(f"eval{target} (function(){{var it={find}('{o}');if(!it)return false;"
                     "it.forceActiveFocus(Qt.TabFocusReason);return it.activeFocus})()")
        if "true" not in str(ok):
            raise LookupError(f"checkann {label}: {o} cannot take keyboard focus ({ok})")
        time.sleep(0.2)
    focus(obj)
    with open(_a11y_path(r, "a11y-events"), "a" if getattr(r.s, "ann_started", False) else "w") as f:
        for press in ("toggle", "back"):
            if press == "back" and back:
                focus(back[0])
            before = json.loads(r.s.cmd(f"a11y{target}"))
            r.s.cmd("a11yevents start")
            r.s.cmd(f"key{target} {KEY_SPACE} 0")
            time.sleep(0.5)
            evs = json.loads(r.s.cmd("a11yevents stop"))
            after = json.loads(r.s.cmd(f"a11y{target}"))
            f.write(json.dumps(dict(label=label, press=press, object=back[0] if press == "back" and back else obj,
                                    before=before, after=after, events=evs),
                               ensure_ascii=False) + "\n")
            if press == "toggle" and before.get("states") == after.get("states"):
                raise RuntimeError(f"checkann {label}: Space did not change {obj}'s states ({before.get('states')})")
    r.s.ann_started = True


def _crop_item(r, im, name, max_h=None, pick="first"):
    """The item's rect, cut from the window grab `im` at 1:1 (device pixels), or LookupError."""
    if pick == "tallest":
        rs = r.s.visible_rects(name)
        rect = max((v[1:] for v in rs), key=lambda v: v[3]) if rs else None
    else:
        rect = r.s.rect(name)
    if not rect:
        raise LookupError(f"crop: no item {name}")
    sc = r.s.scale
    x, y, w, h = [int(round(v * sc)) for v in rect]
    if max_h:
        h = min(h, int(max_h * sc))
    return im.crop((x, y, x + w, y + h)), rect


def _grab_window(r, tag, settle=True):
    raw = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "closeup-raw")
    os.makedirs(raw, exist_ok=True)
    if settle:  # (a caller that reads live values settles first itself, so nothing waits between its read and the grab)
        r.s.settle_opacity()
    p = os.path.join(raw, f"{tag}_{r.s.suffix}.png")
    res = r.s.raw_grab(p)
    if not str(res).startswith("ok"):
        raise RuntimeError(f"grab {tag}: {res}")
    from PIL import Image
    return Image.open(p).convert("RGB")


def _save_shot(r, img, area, state, desc):
    name = f"{area}_{state}_{r.s.suffix}"
    evidence = r.s.composite_evidence(name + ".png")  # (before the save: a refused image replaces no earlier one)
    img.save(os.path.join(os.environ["SHOT_OUT"], name + ".png"))
    r.s.index.append(dict(file=name + ".png", area=area, state=state, theme=r.s.tag, scale=r.s.scale,
                          size=f"{r.s.size[0]}x{r.s.size[1]}", image=f"{img.width}x{img.height}",
                          renderer=r.s.renderer, window="main", desc=desc, **evidence))


QUEUE_END_JS = ("(function(){var v=" + FIND + "('playlistView');if(!v)return '';var q=v.mapToItem(null,0,0);var bot=q.y+v.height;"
                "var c=v.contentItem.children,best=null;for(var i=0;i<c.length;i++){var it=c[i];if(it.objectName!=='queueRow'||!it.visible)continue;"
                "var p=it.mapToItem(null,0,0);if(p.y<bot&&p.y+it.height>bot-0.5){if(!best||p.y>best[0])best=[p.y,it.height]}}"
                "return JSON.stringify({bottom:bot,row:best})})()")


def m_onetoone(r, arg):
    """One theme's readable 1:1 sheet (the overview's tiles are too small to read): the transport bar and the
    status bar in halves (at most 800 px), the now-playing block, a group of library rows and of queue rows;
    the queue/pane seam (whether the queue ends on a whole row is measured and said) and the windowed album art
    viewer's info column. All actual pixels, laid out under the reviewers' 1568 px."""
    from PIL import Image, ImageDraw, ImageFont
    r.s.away()
    im = _grab_window(r, "onetoone")
    halves = []
    for name, label in (("transportBar", "Transport bar"), ("statusBar", "Status bar")):
        c, _ = _crop_item(r, im, name)
        hw = (c.width + 1) // 2
        halves.append((f"{label}, left half", c.crop((0, 0, hw, c.height))))
        halves.append((f"{label}, right half", c.crop((hw, 0, c.width, c.height))))
    np_, _ = _crop_item(r, im, "nowPlayingPanel")
    lib, _ = _crop_item(r, im, "libraryPane", max_h=340)
    que, _ = _crop_item(r, im, "mainQueuePane", max_h=287)  # its header and ten whole rows
    # the queue's foot against the plot pane: 70 px above the pane's top to 24 px into it
    qr, pr = r.s.rect("mainQueuePane"), r.s.rect("spectrogramPane")
    if not qr or not pr:
        raise LookupError("onetoone: no mainQueuePane or spectrogramPane")
    sc = r.s.scale
    seam = im.crop((int(qr[0] * sc), int((pr[1] - 70) * sc), int((qr[0] + qr[2]) * sc), int((pr[1] + 24) * sc)))
    qe = r.s.ev(QUEUE_END_JS)
    qe = json.loads(qe) if qe else None
    if not qe:
        raise LookupError("onetoone: no playlistView")
    if qe["row"] and qe["row"][0] + qe["row"][1] > qe["bottom"] + 0.5:
        shown = qe["bottom"] - qe["row"][0]
        seam_note = f"the queue ends on a cut row: {shown:.0f} of its {qe['row'][1]:.0f} px shown"
    else:
        seam_note = "the queue ends on a whole row"
    # the windowed album art viewer with its info column, opened and closed again here
    was = r.s.ev("JSON.stringify([root.uiBridge.viewerFullscreenMode, viewerController.albumArtInfoVisible])")
    mode0, info0 = json.loads(was)
    r.s.ev("(function(){root.uiBridge.setViewerFullscreenMode(0);viewerController.openAlbumArtViewer();return true})()")
    time.sleep(0.6)
    # the app's own toggle: it asks for the file's details before showing the column
    r.s.ev("(function(){if(!viewerController.albumArtInfoVisible)viewerController.toggleAlbumArtInfoVisible(null);return true})()")
    end = time.time() + 4.0  # the file's details (dimensions, size, format) arrive after the viewer opens
    while len(r.s.visible_rects("albumArtInfoRow")) < 4:
        if time.time() > end:
            raise RuntimeError(f"onetoone: the album art info shows {len(r.s.visible_rects('albumArtInfoRow'))} "
                               "of its 4 rows after 4 s")
        time.sleep(0.1)
    r.s.settle(timeout=1.5)
    vim = _grab_window(r, "onetoone-artinfo")
    rs = r.s.visible_rects("albumArtInfoColumn")
    if not rs:
        raise LookupError("onetoone: no visible albumArtInfoColumn in the windowed album art viewer")
    _, ix, iy, iw, ih = rs[0]
    info = vim.crop((int(ix * sc), int(iy * sc), int((ix + iw) * sc), int((iy + min(ih, 560)) * sc)))
    r.s.ev(f"(function(){{if(viewerController.albumArtInfoVisible!=={'true' if info0 else 'false'})"
           f"viewerController.toggleAlbumArtInfoVisible(null);"
           f"viewerController.closeAlbumArtViewer();root.uiBridge.setViewerFullscreenMode({int(mode0)});return true}})()")
    time.sleep(0.4)
    r.s.settle(timeout=1.5)
    f = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 15)
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 20)
    LH, GAP, P = 22, 12, 14
    # within 1.15 MP as drawn (a reviewer's image input downscales anything larger, and 1:1 would not be
    # 1:1): the transport in thirds; the info column to its content; the queue rows and the seam to the sheet's
    # width (the queue's right columns are on the full shot); the status bar is on the details sheet.
    W = 1060
    b_x = P + np_.width + 24
    b_w = W - P - b_x
    tb, _ = _crop_item(r, im, "transportBar")
    third = (tb.width + 2) // 3
    thirds = [(f"Transport bar, {k}", tb.crop((n * third, 0, min(tb.width, (n + 1) * third), tb.height)))
              for n, k in enumerate(("left third", "middle third", "right third"))]
    thirds = [(lb, c if c.width <= b_w else c.crop((0, 0, b_w, c.height))) for lb, c in thirds]
    import numpy as np
    ia = np.asarray(info).astype(int)
    bgc = np.median(ia.reshape(-1, 3), axis=0)
    rows_ink = np.nonzero((np.abs(ia - bgc).sum(axis=2) > 60).any(axis=1))[0]
    info_c = info.crop((0, 0, info.width, min(info.height, int(rows_ink.max()) + 16 if len(rows_ink) else info.height)))
    qh = min(que.height, 230)
    que_c = que.crop((0, 0, min(que.width, b_w), qh))
    seam_c = seam.crop((0, 0, min(seam.width, W - 2 * P), seam.height))
    col_a = [("Now playing (the whole block)", np_), ("Library: its header and first rows", lib.crop((0, 0, lib.width, min(lib.height, 250))))]
    col_b = thirds + [("Album art viewer (windowed): its info column", info_c),
                      ("Queue: its header and first rows (left part)", que_c)]
    top_h = max(sum(LH + c.height + GAP for _, c in col_a), sum(LH + c.height + GAP for _, c in col_b))
    H = 44 + top_h + LH + seam_c.height + GAP
    if (W + 28) * (H + 64) > 1.15e6:  # as its sheet will be (a title above, a margin around)
        raise RuntimeError(f"onetoone: {W}x{H} is over 1.15 MP on its sheet")
    S = Image.new("RGB", (W, H), (78, 82, 88))
    d = ImageDraw.Draw(S)
    d.text((P, 10), f"{r.s.tag.capitalize()}, 1:1 (actual pixels, nothing scaled), while playing", fill=(255, 255, 255), font=fb)
    for x0, items in ((P, col_a), (b_x, col_b)):
        y = 44
        for label, c in items:
            d.text((x0, y), label, fill=(255, 220, 120), font=f)
            S.paste(c, (x0, y + LH))
            y += LH + c.height + GAP
    y = 44 + top_h
    d.text((P, y), f"The queue's foot over the plot pane: {seam_note}", fill=(255, 220, 120), font=f)
    S.paste(seam_c, (P, y + LH))
    _save_shot(r, S, "onetoone", "readable", "1:1 crops while playing: the transport in thirds, now playing, library "
               f"rows, queue rows, the album art viewer's info column, the queue/pane seam ({seam_note})")


def m_edge4x(r, arg):
    """The plot's right edge with the level meter on at 4x nearest-neighbour: the meter's top 120 px with 56 px
    of plot left of it and what is right of it (the pane's edge). arg: main (the pane) or viewer (the windowed
    spectrogram viewer, over the main window: its meter is the tallest)."""
    from PIL import Image, ImageDraw, ImageFont
    im = _grab_window(r, f"edge-{arg}")
    rs = r.s.visible_rects("levelMeter")
    if not rs:
        raise LookupError("edge4x: no visible levelMeter")
    _, x, y, w, h = max(rs, key=lambda v: v[4])
    sc, Z, CH = r.s.scale, 4, 120
    # 120 px of the meter around its level: from 50 px above the first row (from the top) where the meter's
    # column leaves its trough (the peak tick or the fill), clamped to the meter; its top when nothing shows.
    cx = int((x + w / 2) * sc)
    trough = im.getpixel((cx, int((y + 4) * sc)))
    top = y
    for yy in range(int((y + 8) * sc), int((y + h) * sc)):
        if sum(abs(a - b) for a, b in zip(im.getpixel((cx, yy)), trough)) > 120:
            top = max(y, min(y + h - CH, yy / sc - 50))
            break
    x0, x1 = int((x - 56) * sc), int(min(r.s.size[0], x + w + 10) * sc)
    c = im.crop((x0, int(top * sc), x1, int((top + min(h, CH)) * sc)))
    c = c.resize((c.width * Z, c.height * Z), Image.NEAREST)
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 16)
    f = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 14)
    label = {"main": "Main window: the pane's right edge", "viewer": "Spectrogram viewer (windowed): its right edge"}[arg]
    note = f"meter on, 4x: y {int(top - y)}-{int(top - y + min(h, CH))} of its {int(h)} px, 56 px of plot left of it"
    probe = ImageDraw.Draw(Image.new("RGB", (8, 8)))
    S = Image.new("RGB", (int(max(c.width, probe.textlength(label, font=fb), probe.textlength(note, font=f))) + 28,
                          c.height + 62), (78, 82, 88))
    d = ImageDraw.Draw(S)
    d.text((14, 6), label, fill=(255, 220, 120), font=fb)
    d.text((14, 28), note, fill=(230, 230, 230), font=f)
    S.paste(c, (14, 52))
    _save_shot(r, S, "edge", f"meter-{arg}", f"{label} with the level meter on, at 4x ({note})")


def m_crop11(r, arg):
    """crop11 AREA STATE ITEM [MAX_H]: the item at 1:1 from the current window, as its own shot (e.g. the queue
    at 24 pt, per window size)."""
    area, state, item, *mh = arg.split()
    im = _grab_window(r, f"{area}-{state}")
    c, _ = _crop_item(r, im, item, max_h=float(mh[0]) if mh else None)
    _save_shot(r, c, area, state, f"{item} at 1:1 (actual pixels), window {r.s.size[0]}x{r.s.size[1]}")


def m_inwindow(r, arg):
    """The windowed viewer just opened is a popup inside the main window (a modal Popup over its Overlay),
    not a window of its own: an opened popup covers the window and no other top-level window is visible, or
    the state fails (its strip's caption says "inside the main window")."""
    # shown: listed (visible), opened, fully opaque, covering the window; within 2 s (an unrecorded open lands a
    # moment later than the step that asked for it)
    end = time.time() + 2.0
    while True:
        pops = r.s.popups()
        big = [p for p in pops if p["opened"] and p["opacity"] > 0.99
               and p["rect"][2] >= 0.9 * r.s.size[0] and p["rect"][3] >= 0.9 * r.s.size[1]]
        if big or time.time() > end:
            break
        time.sleep(0.05)
    with open(os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), f"popups_{r.s.suffix}.txt"), "a") as f:
        f.write(f"{r.line_state if hasattr(r, 'line_state') else ''}: " + ", ".join(
            f"{p['name'] or p['cls']} opened={p['opened']} opacity={p['opacity']:.2f}" for p in pops) + "\n")
    others = [w for w in r.s.windows()[1:] if w.get("visible")]
    if not big or others:
        seq = []  # what it does next: still opening, closing, or stuck
        for _ in range(15):
            time.sleep(0.1)
            seq.append(",".join(f"{p['opacity']:.2f}{'o' if p['opened'] else ''}" for p in r.s.popups()) or "-")
        state = r.s.ev("JSON.stringify([viewerController.spectrogramViewerOpen, viewerController.albumArtViewerOpen])")
        raise RuntimeError(f"inwindow: then every 100 ms {seq}; viewers open (spectrogram, album art) {state}; "
                           f"no shown popup covering the window ({[(p['cls'], p['name'], p['opened'], p['rect']) for p in pops]}),"
                           f" or other visible windows {[w.get('title') for w in others]}")


def m_rollprobe(r, arg):
    """rollprobe LABEL: the playback position and how many of the plot pane's columns, from its left
    edge, are black (no plot drawn: every channel under 21 on average over the pane's height), into
    rolling_<variant>.txt and the current line's caption."""
    import numpy as np
    im = _grab_window(r, f"rollprobe-{arg}")
    rect = r.s.rect("spectrogramPane")
    if not rect:
        raise LookupError("rollprobe: no spectrogramPane")
    sc = r.s.scale
    x, y, w, h = [int(round(v * sc)) for v in rect]
    a = np.asarray(im.crop((x, y + 4, x + w, y + h - 4))).astype(int)
    # an unfilled column is the plot's flat floor, whatever its colour (#14062e, no longer black): at least
    # 80 % of its pixels within a few levels of the column's median colour (lane heads and seams aside); a drawn
    # column is the spectrogram's texture
    med = np.median(a, axis=0)
    flat = (np.abs(a - med[None, :, :]).sum(axis=2) <= 12).mean(axis=0) >= 0.8
    lit = ~flat
    black = int(np.argmax(lit)) if lit.any() else w
    pos = r.s.ev("playbackController.playbackClock ? playbackController.playbackClock.positionSeconds : -1")
    try:
        pos = float(pos)
    except (TypeError, ValueError):
        raise RuntimeError(f"rollprobe: no playback position ({pos})")
    mode = r.s.ev("JSON.stringify([viewerController.visualizationMode, root.uiBridge.spectrogramDisplayMode])")
    line = f"{arg}: position {pos:.2f} s, mode/display {mode}, the pane's first {black} of {w} px unfilled (flat floor)"
    with open(os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), f"rolling_{r.s.suffix}.txt"), "a") as f:
        f.write(line + "\n")
    r.line_desc = (r.line_desc or "") + f" [{line}]"


_FLOOR_LABELS = {"pre-start": "before the track's start (rolling, 0.4 s in: centered mode never shows it)",
                 "post-end": "after the track's end (centered, a track shorter than the view)",
                 "silence": "digital silence inside the track (centered, 6 s: beside the playhead)",
                 "unfilled": "never played: the pane's plain floor",
                 "ends-early-centered": "after the cut of a file that ends early (centered, one lane)",
                 "ends-early-lanes": "after the cut of a file that ends early (centered, per channel)",
                 "ends-early-rolling": "after the cut of a file that ends early (rolling)"}


def m_floorprobe(r, arg):
    """floorprobe LABEL:REGION: the colour of one empty region of the spectrogram pane, sampled from
    a window grab, into floors.jsonl (pack.py's plot-floor table) and the facts. REGION picks the run of flat
    columns (80 % of a column within a few levels of its median: the floor, a lane seam or a pure tone's line
    aside) of one colour: `left` (touching the pane's left edge), `right` (its right edge), `centre` (holding
    the pane's middle column) or `whole` (the longest). `floorprobe save` / `floorprobe restore` keep and put
    back the display and view modes around the probes."""
    import numpy as np
    if arg in ("save", "restore"):
        if arg == "save":
            r.floor_saved = r.s.ev("JSON.stringify([root.uiBridge.spectrogramDisplayMode, root.uiBridge.spectrogramViewMode,"
                                   " viewerController.visualizationMode, root.uiBridge.stretchShortTracks])")
            if not r.floor_saved:
                raise RuntimeError("floorprobe save: no modes")
        else:
            dm, vm, vis, stretch = json.loads(getattr(r, "floor_saved"))
            r.s.ev(f"root.uiBridge.setStretchShortTracks({'true' if stretch else 'false'})")
            r.s.ev(f"root.uiBridge.setSpectrogramDisplayMode({dm})")
            r.s.ev(f"root.uiBridge.setSpectrogramViewMode({vm})")
            r.s.ev(f"viewerController.setVisualizationMode({vis})")
        return
    label, _, region = arg.partition(":")
    im = _grab_window(r, f"floorprobe-{label}")
    rect = r.s.rect("spectrogramPane")
    if not rect:
        raise LookupError("floorprobe: no spectrogramPane")
    sc = r.s.scale
    x, y, w, h = [int(round(v * sc)) for v in rect]
    a = np.asarray(im.crop((x, y + 4, x + w, y + h - 4))).astype(int)
    med = np.median(a, axis=0)
    flat = (np.abs(a - med[None, :, :]).sum(axis=2) <= 12).mean(axis=0) >= 0.8
    runs, k = [], 0
    while k < w:
        if not flat[k]:
            k += 1
            continue
        j = k
        while j + 1 < w and flat[j + 1] and np.abs(med[j + 1] - med[k]).sum() <= 12:
            j += 1
        runs.append((k, j))
        k = j + 1
    pick = None
    if region == "left":
        pick = next((u for u in runs if u[0] <= 3), None)
    elif region == "right":
        pick = next((u for u in runs if u[1] >= w - 4), None)
    elif region == "playhead":  # the wide run just left of the playhead (the column with the brightest median)
        ph = int(np.argmax(med @ np.array([0.2126, 0.7152, 0.0722])))
        wide = [u for u in runs if u[1] - u[0] >= 8 and u[1] < ph]
        pick = max(wide, key=lambda u: u[1]) if wide and ph - max(u[1] for u in wide) <= 4 else None
    elif region == "marker":  # from where the file is cut off (the spectrogram item's markerX) to the pane's end
        mx = r.s.ev("(function(){var s=[" + FIND + "('spectrogramPane')];while(s.length){var it=s.shift();if(!it||!it.visible)continue;"
                    "if(it.markerX!==undefined&&it.markerSeconds>=0){if(isNaN(it.markerX))return 'nan '+it.markerSeconds;"
                    "return String(it.mapToItem(null,it.markerX,0).x)+' '+it.markerSeconds}var c=it.children;"
                    "for(var i=0;i<c.length;i++)s.push(c[i]);}return 'none'})()") or "none"
        if mx == "none" or mx.startswith("nan"):
            pick, region = None, f"marker ({'no cut marker' if mx == 'none' else 'the cut at ' + mx.split()[1] + ' s is not in view'})"
        else:
            k0 = int(round((float(mx.split()[0]) - rect[0]) * sc)) + 2
            pick = (k0, w - 1) if k0 < w - 8 else None
            region = (f"marker (the cut at {float(mx.split()[1]):.2f} s, x {float(mx.split()[0]):.0f}"
                      + ("" if pick else f": at or past the pane's right edge, x {rect[0] + rect[2]:.0f}") + ")")
    elif region == "whole":
        pick = max(runs, key=lambda u: u[1] - u[0]) if runs else None
    else:
        raise ValueError(f"floorprobe: unknown region {region!r}")
    floor = r.s.ev("String(root.uiPalette.plotFloor)") or ""
    pos = r.s.ev("playbackController.playbackClock ? playbackController.playbackClock.positionSeconds : -1")
    modes = r.s.ev("JSON.stringify([viewerController.visualizationMode, root.uiBridge.spectrogramDisplayMode,"
                   " root.uiBridge.spectrogramViewMode])")
    rec = dict(label=label, what=_FLOOR_LABELS.get(label, label), theme=r.s.tag, suffix=r.s.suffix, region=region,
               floor_token=floor, position=pos, modes=modes, pane_w=w)
    if pick:
        x0, x1 = pick
        # the run's middle rows (lane heads and the frequency labels' rows aside): its most frequent colour and
        # the spread (max channel difference from it) over those pixels
        band = a[int(a.shape[0] * 0.2):int(a.shape[0] * 0.8), x0:x1 + 1].reshape(-1, 3)
        vals, counts = unique_colours(band)
        top = vals[counts.argmax()]
        rec.update(colour=_hexc(top), share=round(float(counts.max() / len(band)), 3), x0=int(x0), x1=int(x1),
                   spread=int(np.abs(band - top).max()),
                   equals_floor=floor.lower() == _hexc(top), lum=round(_lum(top), 4))
        if floor.startswith("#") and len(floor) == 7:
            fl = tuple(int(floor[i:i + 2], 16) for i in (1, 3, 5))
            rec["vs_floor"] = round(_contrast(top, fl), 3)
            rec["max_channel_diff"] = int(max(abs(int(top[i]) - fl[i]) for i in range(3)))
        rel = " (the same)" if rec["equals_floor"] else f", {rec.get('vs_floor', 0):.2f}:1 from it"
        line = (f"{label}: {rec['colour']} over px {x0}-{x1} of {w} ({rec['share'] * 100:.0f} % of its pixels exactly), "
                f"plotFloor {floor}{rel}")
    else:
        rec.update(colour=None, runs=[list(map(int, u)) for u in runs][:8])
        line = f"{label}: no flat {region} region found (flat runs {rec['runs']})"
    with open(os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "floors.jsonl"), "a") as f:
        f.write(json.dumps(rec) + "\n")
    print(f"  floorprobe {r.s.suffix} {line}")
    # the pane as sampled, the region outlined (reference: the area "plotfloor")
    from PIL import ImageDraw
    pane = im.crop((x, y, x + w, y + h))
    if pick:
        ImageDraw.Draw(pane).rectangle((pick[0], int(h * 0.2), pick[1], int(h * 0.8)), outline=(255, 64, 160), width=2)
    _save_shot(r, pane, "plotfloor", label, f"The plot pane sampled for its empty region: {line} (outlined in pink; "
                                            f"position {pos} s, modes {modes})")


def m_rowdetail(r, arg):
    """rowdetail glyph|ring: a library row at 1:1 across the pane (scrollbar lane included) with a 4x zoom
    below. `glyph`: the first shown no-art thumbnail (libraryRowCoverPlaceholder), its glyph's colour and contrast
    against the row; `ring`: the current row (keyboard focus), where the focus ring's accent pixels end against
    the scrollbar's lane (uiPalette.focusRing within 60 levels). Saved as details/library-noart-glyph or details/library-ring-lane, with a fact."""
    from PIL import Image, ImageDraw, ImageFont
    import numpy as np
    im = _grab_window(r, f"rowdetail-{arg}")
    sc = r.s.scale
    pane = r.s.rect("libraryPane")
    if not pane:
        raise LookupError("rowdetail: no libraryPane")
    if arg == "glyph":
        rs = r.s.visible_rects("libraryRowCoverPlaceholder")
        if not rs:
            raise LookupError("rowdetail glyph: no libraryRowCoverPlaceholder shown")
        _, gx, gy, gw, gh = rs[0]
        row = (pane[0], gy - 8, pane[2], gh + 16)
        zoom = (gx - 6, gy - 6, gw + 12, gh + 12)
    elif arg == "ring":
        # the row background that draws the focus ring (RowBackground: focused, with a scrollbarLane), shown
        v = r.s.ev(f"(function(){{var s=[{FIND}('libraryView')];while(s.length){{var it=s.shift();if(!it||!it.visible)continue;"
                   "if(it.scrollbarLane!==undefined&&it.focused===true){var p=it.mapToItem(null,0,0);"
                   "if(p.y>=0&&p.y<root.height)return JSON.stringify([p.x,p.y,it.width,it.height])}"
                   "var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}return ''})()")
        if not v:
            raise LookupError("rowdetail ring: no focused library row (RowBackground focused) on screen")
        cx, cy, cw, ch = json.loads(v)
        row = (pane[0], cy - 6, pane[2], ch + 12)
        zoom = (pane[0] + pane[2] - 40, cy - 4, 40, ch + 8)
    else:
        raise ValueError(f"rowdetail: unknown kind {arg!r}")
    box = lambda b: tuple(int(round(v * sc)) for v in (b[0], b[1], b[0] + b[2], b[1] + b[3]))
    one = im.crop(box(row))
    z = im.crop(box(zoom))
    z = z.resize((z.width * 4, z.height * 4), Image.NEAREST)
    a = np.asarray(im).astype(int)
    if arg == "glyph":
        x0, y0, x1, y1 = box((gx, gy, gw, gh))
        bg = _tint(im, [v * sc for v in row], inset=1)
        px = a[y0:y1, x0:x1].reshape(-1, 3)
        fg = max((tuple(p) for p in px), key=lambda p: _contrast(p, bg))
        n_ink = int((np.abs(px - np.array(bg)).sum(axis=1) > 30).sum())
        tile_txt, _ = _mark_facts(im, (gx, gy, gw, gh), sc)  # (the tile as a no-art mark: fill, mark, edge)
        note = (f"tile {gw:.0f}x{gh:.0f} px at {gx:.0f},{gy:.0f}; its strongest pixel {_hexc(fg)} on the row's {_hexc(bg)}: "
                f"{_contrast(fg, bg):.2f}:1; {n_ink} of {len(px)} pixels off the row colour; {tile_txt}")
        key, state = "library-noart-glyph", "library-noart-glyph"
    else:
        acc = r.s.ev("String(root.uiPalette.focusRing)") or ""  # (RowBackground's ring: uiPalette.focusRing)
        rgb = np.array([int(acc[i:i + 2], 16) for i in (1, 3, 5)]) if acc.startswith("#") and len(acc) == 7 else None
        if rgb is None:
            raise RuntimeError(f"rowdetail ring: no focus ring colour ({acc})")
        x0, y0, x1, y1 = box(row)
        hit = np.nonzero((np.abs(a[y0:y1, x0:x1] - rgb).sum(axis=2) < 60).any(axis=0))[0]
        sb = r.s.visible_rects("libraryScrollBar")
        lane = f"the scrollbar from x {sb[0][1]:.0f}" if sb else "no scrollbar shown"
        ring_end = (x0 + int(hit.max())) / sc if len(hit) else None
        over = (ring_end - sb[0][1] + 1) if sb and ring_end is not None else None
        note = (f"focus ring {acc} pixels from x {(x0 + int(hit.min())) / sc:.0f} to {ring_end:.0f}; {lane}"
                + (f" (the ring reaches {over:.0f} px into it)" if over and over > 0 else
                   " (the ring stops before it)" if over is not None else "")) if len(hit) else f"no focus ring {acc} pixels in the row; {lane}"
        if not sb:
            raise RuntimeError("rowdetail ring: the library's scrollbar is not shown")
        key, state = "library-ring-lane", "library-ring-lane"
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 14)
    fr = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 12)
    W = max(one.width, z.width) + 20
    S = Image.new("RGB", (W, 24 + one.height + 22 + z.height + 30), (78, 82, 88))
    d = ImageDraw.Draw(S)
    d.text((10, 4), f"1:1, the row across the pane ({r.s.tag})", fill=(255, 220, 120), font=fb)
    S.paste(one, (10, 24))
    d.text((10, 28 + one.height), "4x", fill=(255, 220, 120), font=fb)
    S.paste(z, (10, 46 + one.height))
    d.text((10, 50 + one.height + z.height), note[:110], fill=(230, 230, 230), font=fr)
    what = "The library's no-art thumbnail glyph" if arg == "glyph" else "The focused library row's ring beside the scrollbar"
    _save_shot(r, S, "details", state, f"{what}, 1:1 and 4x: {note}")
    _fact(r, key, f"{what} ({r.s.tag}): {note}")


def _hexs(c):
    return "#%02x%02x%02x" % tuple(int(v) for v in c[:3])


def _colours(a, n=4, min_share=0.004):
    """The n most frequent exact colours of a pixel array (H, W, 3), with their shares."""
    import numpy as np
    v, c = unique_colours(a)
    o = c.argsort()[::-1][:n]
    return [(tuple(int(x) for x in v[i]), float(c[i] / c.sum())) for i in o if c[i] / c.sum() >= min_share]


def _mark_facts(im, box, sc):
    """A no-art cover or tile at box (logical x, y, w, h): its fill (most frequent colour), its other colours (the
    mark), its 1 px edge (the outermost pixel ring's most frequent colour) and what lies just outside, with ratios."""
    import numpy as np
    a = np.asarray(im).astype(int)
    x0, y0, x1, y1 = [int(round(v * sc)) for v in (box[0], box[1], box[0] + box[2], box[1] + box[3])]
    inner = a[y0 + 2:y1 - 2, x0 + 2:x1 - 2]
    cols = _colours(inner, n=5)
    fill = cols[0][0]
    ring = np.concatenate([a[y0, x0:x1], a[y1 - 1, x0:x1], a[y0:y1, x0], a[y0:y1, x1 - 1]])
    edge = _colours(ring, n=1)[0][0]
    out = []
    if y0 >= 2:
        out.append(a[y0 - 2, x0:x1])
    if x0 >= 2:
        out.append(a[y0:y1, x0 - 2])
    if x1 + 2 < a.shape[1]:
        out.append(a[y0:y1, x1 + 1])
    around = _colours(np.concatenate(out), n=1)[0][0] if out else None
    # the mark: every pixel clearly off the fill (its antialiased colours spread thin, so not by frequency alone):
    # its share, its bounding box's centre against the cover's, its most frequent colours and its strongest pixel
    off = np.abs(inner - np.array(fill)).sum(axis=2) > 40
    mark = []
    mark_txt = "none"
    if off.any():
        cm = _colours(inner[off], n=5, min_share=0.05)  # (the mark has four traces since the icon rounds)
        mark = [(c, sh * off.mean()) for c, sh in cm]
        strong = max((tuple(int(v) for v in p_) for p_ in inner[off][::7]), key=lambda c: _contrast(c, fill))
        yy, xx = np.nonzero(off)
        cxm, cym = (xx.min() + xx.max()) / 2 / (inner.shape[1] - 1), (yy.min() + yy.max()) / 2 / (inner.shape[0] - 1)
        mark_txt = (f"{off.mean() * 100:.1f} % of the cover, its box {xx.max() - xx.min() + 1}x{yy.max() - yy.min() + 1} px "
                    f"centred at {cxm:.2f},{cym:.2f} of the cover; colours " + ", ".join(
                        f"{_hexs(c)} ({_contrast(c, fill):.2f}:1)" for c, _ in mark) +
                    f"; strongest {_hexs(strong)} ({_contrast(strong, fill):.2f}:1 to the fill)")
    txt = (f"fill {_hexs(fill)}; mark {mark_txt}" +
           f"; edge (outer 1 px) {_hexs(edge)}" + (" (the fill)" if edge == fill else "")
           + (f"; around it {_hexs(around)}: edge {_contrast(edge, around):.2f}:1, fill {_contrast(fill, around):.2f}:1"
              if around else ""))
    return txt, dict(fill=_hexs(fill), mark=[_hexs(c) for c, _ in mark], edge=_hexs(edge),
                     around=_hexs(around) if around else None)


def m_markprobe(r, arg):
    """markprobe ITEM: the no-art cover (ITEM, e.g. nowPlayingCover) sampled: fill, mark, edge, surround."""
    im = _grab_window(r, f"markprobe-{arg}")
    rs = r.s.visible_rects(arg)
    if not rs:
        raise LookupError(f"markprobe: no {arg} shown")
    rect = max((v[1:] for v in rs), key=lambda v: v[2] * v[3])
    txt, data = _mark_facts(im, rect, r.s.scale)
    _fact(r, f"noart-mark-{arg}", f"No-art cover ({arg}, {r.s.tag}, {rect[2]:.0f}x{rect[3]:.0f} px): {txt}", **data)
    r.line_desc = (r.line_desc or "") + f" [{txt}]"


def m_restprobe(r, arg):  # arg: the view's label ("mixed" or "lanes")
    """restprobe: the never-played pane's lines (rows whose most frequent colour is not the floor's, over at
    least half the pane's width: rest lines, the lane seam), and the lane heads' plates, sampled."""
    import numpy as np
    im = _grab_window(r, "restprobe")
    rect = r.s.rect("spectrogramPane")
    if not rect:
        raise LookupError("restprobe: no spectrogramPane")
    sc = r.s.scale
    x, y, w, h = [int(round(v * sc)) for v in rect]
    a = np.asarray(im.crop((x, y, x + w, y + h))).astype(int)
    floor = _colours(a, n=1)[0][0]
    lines, k = [], 0
    while k < h:
        cs = _colours(a[k:k + 1], n=1, min_share=0)[0]
        if cs[0] != floor and cs[1] >= 0.5:
            j = k
            while j + 1 < h and _colours(a[j + 1:j + 2], n=1, min_share=0)[0][0] == cs[0]:
                j += 1
            lines.append((k, j - k + 1, cs[0], cs[1]))
            k = j + 1
        else:
            k += 1
    heads = []
    inside = lambda v: rect[0] <= v[1] + v[3] / 2 <= rect[0] + rect[2] and rect[1] <= v[2] + v[4] / 2 <= rect[1] + rect[3]
    for name in ("plotLaneLabel", "plotLaneHead"):
        for _, hx, hy, hw, hh in [v for v in r.s.rects(name) if inside(v) and v[3] > 0][:4]:
            c = _colours(np.asarray(im.crop(tuple(int(round(v * sc)) for v in (hx, hy, hx + hw, hy + hh)))).astype(int), n=2)
            heads.append(f"{name} at {hx:.0f},{hy:.0f} {hw:.0f}x{hh:.0f}: " + ", ".join(f"{_hexs(cc)} {sh * 100:.0f} %" for cc, sh in c))
        if heads:
            break
    txt = (f"floor {_hexs(floor)}; {len(lines)} line(s): " + ("; ".join(
        f"y {ly / sc:.0f} (+{lh} px) {_hexs(lc)} over {ls * 100:.0f} % of the width, {_contrast(lc, floor):.2f}:1 to the floor"
        for ly, lh, lc, ls in lines[:12]) or "none") + "; plates: " + ("; ".join(heads) or "none shown"))
    view = r.s.ev("root.uiBridge.spectrogramViewMode")
    label = f"{arg or 'view'} (spectrogramViewMode {view})"
    _fact(r, f"rest-lines-{arg or 'view'}", f"Never-played pane, {label} ({r.s.tag}): {txt}", floor=_hexs(floor),
          lines=[[ly, lh, _hexs(lc)] for ly, lh, lc, _ in lines])
    r.line_desc = (r.line_desc or "") + f" [{txt}]"


def _edge_facts(im, rect, sc):
    """A 1 px edge drawn on an item's rect (the checked segment's, the art info toggle's): the rect's outer pixel
    ring (its most frequent colour), the fill inside (2 px in) and what lies 1-3 px outside, with the edge's ratios."""
    import numpy as np
    a = np.asarray(im).astype(int)
    x0, y0, x1, y1 = [int(round(v * sc)) for v in (rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3])]
    ring = np.concatenate([a[y0, x0:x1], a[y1 - 1, x0:x1], a[y0:y1, x0], a[y0:y1, x1 - 1]])
    edge = _colours(ring, n=1)[0][0]
    fill = _colours(a[y0 + 2:y1 - 2, x0 + 2:x1 - 2], n=1)[0][0]
    out = np.zeros(a.shape[:2], bool)
    out[max(0, y0 - 3):y1 + 3, max(0, x0 - 3):x1 + 3] = True
    out[max(0, y0 - 1):y1 + 1, max(0, x0 - 1):x1 + 1] = False
    around = _colours(a[out], n=1)[0][0]
    return dict(edge=_hexs(edge), fill=_hexs(fill), around=_hexs(around), edge_vs_fill=round(_contrast(edge, fill), 2),
                edge_vs_around=round(_contrast(edge, around), 2))


def _switch_measure(im, sc, btns, sw, ring_hex, base_im=None, edge_rect=None, pane_rect=None):
    """The mode switch in one grab: the checked segment's fill and 1 px edge, the plate (gaps), the pane around,
    the focus ring's pixels; ratios."""
    import numpy as np
    a = np.asarray(im).astype(int)
    px = lambda b: [int(round(v * sc)) for v in (b[0], b[1], b[0] + b[2], b[1] + b[3])]
    ck = [b for b in btns if b[2]][0]
    x0, y0, x1, y1 = px(ck[1])
    fill = _colours(a[y0 + 2:y1 - 2, x0 + 2:x1 - 2], n=1)[0][0]
    X0, Y0, X1, Y1 = px(sw)
    keep = np.ones(a.shape[:2], bool)  # the edge: the checked segment's outer pixel ring, a focus ring's pixels aside
    if base_im is not None:
        keep[Y0:Y1, X0:X1] = np.abs(a[Y0:Y1, X0:X1] - np.asarray(base_im).astype(int)[Y0:Y1, X0:X1]).sum(axis=2) <= 60
    band = np.zeros(a.shape[:2], bool)
    band[y0, x0:x1] = band[y1 - 1, x0:x1] = True
    band[y0:y1, x0] = band[y0:y1, x1 - 1] = True
    edge = _colours(a[band & keep] if (band & keep).any() else a[band], n=1)[0][0]
    if edge_rect:  # (the checked segment's own edge item, plotToolButtonCheckedEdge: its outer pixel ring)
        ex0, ey0, ex1, ey1 = px(edge_rect)
        eb = np.zeros(a.shape[:2], bool)
        eb[ey0, ex0:ex1] = eb[ey1 - 1, ex0:ex1] = True
        eb[ey0:ey1, ex0] = eb[ey0:ey1, ex1 - 1] = True
        edge = _colours(a[eb & keep] if (eb & keep).any() else a[eb], n=1)[0][0]
    mask = np.zeros(a.shape[:2], bool)
    mask[Y0:Y1, X0:X1] = True
    for b in btns:
        bx0, by0, bx1, by1 = px(b[1])
        mask[by0:by1, bx0:bx1] = False
    # the focus ring or hover: the pixels on the switch's plate that differ from the unfocused, unhovered grab (not
    # around it: the plot behind keeps playing)
    win = (slice(Y0, Y1), slice(X0, X1))
    changed = np.zeros(a.shape[:2], bool)
    ring_col = None
    if base_im is not None:
        b = np.asarray(base_im).astype(int)
        changed[win] = np.abs(a[win] - b[win]).sum(axis=2) > 60
        if changed.any():
            ring_col = _colours(a[changed], n=1)[0][0]
    pm = mask & ~changed
    plate = _colours(a[pm], n=1)[0][0] if pm.any() else None
    out = np.zeros(a.shape[:2], bool)
    out[max(0, Y0 - 5):Y1 + 5, max(0, X0 - 5):X1 + 5] = True
    out[max(0, Y0 - 3):Y1 + 3, max(0, X0 - 3):X1 + 3] = False
    if pane_rect:  # (the plot behind: inside the pane only, the window's chrome above it aside)
        PX0, PY0, PX1, PY1 = px(pane_rect)
        inside = np.zeros(a.shape[:2], bool)
        inside[PY0:PY1, PX0:PX1] = True
        out &= inside
    pane = _colours(a[out], n=1)[0][0]
    base = plate or pane
    un = [b for b in btns if not b[2]][0]  # the unselected segment's fill (its hover tint, when hovered)
    ux0, uy0, ux1, uy1 = px(un[1])
    ufill = _colours(a[uy0 + 2:uy1 - 2, ux0 + 2:ux1 - 2], n=1)[0][0]

    def glyph(b, f_):  # the segment's glyph: its strongest pixel against the segment's fill, inside 3 px
        gx0, gy0, gx1, gy1 = px(b[1])
        pxs = a[gy0 + 3:gy1 - 3, gx0 + 3:gx1 - 3].reshape(-1, 3)
        g_ = max((tuple(int(v) for v in q) for q in pxs), key=lambda c: _contrast(c, f_))
        return _hexs(g_), round(_contrast(g_, f_), 2)
    gck, gun = glyph(ck, fill), glyph(un, ufill)
    return dict(fill=_hexs(fill), edge=_hexs(edge), plate=_hexs(plate) if plate else None, pane=_hexs(pane),
                unselected_fill=_hexs(ufill), unselected_vs_plate=round(_contrast(ufill, plate or pane), 2),
                glyph_checked=gck, glyph_unchecked=gun,
                selected_vs_unselected=round(_contrast(fill, ufill), 2),
                edge_vs_plate=round(_contrast(edge, base), 2), fill_vs_plate=round(_contrast(fill, base), 2),
                edge_vs_pane=round(_contrast(edge, pane), 2), edge_vs_fill=round(_contrast(edge, fill), 2),
                edge_item=bool(edge_rect), changed_px=int(changed.sum()),
                changed_colour=_hexs(ring_col) if ring_col else None,
                changed_vs_plate=round(_contrast(ring_col, base), 2) if ring_col else None)


def m_switchstates(r, arg):
    """switchstates: the mode switch in four states, each at 1:1 and 4x with its measures: selected (pointer
    in the pane, not on the switch), the unselected segment hovered, the unselected segment with keyboard focus,
    the selected segment with keyboard focus. Then the lane heads (Mute/Solo) at 1:1 and 4x with the
    accessibility lines of the first lane's buttons. Shots details/modeswitch-states and details/lane-heads."""
    from PIL import Image, ImageDraw, ImageFont
    sc = r.s.scale
    # `switchstates waveform` puts the plot in the waveform first (the switch on the waveform's plate) and
    # skips the lane heads; the mode is put back after
    plate_kind = arg.strip() or "spectrogram"  # "lanes": only the lane heads (at 24 pt)
    mode0 = r.s.ev("viewerController.visualizationMode")
    if plate_kind == "waveform":
        r.s.ev("viewerController.setVisualizationMode(1)")
        r.s.ready(timeout=3)
    pane = r.s.rect("spectrogramPane")
    if not pane:
        raise LookupError("switchstates: no spectrogramPane")
    ring_hex = r.s.ev("String(root.uiPalette.focusRing)") or ""

    def buttons():
        out = []
        for i in range(4):
            rr = r.s.rect(f"visualizationModeButton{i}")
            if not rr:
                break
            out.append((i, rr, r.s.ev(f"String({FIND}('visualizationModeButton{i}').checked)") == "true"))
        if len([b for b in out if b[2]]) != 1:
            raise LookupError(f"switchstates: want one checked segment, have {[(b[0], b[2]) for b in out]}")
        return out
    in_pane = (pane[0] + pane[2] * 0.4, pane[1] + pane[3] * 0.5)
    cells, base_im = [], None
    if plate_kind == "lanes":
        _lane_heads(r, in_pane, zoom=2, only_unchecked=True)
        return
    for label in ("selected", "unselected hovered", "unselected focused", "selected + focused"):
        r.s.ev("root.contentItem.forceActiveFocus()")
        r.s.move(*in_pane)
        time.sleep(0.4)
        btns = buttons()
        ck = [b for b in btns if b[2]][0]
        un = [b for b in btns if not b[2]][0]
        if label == "unselected hovered":
            r.s.move(un[1][0] + un[1][2] / 2, un[1][1] + un[1][3] / 2)
        elif label.endswith("focused"):
            tgt = un if label.startswith("unselected") else ck
            r.s.ev(f"{FIND}('visualizationModeButton{tgt[0]}').forceActiveFocus(Qt.TabFocusReason)")
        time.sleep(0.6)
        # (the waveform run's grabs apart from the spectrogram run's: each composite's sources name its own walks)
        im = _grab_window(r, "switch-" + ("waveform-" if plate_kind == "waveform" else "")
                          + label.replace(" ", "-").replace("+", "and"))
        sw = r.s.rect("visualizationModeSwitch")
        er = r.s.visible_rects("plotToolButtonCheckedEdge")
        er = next((v[1:] for v in er if sw[0] <= v[1] + v[3] / 2 <= sw[0] + sw[2] and sw[1] <= v[2] + v[4] / 2 <= sw[1] + sw[3]), None)
        m = _switch_measure(im, sc, btns, sw, ring_hex, base_im, er, pane)
        base_im = base_im or im
        c = im.crop(tuple(int(round(v * sc)) for v in (sw[0] - 3, sw[1] - 3, sw[0] + sw[2] + 3, sw[1] + sw[3] + 3)))
        cells.append((label, c, m))
    r.s.ev("root.contentItem.forceActiveFocus()")
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 14)
    fr = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 12)
    # each state: the 1:1 crop beside its 3x, the state as the label, the measures in three short lines, so the
    # dark and the light sheets of both plates fit one focus image
    Z = 3
    notes = lambda m: [f"edge {m['edge']}: {m['edge_vs_plate']}:1 plate,", f"{m['edge_vs_fill']}:1 fill, {m['edge_vs_pane']}:1 plot",
                       (f"vs selected: {m['changed_px']} px, {m['changed_colour']} ({m['changed_vs_plate']}:1)"
                        if m["changed_px"] else f"unselected {m['unselected_fill']} ({m['unselected_vs_plate']}:1)")]
    ch = max(c.height for _, c, _ in cells)
    c1w = max(c.width for _, c, _ in cells)
    cw = max(c1w + 8 + c1w * Z, 250) + 16
    S = Image.new("RGB", (cw * len(cells) + 20, 26 + ch * Z + 6 + 3 * 15 + 6), (78, 82, 88))
    d = ImageDraw.Draw(S)
    for k, (label, c, m) in enumerate(cells):
        x = 10 + k * cw
        d.text((x, 4), label, fill=(255, 220, 120), font=fb)
        S.paste(c, (x, 26))
        S.paste(c.resize((c.width * Z, c.height * Z), Image.NEAREST), (x + c1w + 8, 26))
        for j, t in enumerate(notes(m)):
            d.text((x, 26 + ch * Z + 6 + 15 * j), t, fill=(230, 230, 230), font=fr)
    summ = "; ".join(f"{lb}: fill {m['fill']} {m['fill_vs_plate']}:1 to the plate {m['plate']}, edge {m['edge']}"
                     f"{' (plotToolButtonCheckedEdge)' if m['edge_item'] else ' (no edge item: the outer pixel ring)'} "
                     f"{m['edge_vs_plate']}:1 to the plate, {m['edge_vs_fill']}:1 to the fill, {m['edge_vs_pane']}:1 to the plot "
                     f"{m['pane']}, glyphs: checked {m['glyph_checked'][0]} ({m['glyph_checked'][1]}:1 on its fill), "
                     f"unchecked {m['glyph_unchecked'][0]} ({m['glyph_unchecked'][1]}:1), unselected {m['unselected_fill']} {m['unselected_vs_plate']}:1 "
                     f"(selected {m['selected_vs_unselected']}:1 to it)" + (f", {m['changed_px']} px changed from the selected state, mostly "
                                                  f"{m['changed_colour']} ({m['changed_vs_plate']}:1 to the plate)"
                                                  if m["changed_px"] else "") for lb, _, m in cells)
    _save_shot(r, S, "details", "modeswitch-states" + ("-waveform" if plate_kind == "waveform" else ""),
               f"The mode switch on the {plate_kind}'s plate at 1:1 and 4x in four states: {summ}")
    _fact(r, f"modeswitch-states-{plate_kind}", f"Mode switch states on the {plate_kind} ({r.s.tag}, focus ring {ring_hex}): {summ}",
          states={lb: m for lb, _, m in cells})
    if plate_kind == "waveform":
        r.s.ev(f"viewerController.setVisualizationMode({int(mode0 or 0)})")
        r.s.ready(timeout=3)
        r.s.move(r.s.size[0] - 5, r.s.size[1] * 0.45)
        return
    _lane_heads(r, in_pane, zoom=3)


def _lane_heads(r, in_pane, zoom=3, only_unchecked=False):
    """The first lane head (label, Mute, Solo) at ZOOM: unchecked, Mute checked, Solo checked; the letters' chroma
    (grey antialiasing: low; subpixel: coloured fringes); the buttons' accessibility lines with which has focus."""
    from PIL import Image, ImageDraw, ImageFont
    import numpy as np
    sc = r.s.scale
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 14)
    fr = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 12)
    # the lane heads: pointer over the first lane (its buttons show while the lane is hovered); a11y of its buttons
    lab = r.s.visible_rects("plotLaneLabel")
    if lab:
        in_pane = (in_pane[0], lab[0][2] + 30)
    r.s.move(*in_pane)
    time.sleep(0.6)
    heads = r.s.visible_rects("plotLaneHead")
    if not heads:
        raise LookupError("switchstates: no plotLaneHead shown")
    im = _grab_window(r, "lane-heads")
    # the first lane's head (its label, Mute and Solo; the others are alike), its own buttons' extent
    bt = [b for nm in ("plotLaneLabel", "plotLaneMuteButton", "plotLaneSoloButton") for b in r.s.visible_rects(nm)[:1]]
    if len(bt) == 3:
        hx, hy = min(b[1] for b in bt), min(b[2] for b in bt)
        hw, hh = max(b[1] + b[3] for b in bt) - hx, max(b[2] + b[4] for b in bt) - hy
    else:
        _, hx, hy, hw, hh = heads[0]
    box_ = tuple(int(round(v * sc)) for v in (hx - 3, hy - 3, hx + hw + 3, hy + hh + 3))
    crops = [("unchecked", im.crop(box_))]
    # Mute checked, then Solo checked (clicked, grabbed, clicked back), each with its accessibility states
    a11y = []
    if len(bt) != 3:  # loudly: a head without its buttons shown is not what this image claims
        raise LookupError(f"lane heads: label, Mute and Solo not all shown ({[b[0] for b in bt]})")
    for nm, lb in (() if only_unchecked else (("plotLaneMuteButton", "Mute checked"), ("plotLaneSoloButton", "Solo checked"))):
        c_ = r.s.center(nm)
        r.s.click(*c_)
        time.sleep(0.5)
        r.s.move(*in_pane)
        time.sleep(0.4)
        g_ = _grab_window(r, "lane-heads-" + lb.split()[0].lower())
        crops.append((lb, g_.crop(box_)))
        if lb == "Solo checked":  # Solo's effect on the other lanes: the pane's left 330 px, every lane, 1:1
            pr_ = r.s.rect("spectrogramPane")
            if pr_:
                sol = g_.crop(tuple(int(round(v * sc)) for v in (pr_[0], pr_[1], pr_[0] + min(330, pr_[2]), pr_[1] + pr_[3])))
                _save_shot(r, sol, "details", "solo-effect", "Lane 1 soloed: the pane's left 330 px with every lane, 1:1")
        r.s.click(*c_)
        time.sleep(0.4)
    # the accessibility transcript of each button with keyboard focus, and at that same moment which of the two has
    # activeFocus and what the other's interface says (one focus at a time, or both "focused"?)
    for nm in ("plotLaneMuteButton", "plotLaneSoloButton"):
        ok = r.s.ev(f"(function(){{var it={FIND}('{nm}');if(!it)return false;it.forceActiveFocus(Qt.TabFocusReason);return it.activeFocus}})()")
        if ok is not True and str(ok) != "true":
            raise LookupError(f"switchstates: {nm} cannot take focus ({ok})")
        time.sleep(0.2)
        row = json.loads(r.s.cmd("a11y"))
        if nm == "plotLaneMuteButton" and not only_unchecked:  # the head with Mute's focus ring, 1:1 and 3x
            from PIL import Image as _I
            fc = _grab_window(r, "lane-head-focus").crop(box_)
            fz = fc.resize((fc.width * 3, fc.height * 3), _I.NEAREST)
            F_ = _I.new("RGB", (fc.width + fz.width + 30, fz.height + 20), (78, 82, 88))
            F_.paste(fc, (10, 10))
            F_.paste(fz, (20 + fc.width, 10))
            _save_shot(r, F_, "details", "lane-head-focus", "The first lane head with keyboard focus on Mute (its focus ring), "
                                                             "1:1 and 3x")
        both = r.s.ev(f"JSON.stringify([{FIND}('plotLaneMuteButton').activeFocus, {FIND}('plotLaneSoloButton').activeFocus,"
                      f" {FIND}('plotLaneMuteButton').visualFocus, {FIND}('plotLaneSoloButton').visualFocus])")
        mf, sf, mv, sv = json.loads(both)
        a11y.append(f"{nm} focused: {row.get('role')} \u201c{row.get('name')}\u201d, states {row.get('states') or '—'}"
                    + (f", description {row['description']}" if row.get("description") else "")
                    + f" (at that moment activeFocus Mute {mf}, Solo {sf}; visualFocus Mute {mv}, Solo {sv})")
    r.s.ev("root.contentItem.forceActiveFocus()")
    Z3 = zoom
    # the letters' antialiasing: in each of the label's, Mute's and Solo's own rects (the unchecked grab), every
    # pixel's distance from the straight line between the plate's fill and the letter's colour: grey antialiasing
    # blends along that line (distance ~0), subpixel antialiasing leaves coloured fringes off it
    chroma = []
    a0 = np.asarray(im).astype(float)
    for nm_, b_ in zip(("L", "M", "S"), bt):
        _, bx, by, bw, bh = b_
        q = a0[int(by * sc) + 2:int((by + bh) * sc) - 2, int(bx * sc) + 2:int((bx + bw) * sc) - 2].reshape(-1, 3)
        fill_ = np.array(_colours(q.astype(int), n=1)[0][0], dtype=float)
        ink_ = q[np.argmax(np.abs(q - fill_).sum(axis=1))]
        d_ = ink_ - fill_
        nd = float((d_ * d_).sum()) or 1.0
        tq = np.clip(((q - fill_) @ d_) / nd, 0, 1)
        off = np.sqrt((((q - fill_) - tq[:, None] * d_) ** 2).sum(axis=1))
        edge = off[(tq > 0.05) & (tq < 0.95)]
        chroma.append(f"{nm_}: letter {_hexs(ink_)} on {_hexs(fill_)}, edge pixels off the grey blend mean "
                      f"{(edge.mean() if len(edge) else 0):.1f}, max {(edge.max() if len(edge) else 0):.0f} (RGB units)")
    hw_ = max(c.width * Z3 for _, c in crops) + 16
    hh_ = max(c.height for _, c in crops)
    S2 = Image.new("RGB", (max(hw_ * len(crops) + 20, 360), 26 + 18 + hh_ * Z3 + 10 + 16 * len(a11y) + 8), (78, 82, 88))
    d = ImageDraw.Draw(S2)
    d.text((10, 4), f"Lane heads, {Z3}x ({r.s.tag}, {r.s.suffix.split('_')[-1]})" + ("" if only_unchecked else
           ": unchecked, Mute checked, Solo checked"),
           fill=(255, 220, 120), font=fb)
    for k, (lb, c) in enumerate(crops):
        d.text((10 + k * hw_, 26), lb, fill=(230, 230, 230), font=fr)
        S2.paste(c.resize((c.width * Z3, c.height * Z3), Image.NEAREST), (10 + k * hw_, 44))
    for j, t in enumerate(a11y):
        d.text((10, 44 + hh_ * Z3 + 8 + 16 * j), t[:max(40, int((S2.width - 20) / 6.2))], fill=(230, 230, 230), font=fr)
    _save_shot(r, S2, "details", "lane-heads", f"The plot's first lane head at {Z3}x: unchecked, Mute checked, Solo checked; "
               + "; ".join(chroma) + "; accessibility: " + "; ".join(a11y))
    _fact(r, "lane-heads-a11y", f"Lane heads ({r.s.tag}, {r.s.suffix.split('_')[-1]}): " + "; ".join(chroma) + "; " + "; ".join(a11y))
    r.s.move(r.s.size[0] - 5, r.s.size[1] * 0.45)


def m_platesizes(r, arg):
    """platesizes NAME[,NAME]: each named plate's size, its buttons' rects (descendants whose type is a tool
    button), the padding from the plate's edges to them and the gaps between them; in the window the runner targets."""
    at = ("@" + r.win) if getattr(r, "win", "") else ""
    out = []
    viewer = arg.startswith("@viewer:")
    if viewer:  # open the spectrogram viewer windowed (its mode kept and put back), pointer over it
        arg = arg[len("@viewer:"):]
        mode0 = r.s.ev("root.uiBridge.viewerFullscreenMode")
        r.s.ev("(function(){root.uiBridge.setViewerFullscreenMode(0);viewerController.openSpectrogramViewer();return true})()")
        r.settle_or(1.0)
        r.s.move(700, 450)
        time.sleep(0.6)
    for nm in arg.split(","):
        js = ("(function(n){var r=contentItem;while(r.parent)r=r.parent;var s=[r];var pl=null;while(s.length){var it=s.shift();"
              "if(!it)continue;if(it.objectName===n){pl=it;break}var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}"
              "if(!pl)return '';var p=pl.mapToItem(null,0,0);var vo=1,a=pl;while(a){if(!a.visible){vo=0;break}vo*=a.opacity;a=a.parent}"
              "var o={plate:[p.x,p.y,pl.width,pl.height],shown:vo,buttons:[]};"
              # (the buttons are the plate's descendants, or its siblings when the plate is a backdrop: ViewerCornerControls)
              "var q=[pl.parent];"
              "while(q.length){var it=q.shift();if(it!==pl&&it.visible&&String(it).indexOf('ToolButton')>=0){var b=it.mapToItem(null,0,0);"
              "if(b.x>=p.x-1&&b.y>=p.y-1&&b.x+it.width<=p.x+pl.width+1&&b.y+it.height<=p.y+pl.height+1)"
              "o.buttons.push([it.objectName,b.x,b.y,it.width,it.height]);continue}var c=it.children;for(var i=0;i<c.length;i++)q.push(c[i]);}"
              f"return JSON.stringify(o)}})('{nm}')")
        res = r.s.cmd(f"eval{at} {js}")
        try:
            v = json.loads(res)
            v = json.loads(v[0] if isinstance(v, list) else v) if v and v != [""] else None
        except (TypeError, ValueError):
            v = None
        if not v:  # (diagnostic: what the walk saw in that window)
            dj = ("(function(){var r=contentItem;while(r.parent)r=r.parent;var s=[r];var n=0,o=[];while(s.length){var it=s.shift();"
                  "if(!it)continue;n++;if(/orner|Close|viewer/.test(it.objectName))o.push(it.objectName);var c=it.children;"
                  "for(var i=0;i<c.length;i++)s.push(c[i]);}return n+' items; '+o.slice(0,12).join(',')})()")
            out.append(f"{nm}: not found (probe: {str(res)[:60]}; in {at or 'main'}: {str(r.s.cmd('eval' + at + ' ' + dj))[:300]})")
            continue
        px_, py, pw, ph = v["plate"]
        shown_ = "" if v.get("shown", 1) >= 0.99 else f" (shown at opacity {v.get('shown', 0):.2f})"
        bs = sorted(v["buttons"], key=lambda b: b[1])
        if bs:
            pad = (min(b[1] for b in bs) - px_, min(b[2] for b in bs) - py, px_ + pw - max(b[1] + b[3] for b in bs),
                   py + ph - max(b[2] + b[4] for b in bs))
            gaps = [round(b2[1] - (b1[1] + b1[3]), 1) for b1, b2 in zip(bs, bs[1:])]
            out.append(f"{nm}{shown_} {pw:.0f}x{ph:.0f}: {len(bs)} buttons " + ", ".join(f"{b[3]:.0f}x{b[4]:.0f}" for b in bs)
                       + f"; padding left/top/right/bottom {pad[0]:.0f}/{pad[1]:.0f}/{pad[2]:.0f}/{pad[3]:.0f} px"
                       + (f"; gaps {gaps} px" if gaps else ""))
        else:
            out.append(f"{nm} {pw:.0f}x{ph:.0f}: no tool buttons found")
    if viewer:
        r.s.ev(f"(function(){{viewerController.closeSpectrogramViewer();root.uiBridge.setViewerFullscreenMode({int(mode0 or 0)});return true}})()")
        r.settle_or(1.0)
        r.s.move(r.s.size[0] - 5, r.s.size[1] * 0.45)
    _fact(r, "plates-" + arg.split(",")[0], f"Plate sizes ({r.s.tag}): " + "; ".join(out))
    r.line_desc = (r.line_desc or "") + " [" + "; ".join(out) + "]"


def m_lanesrest(r, arg):
    """lanesrest: Mute and then Solo of lane 1 checked, the pointer away (the buttons' rest state), in the
    spectrogram and the waveform: the pane's left 200 px with every lane at 1:1 (a muted lane's look), and whether the
    checked button shows at rest; shots details/lanes-rest-<mode>-<mute|solo>. The mode and the lanes are put back."""
    pane = r.s.rect("spectrogramPane")
    if not pane:
        raise LookupError("lanesrest: no spectrogramPane")
    sc = r.s.scale
    mode0 = r.s.ev("viewerController.visualizationMode")
    notes = []
    for mode, mname in ((0, "spectrogram"), (1, "waveform")):
        r.s.ev(f"viewerController.setVisualizationMode({mode})")
        r.s.ready(timeout=3)
        time.sleep(0.4)
        for nm, lb in (("plotLaneMuteButton", "mute"), ("plotLaneSoloButton", "solo")):
            lab = r.s.visible_rects("plotLaneLabel")
            lane_pt = (pane[0] + pane[2] * 0.4, (lab[0][2] if lab else pane[1]) + 30)
            r.s.move(*lane_pt)
            time.sleep(0.6)
            c_ = r.s.center(nm)
            if not c_:
                raise LookupError(f"lanesrest: no {nm}")
            r.s.click(*c_)
            time.sleep(0.4)
            r.s.move(r.s.size[0] - 5, r.s.size[1] * 0.45)  # away: the buttons' rest
            time.sleep(1.6)
            shown = bool(r.s.visible_rects(nm))
            g = _grab_window(r, f"lanes-rest-{mname}-{lb}")
            crop = g.crop(tuple(int(round(v * sc)) for v in (pane[0], pane[1], pane[0] + min(200, pane[2]), pane[1] + pane[3])))
            desc = (f"{lb.capitalize()} of lane 1 checked, the pointer away, {mname}: the pane's left 200 px, 1:1; the checked "
                    f"button {'shows' if shown else 'is HIDDEN'} at rest")
            _save_shot(r, crop, "details", f"lanes-rest-{mname}-{lb}", desc)
            notes.append(f"{mname}, {lb} checked: the button {'shows' if shown else 'is hidden'} at rest")
            r.s.move(*lane_pt)
            time.sleep(0.6)
            r.s.click(*r.s.center(nm))  # back
            time.sleep(0.4)
    r.s.ev(f"viewerController.setVisualizationMode({int(mode0 or 0)})")
    r.s.ready(timeout=3)
    _fact(r, "lanes-rest", f"Lane buttons at rest ({r.s.tag}): " + "; ".join(notes))


_METER_POINTS = [(0.0, 1.0), (-6.0, 0.8), (-12.0, 0.6), (-24.0, 0.4), (-48.0, 0.2), (-60.0, 0.0)]


def _meter_position(db):
    """LevelMeterItem::meterPosition as a share of the scale: 0, -6, -12, -24, -48, -60 dB at 5/5 .. 0/5."""
    if db is None or db <= -60.0:
        return 0.0
    db = min(db, 0.0)
    for (u_db, u), (l_db, l) in zip(_METER_POINTS, _METER_POINTS[1:]):
        if db >= l_db:
            return l + (u - l) * (db - l_db) / (u_db - l_db)
    return 0.0


def m_meterprobe(r, arg):
    """meterprobe LABEL: the level meter as LevelMeterItem lays it out: per lane (laneRows: seams on the
    rows k * height / count), the scale between 3 px insets under the 3 px clip lamp and its 1 px gap. Down the bar's
    middle column: the rows of the meter's own fillColor (the bar: its top, as a share of the scale, against the share
    the lane's readout level should reach through meterPosition), the peakColor row (against the readout's peak), the
    mark gaps inside the bar (against -6/-12/-24 dB), the fill against the trough and the floor. A 4x close-up of lane 1."""
    import numpy as np
    from PIL import Image, ImageDraw, ImageFont
    # (the levels move with the music: the readout is read just before and just after the grab, and the drawn bar is
    # judged against the range between them. The window settles first, then read - grab - read with no wait in
    # between: the settle used to sit between the first read and the grab, so a transient fell into that gap and the
    # facts contradicted the pixels)
    rd_js = (f"(function(){{var m={FIND}('levelMeter');var o=[];for(var i=0;i<m.laneCount;i++){{var q=m.laneReadout(i);"
             f"o.push([isFinite(q.level)?q.level:null,isFinite(q.peak)?q.peak:null])}}return JSON.stringify(o)}})()")
    r.s.settle_opacity()
    before = json.loads(r.s.ev(rd_js) or "[]")
    im = _grab_window(r, f"meterprobe-{arg}", settle=False)
    after = json.loads(r.s.ev(rd_js) or "[]")
    rs = r.s.visible_rects("levelMeter")
    if not rs:
        raise LookupError("meterprobe: no levelMeter shown")
    _, x, y, w, h = rs[0]
    sc = r.s.scale
    a = np.asarray(im).astype(int)
    pj = r.s.ev(f"(function(){{var m={FIND}('levelMeter');var o={{lanes:m.laneCount,framed:m.framed,fill:String(m.fillColor),"
                f"trough:String(m.troughColor),gap:String(m.gapColor),peak:String(m.peakColor),r:[]}};"
                f"for(var i=0;i<m.laneCount;i++){{var q=m.laneReadout(i);o.r.push([isFinite(q.level)?q.level:null,"
                f"isFinite(q.peak)?q.peak:null])}}return JSON.stringify(o)}})()")
    if not pj:
        raise LookupError("meterprobe: the meter gave no properties")
    P = json.loads(pj)
    def hx(c_):  # "#rrggbb" or "#aarrggbb"
        if not c_ or not c_.startswith("#") or len(c_) not in (7, 9):
            return None
        c_ = "#" + c_[3:] if len(c_) == 9 else c_
        return tuple(int(c_[q:q + 2], 16) for q in (1, 3, 5))
    fill, trough, peakc = hx(P["fill"]), hx(P["trough"]), hx(P["peak"])
    n = max(1, int(P["lanes"]))
    cx = int(round((x + (2 + 4 if P.get("framed") else 2 + 5)) * sc))  # the bar's middle (framed: 8 px bar 2 px in)
    col = a[:, cx]
    floor_hex = r.s.ev("String(root.uiPalette.plotFloor)") or "#14062e"
    fl = hx(floor_hex)
    near = lambda c_, ref: ref is not None and np.abs(np.array(c_) - np.array(ref)).sum() <= 40
    fmt = lambda v: "-inf" if v is None else f"{v:.1f}"
    parts, close = [], None
    for i in range(n):
        top = round(i * h / n) + 1 if i > 0 else 0
        bot = round((i + 1) * h / n) if i + 1 < n else h
        s_top, s_foot = top + 3 + 3 + 1, bot - 3  # under the lamp and its gap; above the foot's inset
        sh = max(1.0, s_foot - s_top)
        ys = range(int(s_top * sc), int(s_foot * sc))
        fill_rows = [yy for yy in ys if near(col[int(y * sc) + yy], fill)]
        lv, pk = (P["r"][i] if i < len(P["r"]) else (None, None))
        lvs = [v[i][0] for v in (before, after) if i < len(v)]
        pks = [v[i][1] for v in (before, after) if i < len(v)]
        lv = lvs[0] if lvs else lv
        pk = pks[0] if pks else pk
        exp_lo, exp_hi = (min(_meter_position(v) for v in lvs), max(_meter_position(v) for v in lvs)) if lvs else (0, 0)
        exp_top = _meter_position(lv)
        if fill_rows:
            m_top = (s_foot * sc - min(fill_rows)) / (sh * sc)
            # the gaps inside the bar: runs of 2+ non-fill rows between fill rows (rung gaps are 1 px)
            gaps, run, prev = [], 0, None
            for yy in range(max(fill_rows), min(fill_rows) - 1, -1):
                if near(col[int(y * sc) + yy], fill):
                    if run >= 2 * sc:
                        gaps.append((s_foot * sc - (yy + 1 + run / 2)) / (sh * sc))
                    run = 0
                else:
                    run += 1
            # the bar is drawn in whole rungs: its top can sit up to one rung pitch under the exact level, so the
            # lower bound allows one rung (the pitch measured from the fill's runs down this column)
            starts = [yy for yy in sorted(fill_rows) if yy - 1 not in fill_rows]
            pitch = (sorted(b_ - a_ for a_, b_ in zip(starts, starts[1:]))[len(starts) // 2 - 1] / sc
                     if len(starts) >= 3 else 0.0)
            off_ = 0.0 if exp_lo - (1.5 + pitch) / sh <= m_top <= exp_hi + 1.5 / sh else \
                (m_top - exp_hi if m_top > exp_hi else m_top - exp_lo) * sh
            rung_low = off_ == 0.0 and m_top < exp_lo - 1.5 / sh
            lv_txt = (f"{fmt(lvs[0])} dB just before the grab and {fmt(lvs[-1])} dB just after" if len(lvs) == 2 and
                      lvs[0] != lvs[-1] else f"{fmt(lv)} dB")
            bar_txt = (f"bar top at {m_top:.3f} of the scale ({sh:.0f} px); the readout's level, {lv_txt}, puts it at "
                       f"{exp_lo:.3f}–{exp_hi:.3f} ("
                       + (f"within it, a rung under it (rung pitch {pitch:.0f} px)" if off_ == 0 and rung_low else
                          "within it" if off_ == 0 else f"off by {off_:+.1f} px") + ")")
            gaps_txt = ("; mark gaps in the bar at " + ", ".join(f"{g_:.3f}" for g_ in gaps) + " (-6/-12/-24 dB: 0.800/0.600/0.400)"
                        if gaps else "; no mark gap inside the bar")
            capc = col[int(y * sc) + min(fill_rows)]
            cap_txt = (f"; fill {_hexs(capc)}: {_contrast(tuple(capc), trough):.2f}:1 to the trough {_hexs(trough)}, "
                       f"{_contrast(tuple(capc), fl):.2f}:1 to the floor {floor_hex}") if trough and fl else ""
        else:
            bar_txt, gaps_txt, cap_txt = f"no fill-coloured row (level {fmt(lv)} dB should reach {exp_top:.3f})", "", ""
        pk_rows = [yy for yy in ys if near(col[int(y * sc) + yy], peakc)]
        pk_txt = (f"; peak tick at {(s_foot * sc - min(pk_rows)) / (sh * sc):.3f} (the readout's peak {fmt(pk)} dB: "
                  f"{_meter_position(pk):.3f})") if pk_rows else f"; no peak-coloured row (peak {fmt(pk)} dB)"
        parts.append(f"lane {i + 1}: {bar_txt}{gaps_txt}{pk_txt}{cap_txt}")
        if i == 0:
            close = (int(y * sc) + (min(fill_rows) if fill_rows else int(s_top * sc)), min(pk_rows) + int(y * sc) if pk_rows else None)
    mode = r.s.ev("viewerController.visualizationMode")
    mname = "waveform" if str(mode) == "1" else "spectrogram"
    if close:  # lane 1 around its bar top and peak tick, 4x
        ctop, cpk = close
        y0_ = max(0, min(ctop, cpk if cpk is not None else ctop) - 30)
        y1_ = min(im.height, y0_ + 80)
        X0, W_ = int(x * sc), int(w * sc)
        c = im.crop((X0 - 6, y0_, min(im.width, X0 + W_ + 3), y1_))
        z = c.resize((c.width * 4, c.height * 4), Image.NEAREST)
        S = Image.new("RGB", (max(z.width + 20, 150), z.height + 44), (78, 82, 88))
        d = ImageDraw.Draw(S)
        d.text((10, 4), f"{mname.capitalize()}, {r.s.tag}, 4x", fill=(255, 220, 120),
               font=ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 13))
        d.text((10, 22 + z.height + 2), "lane 1: bar top, peak tick", fill=(230, 230, 230),
               font=ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 11))
        S.paste(z, (10, 22))
        _save_shot(r, S, "details", f"meter-closeup-{arg}", f"The level meter's lane 1 around its bar top and peak tick at 4x "
                                                            f"({mname}): {parts[0]}")
    txt = (f"{arg} ({mname}, {w:.0f}x{h:.0f} px, {'framed' if P.get('framed') else 'unframed'}, fill {P['fill']}, "
           f"trough {P['trough']}): " + "; ".join(parts))
    _fact(r, f"meter-{arg}", f"Level meter, {txt} ({r.s.tag})")
    r.line_desc = (r.line_desc or "") + f" [{txt}]"


def m_queuefocus(r, arg):
    """queuefocus [LABEL [KEYS]]: the queue's keyboard focus reached with real keys: Tab from the window (focus on
    its content item) until the queue's list has active focus (at most 40 presses), then KEYS (comma-separated key
    names, default Down; End,Up in the long list); facts queue-focus-[LABEL-]THEME: the Tab count,
    playlistView.activeFocus, the pane's keyboardInUse (InputModality.keyboard), the current row index, and how many
    rows draw the focus ring (rowCurrent). Loud when Tab never reaches the queue."""
    label, _, keys = (arg or "").strip().partition(" ")
    keys = [k for k in (keys or "Down").split(",") if k and k != "-"]  # "-": Tab alone, no key after it
    r.s.ev("root.contentItem.forceActiveFocus()")
    time.sleep(0.2)
    n = 0
    while r.s.ev(f"String({FIND_ANY}('playlistView').activeFocus)") != "true":
        if n >= 40:
            raise LookupError("queuefocus: 40 Tab presses did not reach the queue's list")
        r.step_key("Tab")
        n += 1
        time.sleep(0.05)
    for k in keys:
        r.step_key(k)
        time.sleep(0.15)
    time.sleep(0.3)
    v = r.s.ev("(function(){var q=" + FIND_ANY + "('mainQueuePane');var v=" + FIND_ANY + "('playlistView');var n=0;"
               "var s=[v];while(s.length){var it=s.shift();if(!it)continue;if(it.rowCurrent===true)n++;"
               "var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}"
               "return JSON.stringify([v.activeFocus,q.keyboardInUse,q.controller?q.controller.currentRowIndex:null,n])})()")
    af, kb, cur, n_ring = json.loads(v)
    _fact(r, f"queue-focus-{label + '-' if label else ''}{r.s.tag}",
          f"Queue keyboard focus{' (' + label + ')' if label else ''}, reached with {n} Tab press{'es' if n != 1 else ''} "
          f"then {', '.join(keys) or 'no other key'}: "
          f"playlistView.activeFocus {af}, keyboardInUse (InputModality.keyboard) {kb}, current row index {cur}, rows "
          f"drawing the focus ring (rowCurrent) {n_ring}" + ("" if n_ring else " -- NO ROW DRAWS THE RING")
          + f" ({r.s.suffix.replace('_', ' ')})")
    if not n_ring:
        r.line_desc = (r.line_desc or "") + " [no row draws the focus ring]"


def m_queuenow(r, arg):
    """queuenow top|after: the queue's playing row and its scroll. "top" scrolls the list so the playing row is
    its second row (the recording of Next must show it: in the motion session it sat just above the view); both record
    fact queue-now-ARG-THEME: playingQueueIndex, the first row shown, contentY, originY, rowHeight and the view's
    height, and whether the playing row is in view."""
    js = ("(function(){var v=" + FIND_ANY + "('playlistView');var i=root.uiBridge.playingQueueIndex;"
          + ("if(i>=0)v.contentY=Math.max(v.originY,Math.min(v.originY+v.contentHeight-v.height,"
             "v.originY+(i-1)*v.rowHeight));" if arg == "top" else "")
          + "var first=v.indexAt(1,v.contentY+1);var top=v.originY+i*v.rowHeight;"
          "return JSON.stringify([i,first,v.contentY,v.originY,v.rowHeight,v.height,"
          "top>=v.contentY&&top+v.rowHeight<=v.contentY+v.height])})()")
    i, first, cy, oy, rh, h, shown = json.loads(r.s.ev(js))
    time.sleep(0.3)
    _fact(r, f"queue-now-{arg}-{r.s.tag}", f"Queue's playing row ({arg}): playingQueueIndex {i}, first row shown {first}, "
          f"contentY {cy:.0f}, originY {oy:.0f}, rowHeight {rh:.0f}, view height {h:.0f}, playing row "
          + ("in view" if shown else "NOT IN VIEW") + f" ({r.s.suffix.replace('_', ' ')})")


# items whose contents name this machine's paths but are not a recorded text: painted over in the pack
PRIVATE_ITEMS = []  # (none whole: at the neutral paths the log names no home folder, and any text that does is
# boxed by its content, character-exact in a text edit; painting the whole diagnostics log hid too much)


def private_sidecar(r, file):
    """After every dialog, viewer and search grab (runner): what names this machine's home folder in the
    windows the image shows, as boxes in the image for pack.py to paint over (shots/FILE.redact.json): PRIVATE_ITEMS
    whole (the diagnostics log's body); in a text edit (TextArea: Tag Editor Details' save errors) the exact boxes of
    the home-folder characters, line by line (positionToRectangle), its name listed as `precise`; any other item whose
    text names it (a label, a tooltip, which the layout's 60 recorded characters can miss) whole. The windows answer
    in logical px, scaled to the image's device px (#666). Windows: a dialog capture's own window at its image offset
    and an outer dialog window of a nested capture at the capture's origin below its title bar (28 logical px); a
    viewer window's or the main window's shot at 0, 0."""
    e = next((x for x in reversed(r.s.index) if x["file"] == file + ".png"), None)
    if not e:
        return
    g = e.get("grabbed") or {}
    home = os.path.expanduser("~")
    sc = r.s.scale
    targets = []  # (how to evaluate, root expression, offset)
    if g.get("kind") == "dialog":
        dws = r.s.dialog_windows()
        for k, dw in enumerate(dws):
            off = g.get("image_offset", [0, 0]) if dw["cls"] == g.get("cls") else ((0, 28 * sc) if k == len(dws) - 1 else None)
            if off is not None:
                targets.append((dw["cls"], "(function(){var x=contentItem;while(x.parent)x=x.parent;return x})()", off))
    elif g.get("kind") == "window" and g.get("name"):
        targets.append((g["name"], "(function(){var x=contentItem;while(x.parent)x=x.parent;return x})()",
                        g.get("image_offset", [0, 0])))
    else:
        targets.append((None, "(function(){var x=root.contentItem;while(x.parent)x=x.parent;return x})()", (0, 0)))
    boxes, precise = [], []
    for win, top, off in targets:
        pj = ("(function(){var H=" + json.dumps(home) + ";var INV='\\u200b\\u200c\\u200d\\u2060\\ufeff\\u00ad';"
              "function cl(x){var c='',m=[];for(var i=0;i<x.length;i++){if(INV.indexOf(x[i])<0){c+=x[i];m.push(i)}}return [c,m]}"
              "var t=" + top + ";var o=[],pr=[];var s=[t];while(s.length){var it=s.shift();if(!it||!it.visible||it.opacity===0)continue;"
              "if(" + json.dumps(PRIVATE_ITEMS) + ".indexOf(it.objectName)>=0){var p=it.mapToItem(null,0,0);"
              "o.push([p.x,p.y,it.width,it.height])}"
              "else if(typeof it.text==='string'&&it.text.length&&cl(it.text)[0].indexOf(H)>=0){"
              "if(typeof it.positionToRectangle==='function'){var cm=cl(it.text),c=cm[0],m=cm[1],f=0,k;"
              "while((k=c.indexOf(H,f))>=0){var a=m[k],b=m[k+H.length-1]+1,L={};"
              "for(var q=a;q<=b;q++){var r=it.positionToRectangle(q);var pp=it.mapToItem(null,r.x,r.y);var y=Math.round(pp.y);"
              "if(!L[y])L[y]=[pp.x,pp.x,pp.y,r.height];L[y][0]=Math.min(L[y][0],pp.x);L[y][1]=Math.max(L[y][1],pp.x)}"
              "for(var y in L){var v=L[y];o.push([v[0]-1,v[2],Math.max(4,v[1]-v[0])+2,v[3]])}f=k+H.length}"
              "if(it.objectName&&pr.indexOf(it.objectName)<0)pr.push(it.objectName)}"
              "else{var p2=it.mapToItem(null,0,0);o.push([p2.x,p2.y,it.width,it.height])}}"
              "var ch=it.children;for(var j=0;j<ch.length;j++)s.push(ch[j]);}"
              "return JSON.stringify({boxes:o,precise:pr})})()")
        rr = r.s.a.cmd(f"eval@{win} {pj}") if win else r.s.ev(pj)
        try:
            got = json.loads(rr)
            got = got[0] if isinstance(got, list) and got else got
            got = json.loads(got) if isinstance(got, str) else got
        except (TypeError, ValueError):
            raise RuntimeError(f"private_sidecar {file}: the window {win or 'main'} did not answer ({str(rr)[:80]})")
        boxes += [[x_ * sc + off[0], y_ * sc + off[1], w_ * sc, h_ * sc] for x_, y_, w_, h_ in got["boxes"]]
        precise += got["precise"]
    if boxes:
        with open(os.path.join(os.environ["SHOT_OUT"], file + ".redact.json"), "w") as f:
            json.dump(dict(boxes=boxes, precise=precise,
                           why="the diagnostics log, and texts that name this machine's home folder"), f)


def _rgb_hex(c):
    c = str(c).lstrip("#")
    c = c[2:] if len(c) == 8 else c  # #aarrggbb
    return tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))


def m_dlgfacts(r, arg):
    """dlgfacts AREA/STATE: facts on the dialog (or popup) the state just grabbed, run as the next line, so
    nothing has changed since the grab. Live, in that window: what has keyboard focus as the dialog opened, its
    primary button, every visible button (highlighted, focused), the window's size and minimum, and any scroll area
    whose content is larger than its view (a scrollbar). From the grabbed image and its layout: contrast of the
    window's texts (declared colour and strongest ink against the colour behind them) and of its buttons' labels
    on their faces. Fact dlg-STATE-SUFFIX. A main-window popup (search, Recent Issues): its texts inside the popup."""
    import numpy as np
    from PIL import Image
    area, _, state = arg.partition("/")
    file = f"{area}_{state}_{r.s.suffix}"
    e = next((x for x in reversed(r.s.index) if x["file"] == file + ".png"), None)
    if not e or not e.get("layout"):
        raise LookupError(f"dlgfacts: no grab with a layout for {file}")
    root_ = os.path.dirname(os.environ["SHOT_OUT"].rstrip("/"))
    wins = json.load(open(os.path.join(root_, e["layout"])))["windows"]
    g = e.get("grabbed") or {}
    if g.get("kind") == "dialog":
        w = next((w_ for w_ in wins if w_["cls"] == g.get("cls")), wins[-1])
        dx, dy = g.get("image_offset", [0, 0])
        clip = None
        ev = lambda js: r.s.a.cmd(f"eval@{g['cls']} {js}")
        top = "(function(){var x=contentItem;while(x.parent)x=x.parent;return x})()"
        winp = "[width,height,minimumWidth,minimumHeight,title]"
        foc = "activeFocusItem"
    else:
        w = next(w_ for w_ in wins if w_.get("main"))
        dx = dy = 0
        clip = (e.get("popups") or [None])[0]
        ev = lambda js: r.s.ev(js)
        top = "root.Overlay.overlay"
        winp = "[root.width,root.height,0,0,'(main window popup)']"
        foc = "root.activeFocusItem"
    js = ("(function(){var t=" + top + ";var o={win:" + winp + ",focus:null,buttons:[],scroll:[]};var f=" + foc + ";"
          "if(f)o.focus=(f.text?('\u201c'+f.text+'\u201d '):'')+(f.objectName||String(f).split('(')[0].split('_QML')[0]);"
          "var s=[t];while(s.length){var it=s.shift();if(!it||!it.visible||it.opacity===0)continue;"
          "if(it.highlighted!==undefined&&it.flat!==undefined&&it.text)o.buttons.push([it.text,!!it.highlighted,!!it.activeFocus,"
          "it.objectName||'',it.enabled]);"
          "if(it.contentHeight!==undefined&&it.flickableDirection!==undefined&&it.height>8&&"
          "(it.contentHeight>it.height+1||it.contentWidth>it.width+1))o.scroll.push([it.objectName||String(it).split('(')[0].split('_QML')[0],"
          "Math.round(it.contentWidth),Math.round(it.contentHeight),Math.round(it.width),Math.round(it.height)]);"
          "var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}return JSON.stringify(o)})()")
    raw = ev(js)
    try:
        d = json.loads(raw)
        if isinstance(d, list):  # eval@WINDOW answers a list of its values (shotlib.focus_default_button)
            d = d[0]
        if isinstance(d, str):
            d = json.loads(d)
    except (TypeError, ValueError, IndexError):
        raise RuntimeError(f"dlgfacts {file}: the window did not answer ({str(raw)[:120]})")
    im = Image.open(os.path.join(os.environ["SHOT_OUT"], file + ".png")).convert("RGB")
    a = np.asarray(im).astype(np.int16)

    def box(rc, inset=0):
        x, y, ww, hh = rc
        x0, y0 = int(x + dx + inset), int(y + dy + inset)
        x1, y1 = int(x + dx + ww - inset), int(y + dy + hh - inset)
        return max(0, x0), max(0, y0), min(im.width, x1), min(im.height, y1)

    def judge(rc, inset=0):
        x0, y0, x1, y1 = box(rc, inset)
        if x1 - x0 < 2 or y1 - y0 < 2:
            return None
        px = a[y0:y1, x0:x1].reshape(-1, 3)
        vals, cnt = unique_colours(px)
        bg = tuple(int(v) for v in vals[cnt.argmax()])
        ink = max((tuple(int(v) for v in p_) for p_ in vals), key=lambda p_: _contrast(p_, bg))
        return bg, ink

    inside = lambda rc: clip is None or (rc[0] >= clip[0] - 1 and rc[1] >= clip[1] - 1 and rc[0] + rc[2] <= clip[0] + clip[2] + 1
                                         and rc[1] + rc[3] <= clip[1] + clip[3] + 1)
    texts, rows_c, undrawn = [], [], []
    for t in w.get("texts", []):
        if not t.get("text") or t.get("opacity", 1) < 0.5 or t.get("dimmed") or not inside(t["rect"]):
            continue
        j = judge(t["rect"])
        if not j:
            continue
        bg, ink = j
        decl = _rgb_hex(t.get("color", "#000000"))
        if _contrast(ink, bg) < 1.1:  # listed but not drawn there (a list's kept delegate, covered or culled)
            undrawn.append(t.get("name") or t.get("type"))
            continue
        texts.append(f"{t.get('name') or t.get('type')} \u201c{t['text'][:28]}\u201d {_hexc(decl)} on {_hexc(bg)} "
                     f"{_contrast(decl, bg):.1f}:1 (ink {_contrast(ink, bg):.1f}:1)")
        rows_c.append([t.get("name") or t.get("type"), t["text"][:40], _hexc(decl), _hexc(ink), _hexc(bg),
                       round(_contrast(decl, bg), 2), round(_contrast(ink, bg), 2), t.get("px") or 0, bool(t.get("bold"))])
    btns = []
    for b in w.get("actions", []):
        if b.get("type") not in ("Button", "ToolButton", "DialogButton") or not b.get("text") or not inside(b["rect"]):
            continue
        j = judge(b["rect"], inset=3)
        if j:
            bg, ink = j
            btns.append(f"\u201c{b['text']}\u201d label {_contrast(ink, bg):.1f}:1 on {_hexc(bg)}"
                        + ("" if b.get("enabled", True) else " (disabled)"))
            if b.get("enabled", True):
                rows_c.append([b.get("name") or "button", "\u201c" + b["text"][:30] + "\u201d (button label)", "", _hexc(ink),
                               _hexc(bg), None, round(_contrast(ink, bg), 2), b.get("px") or 0, False])
    ww, wh, mw, mh, title = d["win"]
    bl = [f"\u201c{t}\u201d" + (" highlighted" if h else "") + (" FOCUSED" if fo else "") + ("" if en else " disabled")
          for t, h, fo, _n, en in d["buttons"]]
    sc = "; ".join(f"{n} content {cw if cw >= 0 else 'auto'}x{ch} in {vw}x{vh}" for n, cw, ch, vw, vh in d["scroll"]) or "none"
    fo = d["focus"] or "none"
    fo = fo if len(fo) <= 60 else fo[:57] + "…"
    home = os.path.expanduser("~")
    inv = dict.fromkeys(map(ord, "\u200b\u200c\u200d\u2060\ufeff\u00ad"))  # (a path's break hints)
    clean = lambda t: t.translate(inv).replace(home, "~")  # (the fixture's absolute paths: no home directory in the facts)
    _fact(r, f"dlg-{state}-{r.s.suffix}", clean(f"{title or file} ({r.s.suffix.replace('_', ' ')}): window {ww}x{wh}"
          + (f" (minimum {mw}x{mh})" if mw or mh else "") + f"; focus as opened: {fo}; buttons: "
          + (", ".join(bl) or "none") + f"; scroll areas larger than their view: {sc}; contrast: texts "
          + ("; ".join(texts[:10]) or "none") + "; buttons " + ("; ".join(btns[:8]) or "none")
          + (f"; listed in the layout but not drawn there (no ink in their box: a list's kept delegate, covered or "
             f"culled), left out: {', '.join(dict.fromkeys(undrawn))}" if undrawn else "")),
          state=state, focus=clean(fo), buttons=d["buttons"], scroll=d["scroll"], win=d["win"],
          contrast=[[clean(str(v)) if isinstance(v, str) else v for v in row] for row in rows_c], undrawn=undrawn)


def m_queuegeom(r, arg):
    """queuegeom LABEL: the queue's rows measured as drawn, for FOCUS-FACTS' lead table (facts queuegeom-*): each
    fully shown row that is playing, selected, current (focus ring), hovered or unreadable (and, for a LABEL starting
    with "rest", the first plain row): its flags, x and right edge; each cell's x (queueCell_*); each cell's ink box
    from the pixels (off the row's own colour by more than 40 levels; x in the window, y from the row's top); the
    ring (rowBackgroundTint with a border) as an item and as pixels (uiPalette.focusRing within 60 levels); the
    scrollbar's lane (queueScrollBar) and thumb (laneScrollBarThumb), and the gaps from the ring to them."""
    import numpy as np
    js = ("(function(){var v=" + FIND_ANY + "('playlistView');var vp=v.mapToItem(null,0,0);var rows=[];"
          "function find(it,n){var s=[it];while(s.length){var x=s.shift();if(!x)continue;if(x.objectName===n)return x;"
          "var c=x.children;for(var i=0;i<c.length;i++)s.push(c[i]);}return null}"
          "function rc(x){var p=x.mapToItem(null,0,0);return [p.x,p.y,x.width,x.height]}"
          "var ch=v.contentItem.children;for(var i=0;i<ch.length;i++){var it=ch[i];"
          "if(!it||it.objectName!=='queueRow'||!it.visible)continue;var p=it.mapToItem(null,0,0);"
          "if(p.y<vp.y-0.5||p.y+it.height>vp.y+v.height+0.5)continue;var bg=null;var k=it.children;"
          "for(var j=0;j<k.length;j++)if(k[j].scrollbarLane!==undefined)bg=k[j];"
          "var tint=bg?find(bg,'rowBackgroundTint'):null;var cells={};"
          "['indicator','order','title','artist','album','length'].forEach(function(n){var c=find(it,'queueCell_'+n);"
          "if(c&&c.visible&&c.width>0)cells[n]=rc(c)});"
          "rows.push({i:it.queueRowIndex,r:rc(it),now:it.isNow,cur:it.isCurrentQueueRow,sel:it.rowSelected,"
          "ring:it.rowCurrent,hov:bg?bg.hovered:false,unr:it.unreadable,tint:tint&&tint.visible?rc(tint):null,"
          "bw:tint?tint.border.width:0,lane:bg?bg.scrollbarLane:0,cells:cells})}"
          "var sb=" + FIND_ANY + "('queueScrollBar');var th=sb?find(sb,'laneScrollBarThumb'):null;"
          "return JSON.stringify({rows:rows,active:v.activeFocus,state:root.uiBridge.playbackState,"
          "sb:sb&&sb.visible?rc(sb):null,thumb:th&&sb&&sb.visible?rc(th):null,ringc:String(root.uiPalette.focusRing)})})()")
    d = json.loads(r.s.ev(js))
    im = _grab_window(r, f"queuegeom-{arg}")
    a = np.asarray(im)
    sc = r.s.scale
    acc = d["ringc"]
    rgb = np.array([int(acc[i:i + 2], 16) for i in (1, 3, 5)], dtype=np.int16) if acc.startswith("#") and len(acc) == 7 else None
    flagged = [w for w in d["rows"] if w["cur"] or w["sel"] or w["ring"] or w["hov"] or w["unr"]]
    plain = [w for w in d["rows"] if w not in flagged][:1] if arg.startswith("rest") else []
    rows = sorted(flagged + plain, key=lambda w: w["i"])
    if not rows:
        raise LookupError(f"queuegeom {arg}: no row to measure on screen")
    sb, thumb = d["sb"], d["thumb"]
    span = lambda b: f"{b[0]:.0f}-{b[0] + b[2]:.0f}"
    for w in rows:
        x, y, rw, rh = w["r"]
        bg = np.array(_tint(im, [v * sc for v in (x, y, rw, rh)], inset=1), dtype=np.int16)
        texts, inks = [], []
        for n, (cx, cy, cw, chh) in w["cells"].items():
            texts.append(f"{n} {cx:.0f}")
            x0, x1 = int(round(max(cx, x) * sc)), int(round(min(cx + cw, x + rw) * sc))
            y0, y1 = int(round((y + 1) * sc)), int(round((y + rh - 1) * sc))
            if x1 <= x0 or y1 <= y0:
                continue
            m = (np.abs(a[y0:y1, x0:x1].astype(np.int16) - bg).sum(axis=2) > 40)
            if rgb is not None and w["ring"]:  # the ring's own pixels are not ink
                m &= ~((np.abs(a[y0:y1, x0:x1].astype(np.int16) - rgb).sum(axis=2) < 60))
            cols, rws = np.nonzero(m.any(axis=0))[0], np.nonzero(m.any(axis=1))[0]
            if len(cols):
                inks.append(f"{n} {(x0 + cols[0]) / sc:.0f}-{(x0 + cols[-1] + 1) / sc:.0f}/"
                            f"{(y0 + rws[0]) / sc - y:.0f}-{(y0 + rws[-1] + 1) / sc - y:.0f}")
        flags = [f for f, on in (("now", w["now"]), ("playing row", w["cur"] and not w["now"]), ("selected", w["sel"]),
                                 ("inactive", w["sel"] and not d["active"]), ("ring", w["ring"]), ("hovered", w["hov"]),
                                 ("unreadable", w["unr"])) if on] or ["plain"]
        ring_item = ring_px = "-"
        gap = "-"
        if w["ring"] and w["tint"] and w["bw"] > 0:
            tx, _, tw, _ = w["tint"]
            ring_item = f"{tx:.0f}-{tx + tw:.0f} (lane kept {w['lane']:.0f})"
            if rgb is not None:
                y0, y1 = int(round(y * sc)), int(round((y + rh) * sc))
                hit = np.nonzero((np.abs(a[y0:y1].astype(np.int16) - rgb).sum(axis=2) < 60).any(axis=0))[0]
                hit = hit[(hit >= x * sc) & (hit < (x + rw) * sc)]
                if len(hit):
                    ring_px = f"{hit[0] / sc:.0f}-{(hit[-1] + 1) / sc:.0f}"
                    if thumb:
                        gap = f"to the lane {sb[0] - (hit[-1] + 1) / sc:+.0f}, to the thumb {thumb[0] - (hit[-1] + 1) / sc:+.0f}"
        data = dict(state=arg, size=r.s.suffix.replace("_", " "), row=w["i"] + 1, flags=", ".join(flags),
                    row_x=span(w["r"]), text_x=", ".join(texts), ink="; ".join(inks), ring_item=ring_item,
                    ring_px=ring_px, lane=span(sb) if sb else "none (no scrollbar)", thumb=span(thumb) if thumb else "-",
                    gap=gap)
        _fact(r, f"queuegeom-{r.s.suffix}-{arg}-{w['i']:03d}", f"Queue row {w['i'] + 1} ({arg}): " + "; ".join(
            f"{k} {v}" for k, v in data.items() if k not in ("state", "row")), **data)


def m_unreadable(r, arg):
    """unreadable: a 4-row queue with a real unreadable row: the album "Loose Ends" (3 tracks) and a file that is
    not audio (FLAC by its name, random bytes inside), written to the run's own TMPDIR; playing it fails, and the
    playback error marks its row (warning glyph, dimmed text). Loud when the row is not marked within 15 s."""
    import random
    d = os.path.join(os.environ["TMPDIR"], "ferrous-unreadable")
    os.makedirs(d, exist_ok=True)
    bad = os.path.join(d, "Paper Lantern.flac")
    rnd = random.Random(35)
    put_fixture(bad, b"fLaC" + bytes(rnd.getrandbits(8) for _ in range(48 * 1024)))
    paths = album_paths("Orchid Static/2019 - Loose Ends") + [bad]
    r.s.ev(f"root.uiBridge.replaceWithPaths({json.dumps(paths)})")
    if not r.s.a.wait_for(f"root.uiBridge.queueLength === {len(paths)}", timeout=15, interval=0.05):
        raise RuntimeError("unreadable: the 4-row queue did not load")
    r.s.ev(f"root.uiBridge.playAt({len(paths) - 1})")
    if not r.s.a.wait_for(f"errorController.unreadablePaths[{json.dumps(bad)}] !== undefined", timeout=15, interval=0.1):
        raise RuntimeError("unreadable: playing a non-audio file marked no row unreadable within 15 s")
    time.sleep(0.5)
    v = r.s.ev(f"JSON.stringify([root.uiBridge.playbackState,root.uiBridge.playingQueueIndex,"
               f"errorController.unreadablePaths[{json.dumps(bad)}]])")
    st, pi, msg = json.loads(v)
    _fact(r, f"queue-unreadable-{r.s.tag}", f"Unreadable row: a non-audio file named .flac queued 4th of 4 and played; "
          f"the app's message \u201c{msg}\u201d; then playback {st}, playing row index {pi} ({r.s.suffix.replace('_', ' ')})")


def m_statusjumps(r, arg):
    """statusjumps: the status bar's items while numbers change: the scan count injected at 7, 57, 157, 1157 and
    11157 (of 180 to 18000); each visible item's x and width per step, and how far it moves; fact status-jumps."""
    items = ["statusBarPlaybackState", "statusBarRepeatShuffle", "statusBarFormat", "statusBarScan", "statusBarScanLabel",
             "statusBarScanCount", "statusBarScanBar", "statusBarErrorsButton", "scrobbleIndicator"]
    seen = {}
    for proc, total in ((7, 180), (57, 180), (157, 180), (1157, 1800), (11157, 18000)):
        m_fakebridge(r, 'statusBar {"libraryScanInProgress":true,"libraryScanProcessed":%d,"libraryScanTotal":%d}' % (proc, total))
        time.sleep(0.35)
        for nm in items:
            rs = r.s.visible_rects(nm)
            if rs:
                _, x, y, w, h = rs[0]
                seen.setdefault(nm, []).append((proc, round(x), round(w)))
    m_fakebridge(r, "statusBar reset")
    parts = []
    for nm, v in seen.items():
        xs, ws = [a[1] for a in v], [a[2] for a in v]
        parts.append(f"{nm}: x {min(xs)}-{max(xs)} (moves {max(xs) - min(xs)} px), width {min(ws)}-{max(ws)}")
    _fact(r, f"status-jumps-{r.s.tag}", "Status bar items while the scan count grows (7 -> 11157, injected): " + "; ".join(parts)
          + f" ({r.s.suffix.replace('_', ' ')})")


def m_titlelines(r, arg):
    """titlelines CASE: the now-playing title as drawn: its lines (lineCount), whether it is elided (truncated)
    and its text; fact card-title-CASE."""
    v = r.s.ev("(function(){var s=[" + FIND + "('nowPlayingPanel')];while(s.length){var it=s.shift();if(!it||!it.visible)continue;"
               "if(/^nowPlaying(Narrow)?Title$/.test(it.objectName)&&it.opacity>0.5&&it.text!==undefined)"
               "return JSON.stringify([it.objectName,String(it.text),it.lineCount,it.truncated,it.width,it.height]);"
               "var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}return ''})()")
    if not v:
        raise LookupError(f"titlelines {arg}: no visible now-playing title")
    nm, text, lines, trunc, w, h = json.loads(v)
    txt = (f"title “{text}”: {lines} line{'s' if lines != 1 else ''}, {'elided' if trunc else 'not elided'} "
           f"({nm}, {w:.0f}x{h:.0f} px; {r.s.suffix.replace('_', ' ')})")
    _fact(r, f"card-title-{arg}", f"Now-playing {arg} case: {txt}", lines=lines, elided=bool(trunc))
    r.line_desc = (r.line_desc or "") + f" [{txt}]"


def m_cardgeom(r, arg):
    """cardgeom CASE: the now-playing card's geometry for one metadata case: the panel's and the wide text
    block's rects, and the library header's y (expected the same in every case)."""
    def rect_of(pattern):
        v = r.s.ev(f"(function(){{var it={FIND}('{pattern}');if(!it||!it.visible||it.opacity<0.5||it.width<=0||it.height<=0)"
                   "return '';var p=it.mapToItem(null,0,0);return JSON.stringify([p.x,p.y,it.width,it.height])})()")
        return json.loads(v) if v else None
    panel = rect_of("nowPlayingPanel")
    # the wide text block, or the large-font / narrow one; and, where the title sits beside the cover, the top of
    # the artist line (the full-width lines start there; it replaces the removed nowPlayingHeaderFoot)
    text, narrow = rect_of("nowPlayingWideText"), rect_of("nowPlayingNarrowText")
    foot = rect_of("nowPlayingNarrowArtist")
    cover = rect_of("nowPlayingCoverArea")
    hdr = r.s.ev("(function(){var s=[" + FIND + "('libraryPane')];while(s.length){var it=s.shift();if(!it)continue;"
                 "if(it.objectName==='regionHeaderTitle'){var p=it.mapToItem(null,0,0);return JSON.stringify([p.x,p.y,it.width,it.height])}"
                 "var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}return ''})()")
    hdr = json.loads(hdr) if hdr else None
    det = r.s.ev("(function(){var o=[];var s=[" + FIND + "('nowPlayingPanel')];while(s.length){var it=s.shift();if(!it||!it.visible)continue;"
                 "if(/^nowPlaying(Narrow)?Details$/.test(it.objectName)&&it.opacity>0.5&&it.text)o.push(String(it.text));"
                 "var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}return o.join(' | ')})()") or ""
    if not panel or not hdr:
        raise LookupError(f"cardgeom: panel {panel}, library header {hdr}")
    fmt = lambda q: f"y {q[1]:.0f} h {q[3]:.0f}" if q else "none"
    txt = (f"{arg}: now playing panel {fmt(panel)}, "
           + (f"wide text block {fmt(text)}, " if text else "")
           + (f"narrow text block {fmt(narrow)}, " if narrow else "")
           + (f"cover {cover[2]:.0f}x{cover[3]:.0f} at y {cover[1]:.0f}, " if cover else "")
           + (f"artist line top y {foot[1]:.0f} ({foot[1] - panel[1]:.0f} px into the panel), " if foot and panel else "")
           + f"library header title y {hdr[1]:.0f}, "
           f"details line \u201c{det}\u201d "
           f"({r.s.suffix.replace('_', ' ')})")
    _fact(r, f"card-{arg}", f"Card geometry, {txt}", panel=panel, text_block=text or narrow, header=hdr,
          artist_top=foot[1] if foot else None)
    r.line_desc = (r.line_desc or "") + f" [{txt}]"


def m_infoedge(r, arg):
    """infoedge: the windowed album art viewer with its info shown: the info toggle's checked edge
    (viewerToolButtonCheckedEdge) sampled, and the toggle at 1:1 and 4x as details/info-toggle; the viewer closed again."""
    from PIL import Image, ImageDraw, ImageFont
    was = r.s.ev("JSON.stringify([root.uiBridge.viewerFullscreenMode, viewerController.albumArtInfoVisible])")
    mode0, info0 = json.loads(was)
    r.s.ev("(function(){root.uiBridge.setViewerFullscreenMode(0);viewerController.openAlbumArtViewer();return true})()")
    time.sleep(0.6)
    r.s.ev("(function(){if(!viewerController.albumArtInfoVisible)viewerController.toggleAlbumArtInfoVisible(null);return true})()")
    end = time.time() + 4.0
    while not r.s.visible_rects("viewerToolButtonCheckedEdge"):
        if time.time() > end:
            raise LookupError("infoedge: no viewerToolButtonCheckedEdge shown with the info on")
        time.sleep(0.1)
    r.s.settle(timeout=1.5)
    im = _grab_window(r, "infoedge")
    rect = r.s.visible_rects("viewerToolButtonCheckedEdge")[0][1:]
    m = _edge_facts(im, rect, r.s.scale)
    r.s.ev(f"(function(){{if(viewerController.albumArtInfoVisible!=={'true' if info0 else 'false'})"
           f"viewerController.toggleAlbumArtInfoVisible(null);"
           f"viewerController.closeAlbumArtViewer();root.uiBridge.setViewerFullscreenMode({int(mode0)});return true}})()")
    time.sleep(0.4)
    sc = r.s.scale
    c = im.crop(tuple(int(round(v * sc)) for v in (rect[0] - 4, rect[1] - 4, rect[0] + rect[2] + 4, rect[1] + rect[3] + 4)))
    txt = (f"edge {m['edge']}, fill {m['fill']}, around {m['around']}: edge {m['edge_vs_fill']}:1 to the fill, "
           f"{m['edge_vs_around']}:1 to the surround")
    S = Image.new("RGB", (max(c.width * 5 + 30, 520), c.height * 4 + 64), (78, 82, 88))
    d = ImageDraw.Draw(S)
    d.text((10, 4), f"Album art viewer: the info toggle, checked, 1:1 and 4x ({r.s.tag})", fill=(255, 220, 120),
           font=ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 14))
    S.paste(c, (10, 26))
    S.paste(c.resize((c.width * 4, c.height * 4), Image.NEAREST), (20 + c.width, 26))
    d.text((10, 30 + c.height * 4), txt, fill=(230, 230, 230), font=ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 12))
    _save_shot(r, S, "details", "info-toggle", f"The album art viewer's info toggle, checked, 1:1 and 4x: {txt}")
    _fact(r, "info-toggle-edge", f"Art viewer info toggle, checked ({r.s.tag}): {txt}", **m)


LIB_LARGE = os.path.join(os.path.dirname(LIB), "library-large")
_NA_TRACK = sorted(glob.glob(os.path.join(LIB, "Orchid Static", "2017 - Static Bloom", "01 - *.flac")) or ["01 - missing.flac"])[0]


def m_largelib(r, arg):
    """The large invented library (library density): added as the session's only root, until scanned."""
    cnt = os.path.join(LIB_LARGE, ".count")
    if not os.path.exists(cnt):
        raise FileNotFoundError(f"largelib: no large library at {LIB_LARGE} (make_large_library.py)")
    n = int(open(cnt).read())
    if arg != "wait":  # ("wait": the root was added already, by the scan-large recording)
        r.s.ev(f"root.uiBridge.addLibraryRoot({json.dumps(LIB_LARGE)}, 'Large Shelf')")
    if not r.s.a.wait_for(f"root.uiBridge.libraryTrackCount >= {n} && !root.uiBridge.libraryScanInProgress",
                          timeout=90, interval=0.2):
        raise RuntimeError(f"largelib: the scan did not reach {n} tracks")
    time.sleep(0.8)


def m_glyphline(r, arg):
    """the status bar's right part at 1:1 with a 1 px line through the Last.fm glyph's centre, and the glyph's
    neighbourhood at 4x with the line at its exact place; the glyph's centre against the bar's and the indicator
    label's (when shown), in the caption."""
    from PIL import Image, ImageDraw, ImageFont
    im = _grab_window(r, "glyphline")
    sb = r.s.rect("statusBar")
    gl = r.s.visible_rects("scrobbleIndicatorGlyph")
    if not sb or not gl:
        raise LookupError("glyphline: no statusBar or visible scrobbleIndicatorGlyph")
    _, gx, gy, gw, gh = gl[0]
    lab = r.s.visible_rects("scrobbleIndicatorLabel")
    sc = r.s.scale
    cy = gy + gh / 2
    x0 = max(0, int(sb[0] + sb[2] - 1000))
    y0, y1 = int(sb[1] - 1), int(sb[1] + sb[3])  # the bar and its top edge (not the plot above it)
    c = im.crop((int(x0 * sc), int(y0 * sc), int((sb[0] + sb[2]) * sc), int(y1 * sc)))
    d = ImageDraw.Draw(c)
    row = int((cy - y0) * sc - 0.5)  # the pixel row the centre falls in (or just above, when it is on a boundary)
    d.line((0, row, c.width, row), fill=(255, 0, 200))
    zx1 = int((min(sb[0] + sb[2], gx + gw + 60) - x0) * sc)  # within the window
    zx0 = max(0, zx1 - int((gw + 120) * sc))
    z = im.crop((int(x0 * sc) + zx0, int(y0 * sc), int(x0 * sc) + zx1, int(y1 * sc))).resize(((zx1 - zx0) * 4, (y1 - y0) * int(4 * sc)), Image.NEAREST)
    dz = ImageDraw.Draw(z)
    zy = round((cy - y0) * sc * 4)
    dz.line((0, zy, z.width, zy), fill=(255, 0, 200))
    notes = [f"glyph centre y {cy:.1f}; status bar centre y {sb[1] + sb[3] / 2:.1f} ({cy - (sb[1] + sb[3] / 2):+.1f} px)"]
    if lab:
        _, lx, ly, lw, lh = lab[0]
        notes.append(f"its label's box centre y {ly + lh / 2:.1f} ({cy - (ly + lh / 2):+.1f} px)")
    f = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 14)
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 15)
    W = max(c.width, z.width) + 28
    S = Image.new("RGB", (W, 30 + c.height + 14 + 22 + z.height + 30), (78, 82, 88))
    dd = ImageDraw.Draw(S)
    dd.text((14, 6), f"Status bar's right {c.width} px at 1:1, a magenta line through the Last.fm glyph's centre ({r.s.tag})",
            fill=(255, 220, 120), font=fb)
    S.paste(c, (14, 30))
    dd.text((14, 30 + c.height + 10), "The glyph at 4x, the line at the centre's exact place: " + "; ".join(notes),
            fill=(230, 230, 230), font=f)
    S.paste(z, (14, 30 + c.height + 34))
    _save_shot(r, S, "details", "statusbar-glyph", f"Status bar 1:1 with a line through the Last.fm glyph's centre: {'; '.join(notes)}")


def m_searchfield(r, arg):
    """the search palette's field (glyph, query, buttons, its seam) at 1:1, for the 24 pt session."""
    im = _grab_window(r, "searchfield")
    parts = [v for n in ("globalSearchFieldIcon", "globalSearchQueryField", "globalSearchHelpButton", "globalSearchFieldSeam")
             for v in r.s.visible_rects(n)[:1]]
    if len(parts) < 3:
        raise LookupError(f"searchfield: the search field's parts are not shown ({len(parts)} of 4)")
    x0 = min(v[1] for v in parts) - 2; y0 = min(v[2] for v in parts) - 2
    x1 = max(v[1] + v[3] for v in parts) + 2; y1 = max(v[2] + v[4] for v in parts) + 2
    sc = r.s.scale
    c = im.crop((int(x0 * sc), int(y0 * sc), int(x1 * sc), int(y1 * sc)))
    _save_shot(r, c, "details", f"searchfield{arg}", f"The search palette's field at 1:1 ({arg or 'this'} font), its query typed")


def m_treekeys(r, arg):
    """The library tree's keys of the playing track (uiBridge.currentTrackTreeKeys: what marks its artist, album and
    row as now), into the caption and treekeys_<variant>.txt; none while a track plays fails the state (with a
    stale template DB the playing path matched no library track, and nothing was marked)."""
    k = r.s.ev("JSON.stringify([root.uiBridge.playbackState, root.uiBridge.currentTrackTreeKeys])") or "[]"
    st_, keys = json.loads(k) if k.startswith("[") else (None, None)
    with open(os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), f"treekeys_{r.s.suffix}.txt"), "a") as f:
        f.write(f"{r.line_state}: {st_} {keys}\n")
    if str(st_).lower() in ("playing", "paused") and not keys:
        raise RuntimeError(f"treekeys: a track is {st_} but the library tree has no keys for it (not in the library?)")
    lv = r.s.ev(f"(function(){{var v={FIND}('libraryView');return v?JSON.stringify([v.contentY,v.contentHeight,v.height,v.count]):''}})()")
    with open(os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), f"treekeys_{r.s.suffix}.txt"), "a") as f:
        f.write(f"   libraryView contentY, contentHeight, height, rows: {lv}\n")
    r.line_desc = (r.line_desc or "") + f" [tree keys: {len(keys or [])}]"


# ---- intent checks (runner step "expect NAME[:ARG]"): each evaluates to true, or to the reason it is not.
_ALL = ("(function(n){var o=[];var s=[root.Overlay.overlay.parent];while(s.length){var it=s.shift();if(!it.visible)continue;"
        "if(it.objectName===n)o.push(it);var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}return o})")
# (the rows drawn in the library view: a delegate outside its viewport, kept by the list or leaving it, is not shown)
_ROWS = (f"var lv={_ALL}('libraryView')[0];var lp=lv.mapToItem(null,0,0);var rows={_ALL}('libraryRow').filter(function(r){{"
         "var p=r.mapToItem(null,0,0);var cx=p.x+r.width/2,cy=p.y+r.height/2;"
         "if(!(r.height>1&&r.opacity>0.5&&cy>=lp.y&&cy<=lp.y+lv.height))return false;"
         # inside every clipping ancestor (a folded-away group's rows stay in the tree, clipped to nothing)
         "for(var a=r.parent;a;a=a.parent){if(a.clip){var q=a.mapToItem(null,0,0);"
         "if(cx<q.x||cy<q.y||cx>q.x+a.width||cy>q.y+a.height)return false}}return true});")
_ST = "var st=String(root.uiBridge.playbackState);"
EXPECT = {
    # the library tree opened to the playing track: an expanded row, and the playing track's own row as now
    "now-row": f"(function(){{{_ROWS}if(!(root.uiBridge.libraryExpandedKeys||[]).length)return 'no expanded row';"
               "var nw=rows.filter(function(r){return r.isNow});if('{arg}'==='centred'&&nw.length){var q=nw[0].mapToItem(null,0,0);"
               "var c=q.y+nw[0].height/2-lp.y;if(Math.abs(c-lv.height/2)>lv.height/4)return 'the playing row is not centred: its "
               "centre at '+Math.round(c)+' of '+Math.round(lv.height)+' px'}"
               "if(!rows.some(function(r){return r.isNow}))return 'no row is the playing track (isNow)';return true})()",
    # the tree marks where the playing track is: its own row, or a collapsed row that holds it
    "holds-now": f"(function(){{{_ROWS}return rows.some(function(r){{return r.holdsNow||r.isNow}})||'no row holds the playing track'}})()",
    # (not by the rows' isExpanded: a pooled delegate stays in the scene, culled, with stale properties)
    "collapsed-holds-now": f"(function(){{{_ROWS}var ek=root.uiBridge.libraryExpandedKeys||[];"
                           "if(ek.length)return 'expanded keys: '+ek.join(', ');"
                           "return rows.some(function(r){return r.holdsNow})||'no collapsed row holds the playing track'})()",
    "waveform-meter": f"(function(){{{_ST}if(viewerController.visualizationMode!==1)return 'not the waveform';"
                      f"if(!{_ALL}('levelMeter').length)return 'no level meter shown';return st==='Playing'||'not playing: '+st}})()",
    "spectrogram-meter": f"(function(){{if(viewerController.visualizationMode!==0)return 'not the spectrogram';"
                         f"return {_ALL}('levelMeter').length>0||'no level meter shown'}})()",
    "no-cover": f"(function(){{{_ST}if(root.uiBridge.currentTrackCoverPath!=='')return 'the track has a cover';"
                "return st==='Playing'||'not playing: '+st})()",
    # the playing track is the named file (a path fragment) and has no cover: the stand-in states
    "track-no-cover": f"(function(){{{_ST}if(root.uiBridge.currentTrackPath.indexOf('{{arg}}')<0)return 'playing '+root.uiBridge.currentTrackPath;"
                      "if(root.uiBridge.currentTrackCoverPath!=='')return 'the track has a cover';return st==='Playing'||'not playing: '+st})()",
    "search-empty": "(function(){var f=globalSearchController.queryField;if(!f||!f.visible)return 'the palette is not open';"
                    "return f.text===''||'the query is not empty: '+f.text})()",
    "search-results": "(function(){return globalSearchController.rowCount()>0||'no result rows'})()",
    "first-run": f"(function(){{if(root.uiBridge.libraryTrackCount!==0)return 'the library is not empty';"
                 f"return {_ALL}('firstRunAddFolderButton').length>0||'no Add Library Folder button'}})()",
    "idle": f"(function(){{{_ST}if(root.uiBridge.queueLength!==0)return 'the queue is not empty';"
            f"if(!{_ALL}('nowPlayingIdleRow').length)return 'no idle row';return st==='Stopped'||'not stopped: '+st}})()",
    "lanes": f"(function(){{var n={_ALL}('plotLaneHead').length;return n==={{arg}}||'lanes shown: '+n}})()",
    "look": f"(function(){{var i={_ALL}('scrobbleIndicator');if(!i.length)return 'no Last.fm indicator';"
            "return i[0].look==='{arg}'||'its look is '+i[0].look})()",
    "viewer-spectrogram": "(function(){return viewerController.spectrogramViewerOpen===true||'the spectrogram viewer is not open'})()",
    "viewer-albumart": "(function(){return viewerController.albumArtViewerOpen===true||'the album art viewer is not open'})()",
    "playing": f"(function(){{{_ST}return st==='Playing'||'not playing: '+st}})()",
}


def m_kwinprobe(r, arg):
    """Diagnostic only (feasibility of capturing the compositor's window animations; not part of a round:
    SHOT_KWINPROBE=1 adds its state): which KWin effects are loaded in the virtual session, and how fast the
    compositor's own screenshots of the workspace come while a dialog window opens. Writes kwinprobe_<variant>.txt."""
    import dbus
    out = []
    bus = r.s._kwin_bus()
    try:
        fx = dbus.Interface(bus.get_object("org.kde.KWin", "/Effects"), "org.freedesktop.DBus.Properties")
        out.append("loaded effects: " + ", ".join(str(x) for x in fx.Get("org.kde.kwin.Effects", "loadedEffects")))
        out.append("active effects now: " + ", ".join(str(x) for x in fx.Get("org.kde.kwin.Effects", "activeEffects")))
    except Exception as e:
        out.append(f"effects query failed: {e}")
    try:
        comp = dbus.Interface(bus.get_object("org.kde.KWin", "/Compositor"), "org.freedesktop.DBus.Properties")
        out.append("compositing: " + ", ".join(f"{k}={comp.Get('org.kde.kwin.Compositing', k)}" for k in
                                               ("active", "compositingType", "platformRequiresCompositing")))
    except Exception as e:
        out.append(f"compositor query failed: {e}")
    shot = dbus.Interface(bus.get_object("org.kde.KWin", "/org/kde/KWin/ScreenShot2"), "org.kde.KWin.ScreenShot2")
    raw = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "kwinprobe-raw")
    os.makedirs(raw, exist_ok=True)
    r.s.ev("(function(){aboutDialog.show();return true})()")  # a dialog window opens: KWin animates it, if it does
    t0, stamps, sizes = time.time(), [], []
    for i in range(24):
        rfd, wfd = os.pipe()
        res = shot.CaptureWorkspace({"native-resolution": True}, dbus.types.UnixFd(wfd))
        os.close(wfd)
        data = b""
        while True:
            c = os.read(rfd, 1 << 20)
            if not c:
                break
            data += c
        os.close(rfd)
        stamps.append(time.time() - t0)
        sizes.append((int(res["width"]), int(res["height"])))
        if i in (0, 2, 4, 6, 8, 12, 23):
            from PIL import Image
            Image.frombuffer("RGBA", sizes[-1], data, "raw", "RGBA", int(res["stride"]), 1).save(
                os.path.join(raw, f"ws_{r.s.suffix}_{i:02d}_{int(stamps[-1] * 1000)}ms.png"))
    gaps = [b_ - a_ for a_, b_ in zip(stamps, stamps[1:])]
    out.append(f"CaptureWorkspace x24 while About opens: {sizes[0][0]}x{sizes[0][1]}, first at {stamps[0] * 1000:.0f} ms, "
               f"median gap {sorted(gaps)[len(gaps) // 2] * 1000:.0f} ms, max {max(gaps) * 1000:.0f} ms")
    r.s.ev("(function(){aboutDialog.close();return true})()")
    with open(os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), f"kwinprobe_{r.s.suffix}.txt"), "w") as f:
        f.write("\n".join(out) + "\n")


def _fact(r, key, text, **data):
    """a measured fact for CORE-FACTS.md (pack.write_core_facts), one JSON line per theme in facts.jsonl."""
    p = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "facts.jsonl")
    with open(p, "a") as f:
        f.write(json.dumps(dict(key=key, theme=r.s.tag, suffix=r.s.suffix, text=text, **data)) + "\n")


def _hexc(c):
    return "#%02x%02x%02x" % tuple(c[:3])


def _lum(c):
    v = [x / 255 for x in c[:3]]
    v = [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in v]
    return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2]


def m_platehover(r, arg):
    """the mode switch's plate with the pointer on its unchecked segment, at 3x; any pixel of the accent colour
    (the focus ring's, Kirigami highlightColor) fails the state: without keyboard focus the plate has no blue.
    the pointer goes to whichever segment is unchecked (read from the buttons), and the checked segment's fill,
    the hovered unchecked segment's fill and the plate's own fill are sampled: their luminance and contrast ratios
    go in the image, its description and facts.jsonl."""
    from PIL import Image, ImageDraw, ImageFont
    import numpy as np
    btns = []
    for i in range(4):
        rr = r.s.rect(f"visualizationModeButton{i}")
        if not rr:
            break
        btns.append((i, rr, r.s.ev(f"String({FIND}('visualizationModeButton{i}').checked)") == "true"))
    checked = [b for b in btns if b[2]]
    unchecked = [b for b in btns if not b[2]]
    if len(checked) != 1 or not unchecked:
        raise LookupError(f"platehover: want one checked and an unchecked segment, have {[(b[0], b[2]) for b in btns]}")
    hb = unchecked[0]
    r.s.move(hb[1][0] + hb[1][2] / 2, hb[1][1] + hb[1][3] / 2)
    time.sleep(0.5)  # hover tints fade in 100 ms
    if r.s.ev(f"String({FIND}('visualizationModeButton{hb[0]}').hovered)") != "true":
        raise RuntimeError(f"platehover: visualizationModeButton{hb[0]} is not hovered after the move")
    im = _grab_window(r, "platehover")
    rs = r.s.visible_rects("visualizationModeSwitch")
    if not rs:
        raise LookupError("platehover: no visualizationModeSwitch")
    _, x, y, w, h = rs[0]
    sc = r.s.scale
    c = im.crop((int((x - 6) * sc), int((y - 6) * sc), int((x + w + 6) * sc), int((y + h + 6) * sc)))
    acc = r.s.ev("String(root.uiPalette.accent)") or ""
    rgb = tuple(int(acc[i:i + 2], 16) for i in (1, 3, 5)) if acc.startswith("#") and len(acc) == 7 else None
    if not rgb:
        raise RuntimeError(f"platehover: no accent colour ({acc})")
    n = int((np.abs(np.asarray(c).astype(int) - np.array(rgb)).sum(axis=2) < 60).sum())
    focus = r.s.ev("(function(){var f=root.activeFocusItem;return f?f.objectName:''})()")
    # the fills: each segment's most frequent colour (its glyph and label aside), the plate's in the gaps
    # between and around the segments (the switch's rect less the segments'), and the pane around the plate
    sc_box = lambda b: [v * sc for v in b]
    ck = _tint(im, sc_box(checked[0][1]), inset=int(3 * sc))
    hv = _tint(im, sc_box(hb[1]), inset=int(3 * sc))
    a = np.asarray(im)
    mask = np.zeros(a.shape[:2], bool)
    X, Y, W, H = [int(round(v * sc)) for v in (x, y, w, h)]
    mask[Y:Y + H, X:X + W] = True
    for b in btns:
        bx, by, bw, bh = [int(round(v * sc)) for v in b[1]]
        mask[by:by + bh, bx:bx + bw] = False
    from collections import Counter
    px = [tuple(v) for v in a[mask]]
    plate = Counter(px).most_common(1)[0][0] if px else None
    ring = np.zeros(a.shape[:2], bool)
    ring[max(0, Y - int(4 * sc)):Y + H + int(4 * sc), max(0, X - int(4 * sc)):X + W + int(4 * sc)] = True
    ring[Y:Y + H, X:X + W] = False
    pane = Counter(tuple(v) for v in a[ring]).most_common(1)[0][0]
    rows = [("checked segment", ck), ("hovered unchecked segment", hv)] + ([("plate (around the segments)", plate)] if plate else []) \
        + [("pane behind the plate", pane)]
    lines = [f"{k} {_hexc(v)} L={_lum(v):.3f}" for k, v in rows]
    ratios = [f"checked vs hovered {_contrast(ck, hv):.2f}:1", f"hovered vs pane {_contrast(hv, pane):.2f}:1",
              f"checked vs pane {_contrast(ck, pane):.2f}:1"]
    order = (f"the hovered segment is {'lighter' if _lum(hv) > _lum(ck) else 'darker'} than the checked one "
             f"(L {_lum(hv):.3f} vs {_lum(ck):.3f})")
    z = c.resize((c.width * 3, c.height * 3), Image.NEAREST)
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 15)
    fr = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 13)
    text = [f"accent {acc} pixels: {n}"] + lines + ["; ".join(ratios)]
    S = Image.new("RGB", (max(z.width, 420) + 28, z.height + 30 + 18 * len(text)), (78, 82, 88))
    d = ImageDraw.Draw(S)
    d.text((14, 6), f"Unchecked segment hovered, no focus, 3x ({r.s.tag})", fill=(255, 220, 120), font=fb)
    for k, t in enumerate(text):
        d.text((14, 26 + 18 * k), t, fill=(230, 230, 230), font=fr)
    S.paste(z, (14, 30 + 18 * len(text)))
    summary = f"{'; '.join(lines)}; {'; '.join(ratios)}: {order}"
    _save_shot(r, S, "details", "modeswitch-hover",
               f"The mode switch, its unchecked segment (visualizationModeButton{hb[0]}) hovered without focus, 3x: "
               f"{n} accent pixels; {summary}")
    _fact(r, "modeswitch-plates", f"Mode switch ({r.s.tag}): {summary}; accent pixels {n}",
          checked=_hexc(ck), hovered=_hexc(hv), plate=_hexc(plate) if plate else None, pane=_hexc(pane),
          ratio_checked_hovered=round(_contrast(ck, hv), 2))
    if n > 2 and not str(focus).startswith("visualizationModeButton"):
        raise RuntimeError(f"platehover: {n} pixels of the accent {acc} on the hovered plate without keyboard focus")


def m_artqueue(r, arg):
    """A queue of two tracks: one with album art (The Quiet Meridian, Small Hours) then one without (Orchid
    Static, Static Bloom: no cover.jpg, no embedded art); plays the first, until its cover is loaded."""
    paths = []
    for pat in ("The Quiet Meridian/2005 - Small Hours/01 - *.flac", "Orchid Static/2017 - Static Bloom/01 - *.flac"):
        g = sorted(glob.glob(os.path.join(LIB, pat)))
        if not g:
            raise FileNotFoundError(f"artqueue: no {pat} in {LIB}")
        paths.append(g[0])
    if os.path.exists(os.path.join(os.path.dirname(paths[1]), "cover.jpg")):
        raise RuntimeError("artqueue: the no-art album has a cover.jpg")
    r.s.ev(f"root.uiBridge.replaceWithPaths({json.dumps(paths)})")
    if not r.s.a.wait_for("root.uiBridge.queueLength === 2", timeout=10, interval=0.05):
        raise RuntimeError("artqueue: the queue did not take the two tracks")
    r.s.ev("root.uiBridge.playAt(0)")
    if not r.s.a.wait_for("root.uiBridge.playbackState.toLowerCase() === 'playing' && "
                          "root.uiBridge.currentTrackCoverPath !== ''", timeout=15, interval=0.05):
        raise RuntimeError("artqueue: the first track is not playing with its cover")
    time.sleep(1.0)


def m_launch_focus(r, arg):
    """The item with keyboard focus right after launch (nothing clicked or typed yet), in the state's caption and
    in focus_owners_<variant>.txt."""
    o = r.s.focus_owner()
    _a11y_row(r, "Launch")
    r.line_desc = f"Right after launch, nothing clicked or typed: keyboard focus on {o}"
    p = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), f"focus_owners_{r.s.suffix}.txt")
    with open(p, "w") as f:
        f.write(f"Launch: {o}\n")
    r.s.launch_logged = True


def m_icons(r, arg):
    """Renders the app icon's per-size SVGs with Qt's SVG plugin, on dark and light grounds."""
    r.s.draws_without_app_pixels()  # (drawn from the icon's SVGs)
    if r.s.tag != "dark":  # (the icons session runs both themes; this makes both grounds itself, once)
        return
    from PIL import Image, ImageDraw, ImageFont
    src = os.environ.get("SHOT_SRC", "")
    out = os.environ["SHOT_OUT"]
    raw = os.path.join(os.path.dirname(out.rstrip("/")), "icons-raw")
    os.makedirs(raw, exist_ok=True)
    sizes = [16, 22, 32, 48, 64, 128, 256]
    files = {}
    for px in sizes:
        svg = os.path.join(src, f"ui/assets/icons/hicolor/{px}x{px}/apps/ferrous.svg")
        if not os.path.exists(svg):
            svg = os.path.join(src, "ui/assets/icons/hicolor/scalable/apps/ferrous.svg")
        o = os.path.join(raw, f"ferrous-{px}.png")
        print("  icon", px, r.s.cmd(f"svg {svg} {px} {o}"))
        files[px] = o
    font = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 13)
    for tag, bg, fg in (("dark", (32, 35, 38), (220, 220, 220)), ("light", (239, 240, 241), (35, 38, 41))):
        for zoom in (1, 4):
            W = sum(p * zoom + 40 for p in sizes) + 40
            H = 256 * zoom + 90
            im = Image.new("RGB", (W, H), bg)
            d = ImageDraw.Draw(im)
            x = 40
            for px in sizes:
                ic = Image.open(files[px]).convert("RGBA")
                if zoom != 1:
                    ic = ic.resize((px * zoom, px * zoom), Image.NEAREST)
                im.paste(ic, (x, 40 + (256 * zoom - px * zoom)), ic)
                d.text((x, H - 40), f"{px} px", fill=fg, font=font)
                x += px * zoom + 40
            name = f"icon_app-icon{'-zoom4' if zoom != 1 else ''}_{tag}"
            if zoom == 4 and im.width > 4000:
                continue
            im.save(os.path.join(out, name + ".png"))
            r.s.index.append(dict(file=name + ".png", area="icon", state="app-icon" + ("-zoom4" if zoom != 1 else ""),
                                  theme=tag, scale=1, size=f"{im.width}x{im.height}", renderer=r.s.renderer,
                                  **r.s.composite_evidence(name + ".png"),
                                  desc=f"App icon per-size SVGs (16-256 px) rendered by Qt's SVG plugin on a Breeze "
                                       f"{tag} window colour{' , 4x nearest-neighbour zoom' if zoom != 1 else ''}"))


def m_fakebridge(r, arg):
    """fakebridge OBJECTNAME {json}|reset: the item's uiBridge becomes the real one with these fields set."""
    name, _, over = arg.partition(" ")
    if over.strip() == "reset":
        js = f"(function(){{var it={FIND_ANY}('{name}');it.uiBridge=root.uiBridge;return true}})()"
    else:
        js = (f"(function(o){{var it={FIND_ANY}('{name}');var p=new Proxy(root.uiBridge,{{get:function(t,k){{"
              f"if(k in o)return o[k];var v=t[k];return typeof v==='function'?v.bind(t):v}}}});it.uiBridge=p;return true}})({over})")
    if not r.s.ev(js):
        raise LookupError(f"no {name}")
    time.sleep(0.15)


# An item's top left in the main window's coordinates, which its grab uses: an item in a menu (a window of its
# own, which the grab draws in at its place) is offset by that window's place.
MAIN_POINT_JS = ("(function(it){var p=it.mapToItem(null,0,0);var w=it.Window.window;"
                 "return (w&&w!==root)?Qt.point(p.x+w.x-root.x,p.y+w.y-root.y):p})")

MEASURE_JS = ("(function(){{var r=({expr});if(!r)return '';var it=r._m?r.item:r;var ref=r._m?(r.ref||null):null;"
              "if(r._m&&('color' in r)&&(r.color===undefined||r.color===null))return 'ERR: the named colour does not exist';"
              "if(r._m&&('ref' in r)&&!r.ref)return 'ERR: the reference item does not exist';"
              "var dc=(r._m&&r.color!==undefined)?String(r.color):null;if(!it)return '';"
              "var p=" + MAIN_POINT_JS.replace("{", "{{").replace("}", "}}") + "(it);var c='';"
              "try{{c=dc!==null?dc:((it.border&&it.border.width>0&&it.color!==undefined&&it.color.a===0)?String(it.border.color):String(it.color===undefined?'':it.color))}}catch(e){{}}"
              "var pal=root.uiPalette;var role='';var roles=['text','textSecondary','textTertiary','link','now','negative',"
              "'focusRing','wave','plotText','plotTextSecondary','textColumn','accent','selection','hover'];for(var i=0;i<roles.length;i++){{try{{if(c.length&&pal[roles[i]]!==undefined&&"
              "Qt.colorEqual(pal[roles[i]],c)){{role=roles[i];break}}}}catch(e){{}}}}"
              "var rr=null;if(ref){{var q=" + MAIN_POINT_JS.replace("{", "{{").replace("}", "}}")
              + "(ref);rr=[q.x,q.y,ref.width,ref.height]}}"
              "return JSON.stringify({{rect:[p.x,p.y,it.width,it.height],ref_rect:rr,declared:c,role:role,"
              "shown:!!it.visible&&it.opacity>0,extra:(r._m&&r.extra)?r.extra:null,"
              "text:String(it.text===undefined?'':it.text).slice(0,40)}})}})()")


def m_measure(r, arg):
    """measure LABEL KIND JSEXPR: records an item's rect, declared colour and palette role for CONTRAST.md.
    KIND: text (foreground = the text's ink), ring (a focus ring or outline), fill (a tint against a reference
    rect), bar (a fill judged as a non-text element, 3:1), glyph (an icon: its strongest pixel, like text, judged
    as non-text, 3:1), wave-played, wave-unplayed.
    The runner measures again right after the grab (measure_now), so the numbers describe the grabbed frame."""
    label, kind, expr = arg.split(" ", 2)
    if kind == "na":
        raise ValueError("measure: use measure_na for an n/a row")
    r.pending_measures = getattr(r, "pending_measures", []) + [measure_now(r, label, kind, expr)]


def m_measure_na(r, arg):
    """measure_na LABEL REASON: a CONTRAST.md row marked n/a, with its reason (e.g. no bar by design)."""
    label, reason = arg.split(" ", 1)
    r.pending_measures = getattr(r, "pending_measures", []) + [dict(label=label, kind="na", reason=reason, _expr="")]


def measure_now(r, label, kind, expr):
    if kind == "na":
        raise ValueError("an n/a row is not measured")
    res = r.s.ev(MEASURE_JS.format(expr=expr))
    if not res and "min_w:" in expr:  # an element with a minimum size that is not drawn: a failing row, not a skip
        mw = int(re.search(r"min_w:(\d+)", expr).group(1))
        m = dict(rect=[0, 0, 0, 0], ref_rect=None, declared="", role="", shown=False, extra=dict(min_w=mw), text="")
        m.update(label=label, kind=kind, _expr=expr)
        return m
    if not res:  # loudly: a measurement never falls back to another item or colour
        raise LookupError(f"measure {label}: no item")
    if res.startswith("ERR:"):
        raise LookupError(f"measure {label}: {res[4:].strip()}")
    m = json.loads(res)
    m.update(label=label, kind=kind)
    if kind.startswith("wave"):  # the seek bar's played and unplayed parts, split at the playhead
        ph = r.s.rect("seekPlayhead")
        x, y, w, h = m["rect"]
        if ph:
            cut = ph[0] + ph[2] / 2
            m["rect"] = [x, y, cut - x - 3, h] if kind == "wave-played" else [cut + 3, y, x + w - cut - 3, h]
    m["_expr"] = expr
    return m


def m_dialogmenu(r, arg):
    """dialogmenu: a menu is open in the dialog window, shown by KWin, or the state fails (the dialog alone passed
    the runner's check of a dialog state)."""
    end = time.time() + 1.5
    while time.time() < end:
        dw = r.s.dialog_window()
        if dw and any(p["opened"] and "Menu" in p["cls"] and p.get("exposed", True) for p in r.s.popups(dw["cls"])):
            return
        time.sleep(0.05)
    raise RuntimeError("dialogmenu: no menu open in the dialog")


def m_opendialog(r, arg):
    """opendialog ID: opens a dialog whether it is a Popup (open()) or a Window (show()); P2 turns
    Configure and About into real top-level windows."""
    r.s.ev(f"(function(){{var d={arg};if(!d)return false;if(typeof d.open==='function')d.open();"
           f"else if(typeof d.show==='function')d.show();else d.visible=true;return true}})()")
    r.settle_or(1.0)


def m_openprefs(r, arg):
    """openprefs PAGE: Configure on that page (its title, or its index), through openPage() as the app does. A title
    the dialog does not have fails the state (the pages are read from the source: PREF_PAGES)."""
    titles = r.s.ev("JSON.stringify(preferencesDialog.pageTitles || null)")
    titles = json.loads(titles) if isinstance(titles, str) else None
    if not titles:
        raise LookupError("openprefs: Configure has no pageTitles")
    if [t for t, _ in PREF_PAGES] != titles:  # the build is not the source the states were made from
        raise LookupError(f"openprefs: Configure's pages {titles} are not the source's {[t for t, _ in PREF_PAGES]}")
    a = arg.strip()
    idx = int(a) if a.isdigit() else (titles.index(a) if a in titles else -1)
    if not 0 <= idx < len(titles):  # loudly: openPage() clamps an index past the end to the last page
        raise LookupError(f"openprefs: Configure has no page '{a}' (pages: {titles})")
    r.s.ev(f"(function(){{var d=preferencesDialog;if(typeof d.openPage==='function'){{d.openPage({idx});return true}}"
           f"d.pageIndex={idx};if(typeof d.open==='function')d.open();else if(typeof d.show==='function')d.show();"
           f"else d.visible=true;return true}})()")
    r.settle_or(1.1)
    got = r.s.ev("preferencesDialog.pageIndex")
    if got is None or int(got) != idx:
        raise RuntimeError(f"openprefs: Configure shows page {got}, not {idx} ({titles[idx]})")


# Icon comparisons against icon trees outside the source (all optional; none by default):
# - SHOT_ICON_B: a candidate icon tree (its hicolor folder or the folder that holds it); SHOT_ICON_AB=1 shows the
#   installed icon (A) against it, and then it must exist. Without SHOT_ICON_AB the sheet shows the installed icon alone.
# - SHOT_ICON_VARIANTS_DIR: a folder of variants (<name>/hicolor, <name>/mark/*.png), SHOT_ICON_VARIANT_NAMES the ones
#   to show; SHOT_ICON_VARIANTS=1 makes them required.
# - SHOT_ICON_BEFORE: a "before" snapshot (hicolor/, glyph/spectrogram-symbolic.svg) to compare the installed icon
#   with; SHOT_ICON_BEFORE_LABEL names it. Unset (or "none"), the before rows are left out.
def _env_dir(name):
    v = os.environ.get(name, "")
    return os.path.realpath(v) if v and v != "none" else ""


ICON_B_DIR = _env_dir("SHOT_ICON_B")
ICON_AB = os.environ.get("SHOT_ICON_AB") == "1"
ICON_A_LABEL = os.environ.get("SHOT_ICON_A_LABEL", "A: installed" if ICON_AB else "Installed")
# B is named after its folder (".../icon-p20/hicolor" or ".../icon-p20": "B: proposal P20 (icon-p20)")
_b_base = os.path.basename(os.path.dirname(ICON_B_DIR.rstrip("/")) if ICON_B_DIR.rstrip("/").endswith("hicolor")
                           else ICON_B_DIR.rstrip("/"))
_b_p = re.search(r"-p(\d+)$", _b_base)
ICON_B_LABEL = os.environ.get("SHOT_ICON_B_LABEL", f"B: proposal P{_b_p.group(1)} ({_b_base})" if _b_p else f"B: {_b_base}")


def _icon_b_root():
    """The B tree's hicolor folder: SHOT_ICON_B may name it or the folder that holds it."""
    d = ICON_B_DIR.rstrip("/")
    if not d:
        return ""
    return d if os.path.basename(d) == "hicolor" else os.path.join(d, "hicolor")


def icon_b_svg(px):
    if not ICON_B_DIR:
        return None
    for d in (f"{px}x{px}", str(px)):
        p = os.path.join(_icon_b_root(), d, "apps", "ferrous.svg")
        if os.path.exists(p):
            return p
    p = os.path.join(_icon_b_root(), "scalable", "apps", "ferrous.svg")
    return p if os.path.exists(p) else None


# ---- The app icon (focus.py "app-icon")
_BREEZE = {"dark": dict(window=(32, 35, 38), panel=(41, 44, 48), text=(252, 252, 252), muted=(161, 169, 177)),
           "light": dict(window=(239, 240, 241), panel=(222, 224, 226), text=(35, 38, 41), muted=(112, 125, 138))}  # (muted = ForegroundInactive)  # Window / Header BackgroundNormal
_CONTEXT_APPS = ["system-file-manager", "utilities-terminal", "kate", "okular", "gwenview", "elisa", "systemsettings"]
# (candidate icons to compare; see SHOT_ICON_VARIANTS_DIR above)
ICON_VARIANTS_DIR = _env_dir("SHOT_ICON_VARIANTS_DIR")
ICON_VARIANTS = [v for v in os.environ.get("SHOT_ICON_VARIANT_NAMES", "").split(",") if v]


def _render_svg(r, svg, px, out):
    # the dark and light workers share appicon-raw and render some files alike: each writes its own temporary
    # file and renames it into place (atomic), so neither reads the other's half-written PNG
    import threading
    tmp = f"{out[:-4]}.{os.getpid()}-{threading.get_ident()}.png"
    res = r.s.cmd(f"svg {svg} {px} {tmp}")
    if not str(res).startswith("ok"):
        raise RuntimeError(f"appicon: {svg} at {px} px did not render: {res}")
    from PIL import Image
    im = Image.open(tmp).convert("RGBA")
    os.replace(tmp, out)
    return im


def _icon_svg(root, px):
    for d in (f"{px}x{px}", str(px)):
        p = os.path.join(root, d, "apps", "ferrous.svg")
        if os.path.exists(p):
            return p
    p = os.path.join(root, "scalable", "apps", "ferrous.svg")
    return p if os.path.exists(p) else None


def _breeze_svg(name, px):
    for d in (str(px), "48"):
        p = os.path.join("/usr/share/icons/breeze/apps", d, name + ".svg")
        if os.path.exists(p):
            return p
    return None


def _icon_measure(im, panels):
    """An icon raster (RGBA): its tile (most frequent opaque colour), its rim (the most frequent colour of the opaque
    pixels on the alpha edge), the strokes (opaque pixels far from both) at the columns 25/50/75 % across: their count,
    widths and the gaps between them; the fan's divergence (the strokes' vertical spread at 75 % minus at 25 %);
    the tile's and the rim's contrast against each panel colour."""
    import numpy as np
    a = np.asarray(im).astype(int)
    rgb, al = a[..., :3], a[..., 3]
    op = al > 200
    if not op.any():
        return "nothing opaque"
    tile = _colours(rgb[op], n=1)[0][0]
    edge = op & ~(np.roll(op, 1, 0) & np.roll(op, -1, 0) & np.roll(op, 1, 1) & np.roll(op, -1, 1))
    rim = _colours(rgb[edge], n=1)[0][0] if edge.any() else tile
    inner = op & ~edge
    for _ in range(1):
        inner = inner & np.roll(inner, 1, 0) & np.roll(inner, -1, 0) & np.roll(inner, 1, 1) & np.roll(inner, -1, 1)
    far = lambda c_: np.abs(rgb - np.array(c_)).sum(axis=2) > 90
    stroke = inner & far(tile) & far(rim)
    H, W = op.shape
    cols = []
    spreads = []
    for fx in (0.25, 0.5, 0.75):
        x = min(W - 1, int(W * fx))
        c = stroke[:, x]
        runs, y, gaps, last_end = [], 0, [], None
        while y < H:
            if c[y]:
                y0 = y
                while y < H and c[y]:
                    y += 1
                if last_end is not None:
                    gaps.append(y0 - last_end)
                runs.append(y - y0)
                last_end = y
            else:
                y += 1
        ys = np.nonzero(c)[0]
        spreads.append(int(ys.max() - ys.min() + 1) if len(ys) else 0)
        cols.append(f"{int(fx * 100)} %: {len(runs)} stroke(s) {runs} px" + (f", gaps {gaps} px" if gaps else ""))
    con = "; ".join(f"tile {_contrast(tile, pc):.2f}:1, rim {_contrast(rim, pc):.2f}:1 on {nm}" for nm, pc in panels)
    # the rim's width: along the middle row and column, from each opaque edge inward, the pixels near the rim colour
    widths = []
    if rim != tile:
        near_rim = np.abs(rgb - np.array(rim)).sum(axis=2) <= 40
        for line, opl in ((near_rim[H // 2, :], op[H // 2, :]), (near_rim[H // 2, ::-1], op[H // 2, ::-1]),
                          (near_rim[:, W // 2], op[:, W // 2]), (near_rim[::-1, W // 2], op[::-1, W // 2])):
            k0 = int(np.argmax(opl))
            k = k0
            while k < len(line) and line[k]:
                k += 1
            widths.append(k - k0)
    rim_txt = f"rim {_hexs(rim)}, {min(widths)}-{max(widths)} px wide" if widths else "no rim (the edge is the tile)"
    return (f"tile {_hexs(tile)}, {rim_txt}; strokes at " + "; ".join(cols)
            + f"; fan spread {spreads[0]} px at 25 % -> {spreads[2]} px at 75 % (divergence {spreads[2] - spreads[0]:+d} px); "
            + con)


def m_appicon(r, arg):
    """appicon: the installed icon (from the source tree) and, when present, the variants under ICON_VARIANTS_DIR
    rendered with Qt's SVG plugin (as icon themes are), composed on Breeze's Window and Header (panel) colours:
    appicon/sizes (16-256 px at 1:1, 4x of 16/22/32), appicon/context (a panel row at 22 and 32 px and a launcher
    grid at 48 px beside real Breeze app icons), appicon/variants (installed vs each variant at 16/22/48/128, when the
    variant folders exist; SHOT_ICON_VARIANTS=1 makes them required); per-size measures into facts.jsonl."""
    r.s.draws_without_app_pixels()  # (SVGs and icon files; app pixels only from shots it names)
    from PIL import Image, ImageDraw, ImageFont
    src = os.environ.get("SHOT_SRC", "")
    a_root = os.path.join(src, "ui/assets/icons/hicolor")
    raw = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "appicon-raw")
    os.makedirs(raw, exist_ok=True)
    th = r.s.tag
    B = _BREEZE[th]
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 14)
    fr = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 12)
    sizes = [16, 22, 32, 48, 64, 128, 256]
    icons = {}
    for px in sizes:
        svg = _icon_svg(a_root, px)
        if not svg:
            raise FileNotFoundError(f"appicon: no installed icon at {px} px in {a_root}")
        icons[px] = _render_svg(r, svg, px, os.path.join(raw, f"installed-{px}.png"))
    panels = [("Breeze Dark window", _BREEZE["dark"]["window"]), ("Breeze Dark panel", _BREEZE["dark"]["panel"]),
              ("Breeze Light window", _BREEZE["light"]["window"]), ("Breeze Light panel", _BREEZE["light"]["panel"])]
    if th == "dark":  # the measures once (the icon is the same in both themes)
        for px in sizes:
            _fact(r, f"appicon-installed-{px}", f"Installed icon at {px} px: {_icon_measure(icons[px], panels)}")
    # 1. sizes at 1:1 and 4x of 16/22/32
    gap = 18
    w1 = sum(sizes) + gap * (len(sizes) + 1)
    z = [16, 22, 32]
    h_ = 30 + 256 + 24 + 4 * 32 + 30
    S = Image.new("RGB", (max(w1, 400), h_), B["window"])
    d = ImageDraw.Draw(S)
    d.text((gap, 6), f"Installed icon, 1:1 at 16-256 px; 4x of 16/22/32 (Breeze {th.capitalize()} window colour)",
           fill=B["text"], font=fb)
    x = gap
    for px in sizes:
        S.paste(icons[px], (x, 30 + 256 - px), icons[px])
        d.text((x, 30 + 256 + 4), f"{px}", fill=B["text"], font=fr)
        x += px + gap
    x = gap
    for px in z:
        big = icons[px].resize((px * 4, px * 4), Image.NEAREST)
        S.paste(big, (x, 30 + 256 + 24 + 4 * 32 - px * 4), big)
        d.text((x + px * 4 + 4, 30 + 256 + 24 + 4 * 32 - 16), f"{px} at 4x", fill=B["text"], font=fr)
        x += px * 4 + 70
    _save_shot(r, S, "appicon", "sizes", f"The installed icon at 1:1 (16-256 px) and 4x (16/22/32 px) on Breeze {th.capitalize()}")
    # 2. context: a panel row at 22 and 32 px, a launcher grid at 48 px, beside real Breeze app icons
    def app_icon(name, px):
        if name == "ferrous":
            return icons[px]
        svg = _breeze_svg(name, px)
        if not svg:
            raise FileNotFoundError(f"appicon: no Breeze icon {name}")
        return _render_svg(r, svg, px, os.path.join(raw, f"breeze-{name}-{px}.png"))
    row = _CONTEXT_APPS[:3] + ["ferrous"] + _CONTEXT_APPS[3:]
    cellw = 40
    C = Image.new("RGB", (len(row) * 96 + 40, 30 + 40 + 16 + 52 + 26 + 2 * 96 + 20), B["window"])
    d = ImageDraw.Draw(C)
    d.text((14, 6), f"Breeze {th.capitalize()}: panels at 22 and 32 px, a launcher at 48 px", fill=B["text"], font=fb)
    y = 30
    for px, ph in ((22, 40), (32, 52)):
        d.rectangle((0, y, C.width, y + ph - 1), fill=B["panel"])
        x = 14
        for nm in row:
            ic = app_icon(nm, px)
            C.paste(ic, (x + (cellw - px) // 2, y + (ph - px) // 2), ic)
            x += cellw + 6
        d.text((x + 10, y + ph // 2 - 8), f"{px} px panel", fill=B["text"], font=fr)
        y += ph + 16
    y += 6
    for i, nm in enumerate(row):
        cx, cy = 14 + (i % 4) * 96, y + (i // 4) * 96
        ic = app_icon(nm, 48)
        C.paste(ic, (cx + 24, cy + 6), ic)
        lab = "Ferrous" if nm == "ferrous" else nm.replace("system-file-manager", "Files").replace("utilities-terminal", "Terminal") \
            .replace("systemsettings", "Settings").capitalize()
        tw = d.textlength(lab, font=fr)
        d.text((cx + 48 - tw / 2, cy + 60), lab, fill=B["text"], font=fr)
    C = C.crop((0, 0, max(14 + 4 * 96 + 14, len(row) * (cellw + 6) + 140), y + 2 * 96 + 10))
    _save_shot(r, C, "appicon", "context", f"The icon in context on Breeze {th.capitalize()}: panel rows (22, 32 px) and a "
                                           f"launcher grid (48 px) beside Breeze app icons ({', '.join(_CONTEXT_APPS)})")
    # 3. variants, when they exist
    have = [v for v in ICON_VARIANTS if os.path.isdir(os.path.join(ICON_VARIANTS_DIR, v, "hicolor"))]
    if os.environ.get("SHOT_ICON_VARIANTS") == "1" and len(have) < len(ICON_VARIANTS):
        raise FileNotFoundError(f"appicon: variants requested (SHOT_ICON_VARIANTS=1) but missing: "
                                f"{[v for v in ICON_VARIANTS if v not in have]} under {ICON_VARIANTS_DIR}")
    if have:
        vs = [("installed", a_root)] + [(v, os.path.join(ICON_VARIANTS_DIR, v, "hicolor")) for v in have]
        vsz = [16, 22, 48, 128]
        rh = 128 + 30  # (the 4x icons sit in the same row as the 1:1 ones)
        V = Image.new("RGB", (60 + sum(vsz) + 4 * 22 * 2 + 30 * 7, 34 + len(vs) * rh), B["window"])
        d = ImageDraw.Draw(V)
        d.text((14, 6), f"Installed vs variants, Breeze {th.capitalize()}: 16/22/48/128 px at 1:1, 16/22 at 4x", fill=B["text"], font=fb)
        for k, (vn, root) in enumerate(vs):
            y = 34 + k * rh
            d.text((14, y), vn, fill=B["text"], font=fb)
            x = 100
            for px in vsz:
                svg = _icon_svg(root, px)
                if not svg:
                    raise FileNotFoundError(f"appicon: variant {vn} has no {px} px icon in {root}")
                ic = _render_svg(r, svg, px, os.path.join(raw, f"{vn}-{px}.png"))
                if th == "dark" and vn != "installed":
                    _fact(r, f"appicon-{vn}-{px}", f"Variant {vn} at {px} px: {_icon_measure(ic, panels)}")
                V.paste(ic, (x, y + 128 - px), ic)
                d.text((x, y + 132), f"{px}", fill=B["text"], font=fr)
                x += px + 30
            for px in (16, 22):
                ic = _render_svg(r, _icon_svg(root, px), px, os.path.join(raw, f"{vn}-{px}.png")).resize((px * 4, px * 4), Image.NEAREST)
                V.paste(ic, (x, y + 128 - px * 4), ic)
                x += px * 4 + 30
        _save_shot(r, V, "appicon", "variants", f"The installed icon vs {', '.join(have)} on Breeze {th.capitalize()}")
    else:
        _fact(r, "appicon-variants", f"Variants: none rendered yet (no {', '.join(ICON_VARIANTS)} under {ICON_VARIANTS_DIR})"
              if ICON_VARIANTS else "Variants: none (SHOT_ICON_VARIANTS_DIR and SHOT_ICON_VARIANT_NAMES unset)")


ICON_BEFORE_DIR = _env_dir("SHOT_ICON_BEFORE")
ICON_BEFORE_LABEL = os.environ.get("SHOT_ICON_BEFORE_LABEL", os.path.basename(ICON_BEFORE_DIR))
_NEIGHBOURS = ["system-file-manager", "utilities-terminal", "elisa", "labplot", "kmplot", "kate", "okular"]


def _app_svg(name, px):
    for base in ("/usr/share/icons/breeze/apps", "/usr/share/icons/hicolor"):
        for d in (str(px), f"{px}x{px}", "48", "48x48", "scalable"):
            for sub in ("", "apps"):
                p = os.path.join(base, d, sub, name + ".svg")
                if os.path.exists(p):
                    return p
    return None


def _icon_measure2(im, panels):
    """an icon raster (RGBA): tile, rim (colour, width); the traces at 10/30/50/70/90 % across: their widths, the
    clear rows above the top trace and under the bottom one (to the rim's inner edge) and between traces; each trace's
    colour at 50 % and its contrast against the tile; whether the traces run flush to the tile's inner edge (per row
    with trace pixels: the gap from the inner edge to the trace, left and right); tile and rim on the panel colours."""
    import numpy as np
    a = np.asarray(im).astype(int)
    rgb, al = a[..., :3], a[..., 3]
    op = al > 200
    if not op.any():
        return "nothing opaque"
    H, W = op.shape
    tile = _colours(rgb[op], n=1)[0][0]
    edge = op & ~(np.roll(op, 1, 0) & np.roll(op, -1, 0) & np.roll(op, 1, 1) & np.roll(op, -1, 1))
    rim = _colours(rgb[edge], n=1)[0][0] if edge.any() else tile
    near_rim = (np.abs(rgb - np.array(rim)).sum(axis=2) <= 40) & op if rim != tile else np.zeros_like(op)
    # the rim's band: rim-coloured pixels connected to the outer edge along rows and columns
    band = np.zeros_like(op)
    for y in range(H):
        xs = np.nonzero(op[y])[0]
        if len(xs):
            for rng in (range(xs[0], W), range(xs[-1], -1, -1)):
                for x in rng:
                    if near_rim[y, x]:
                        band[y, x] = True
                    else:
                        break
    for x in range(W):
        ys = np.nonzero(op[:, x])[0]
        if len(ys):
            for rng in (range(ys[0], H), range(ys[-1], -1, -1)):
                for y in rng:
                    if near_rim[y, x]:
                        band[y, x] = True
                    else:
                        break
    inner = op & ~band
    stroke = inner & (np.abs(rgb - np.array(tile)).sum(axis=2) > 90)
    rim_w = []
    if rim != tile:
        for line in (band[H // 2, :], band[H // 2, ::-1], band[:, W // 2], band[::-1, W // 2]):
            k0 = int(np.argmax(line)) if line.any() else 0
            k = k0
            while k < len(line) and line[k]:
                k += 1
            rim_w.append(k - k0)
    cols, trace_cols = [], None
    for fx in (0.1, 0.3, 0.5, 0.7, 0.9):
        x = min(W - 1, int(W * fx))
        c = stroke[:, x]
        ins = np.nonzero(inner[:, x])[0]
        runs, y = [], 0
        while y < H:
            if c[y]:
                y0 = y
                while y < H and c[y]:
                    y += 1
                runs.append((y0, y))
            else:
                y += 1
        if fx == 0.5:
            trace_cols = (x, runs)
        if runs and len(ins):
            clear_top, clear_bot = runs[0][0] - ins[0], ins[-1] + 1 - runs[-1][1]
            gaps = [b[0] - a_[1] for a_, b in zip(runs, runs[1:])]
            cols.append(f"{int(fx * 100)} %: widths {[b - a_ for a_, b in runs]}, clear {clear_top} above / {clear_bot} under"
                        + (f", between {gaps}" if gaps else ""))
        else:
            cols.append(f"{int(fx * 100)} %: no trace")
    tr_txt = ""
    if trace_cols and trace_cols[1]:
        x, runs = trace_cols
        tr_txt = "; traces at 50 % (top to bottom): " + ", ".join(
            f"{_hexs(tuple(int(v) for v in rgb[(a_ + b) // 2, x]))} {_contrast(tuple(int(v) for v in rgb[(a_ + b) // 2, x]), tile):.2f}:1"
            for a_, b in runs)
    lg, rg_ = [], []
    for y in range(H):
        sx = np.nonzero(stroke[y])[0]
        ix = np.nonzero(inner[y])[0]
        if len(sx) and len(ix):
            lg.append(sx[0] - ix[0])
            rg_.append(ix[-1] - sx[-1])
    flush = (f"; ends: the traces come within {min(lg)} px of the inner edge on the left ({sum(1 for v in lg if v <= 1)} rows "
             f"flush) and {min(rg_)} px on the right ({sum(1 for v in rg_ if v <= 1)} rows flush)") if lg else ""
    rim_txt = f"rim {_hexs(rim)} {min(rim_w)}-{max(rim_w)} px" if rim_w else "no rim"
    con = "; ".join(f"tile {_contrast(tile, pc):.2f}:1, rim {_contrast(rim, pc):.2f}:1 on {nm}" for nm, pc in panels)
    return f"tile {_hexs(tile)}, {rim_txt}; traces " + " | ".join(cols) + tr_txt + flush + "; " + con


def m_appicon2(r, arg):
    """appicon2: the installed icon and the "before" snapshot (ICON_BEFORE_DIR, SHOT_ICON_BEFORE; loud when
    set but missing), on Breeze's window and panel colours: appicon/compare (before vs installed
    at 16/22/32/48/128 and a greyscale squint row at 16 and 48), appicon/surfaces (an icon-only task manager at 22 and
    32 px with an audio badge, a 16 px menu, Alt-Tab 64 px tiles, a 48 px launcher grid; neighbours incl. Elisa and the
    chart-like LabPlot and KmPlot), appicon/sizes2x (1:1 16-256, 32@2x = the 64 px file, 22@2x = 44 px from the
    scalable and from the 48 px file, 4x of 48); appicon/icon128 (for the motif sheet); facts per size."""
    r.s.draws_without_app_pixels()  # (SVGs and icon files; app pixels only from shots it names)
    from PIL import Image, ImageDraw, ImageFont, ImageFilter
    src = os.environ.get("SHOT_SRC", "")
    a_root = os.path.join(src, "ui/assets/icons/hicolor")
    b_root = None if not ICON_BEFORE_DIR else os.path.join(ICON_BEFORE_DIR, "hicolor")
    if b_root and not os.path.isdir(b_root):
        raise FileNotFoundError(f"appicon2: no 'before' icon at {b_root} (unset SHOT_ICON_BEFORE to leave it out)")
    raw = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "appicon-raw")
    os.makedirs(raw, exist_ok=True)
    th = r.s.tag
    B = _BREEZE[th]
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 13)
    fr = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 12)
    panels = [("Breeze Dark window", _BREEZE["dark"]["window"]), ("Breeze Dark panel", _BREEZE["dark"]["panel"]),
              ("Breeze Light window", _BREEZE["light"]["window"]), ("Breeze Light panel", _BREEZE["light"]["panel"])]

    def ico(root, px, tag, svg_px=None):
        svg = _icon_svg(root, svg_px or px)
        if not svg:
            raise FileNotFoundError(f"appicon2: no {tag} icon at {svg_px or px} px in {root}")
        return _render_svg(r, svg, px, os.path.join(raw, f"{tag}-{px}-{svg_px or px}.png"))
    sizes = [16, 22, 32, 48, 64, 128, 256]
    inst = {px: ico(a_root, px, "installed") for px in sizes}
    before = {px: ico(b_root, px, "before") for px in (16, 22, 32, 48, 128)} if b_root else {}
    if th == "dark":
        for px in sizes:
            _fact(r, f"appicon-installed-{px}", f"Installed icon at {px} px: {_icon_measure2(inst[px], panels)}")
        for px, im_ in before.items():
            _fact(r, f"appicon-before-{px}", f"Before ({ICON_BEFORE_LABEL}) icon at {px} px: {_icon_measure2(im_, panels)}")
    # 1. compare + squint
    cs = [16, 22, 32, 48, 128]
    rows = ([("before ", before)] if before else []) + [("installed", inst)]
    cw = 110 + sum(cs) + 20 * len(cs)
    S = Image.new("RGB", (cw, 28 + len(rows) * 158 + 30 + 78), B["window"])
    d = ImageDraw.Draw(S)
    d.text((10, 6), f"Breeze {th.capitalize()}: 1:1", fill=B["text"], font=fb)
    for k, (lb, icons_) in enumerate(rows):
        y = 28 + k * 158
        d.text((10, y + 100), lb, fill=B["text"], font=fr)
        x = 110
        for px in cs:
            S.paste(icons_[px], (x, y + 128 - px), icons_[px])
            d.text((x, y + 132), str(px), fill=B["text"], font=fr)
            x += px + 20
    y = 28 + len(rows) * 158
    d.text((10, y), "squint: greyscale, blurred (radius px/24)", fill=B["text"], font=fr)
    x = 10
    for lb, icons_ in rows:
        for px in (16, 48):
            g = Image.new("RGB", (px + 8, px + 8), B["window"])
            g.paste(icons_[px], (4, 4), icons_[px])
            g = g.convert("L").filter(ImageFilter.GaussianBlur(px / 24)).convert("RGB")
            S.paste(g, (x, y + 22 + 48 - px))
            x += px + 24
        d.text((x - 10, y + 22 + 48 + 2), "", fill=B["text"], font=fr)
        x += 20
    _save_shot(r, S, "appicon", "compare", f"Before ({ICON_BEFORE_LABEL}) vs installed at 16-128 px and a greyscale squint row, Breeze {th}")
    # 2. surfaces
    def app(name, px):
        if name == "ferrous":
            return inst[px] if px in inst else ico(a_root, px, "installed")
        svg = _app_svg(name, px)
        if not svg:
            raise FileNotFoundError(f"appicon2: no icon {name}")
        return _render_svg(r, svg, px, os.path.join(raw, f"app-{name}-{px}.png"))
    row = _NEIGHBOURS[:3] + ["ferrous"] + _NEIGHBOURS[3:]
    badge_svg = f"/usr/share/icons/{'breeze-dark' if th == 'dark' else 'breeze'}/status/16/audio-volume-high.svg"
    W_ = 30 + len(row) * 96
    U = Image.new("RGB", (W_, 820), B["window"])
    d = ImageDraw.Draw(U)
    y = 8
    d.text((10, y), f"Breeze {th.capitalize()}: icon-only task manager at 22 and 32 px (Ferrous playing: the audio badge)",
           fill=B["text"], font=fb)
    y += 22
    for px, ph in ((22, 44), (32, 56)):
        d.rectangle((0, y, W_, y + ph - 1), fill=B["panel"])
        x = 10
        for nm in row:
            ic = app(nm, px)
            ox, oy = x + (ph - px) // 2, y + (ph - px) // 2
            U.paste(ic, (ox, oy), ic)
            if nm == "ferrous":  # the audio badge over the icon's top right, as Plasma's task manager draws it
                bp = 12 if px == 22 else 16
                bd = _render_svg(r, badge_svg, bp, os.path.join(raw, f"badge-{bp}.png"))
                cx, cy = ox + px - bp // 2 - 1, oy - bp // 2 + 2
                d.ellipse((cx - 2, cy - 2, cx + bp + 1, cy + bp + 1), fill=B["panel"])
                U.paste(bd, (cx, cy), bd)
            x += ph + 4
        y += ph + 12
    d.text((10, y), "a 16 px menu", fill=B["text"], font=fb)
    y += 20
    for nm, lab in (("system-file-manager", "Files"), ("elisa", "Elisa"), ("ferrous", "Ferrous"), ("labplot", "LabPlot")):
        if nm == "ferrous":
            d.rectangle((10, y, 210, y + 23), fill=B["panel"])
        ic = app(nm, 16)
        U.paste(ic, (16, y + 4), ic)
        d.text((40, y + 4), lab, fill=B["text"], font=fr)
        y += 24
    y += 10
    d.text((10, y), "Alt-Tab, 64 px tiles", fill=B["text"], font=fb)
    y += 20
    d.rectangle((0, y, W_, y + 106), fill=(B["panel"][0] // 2 + 10, B["panel"][1] // 2 + 10, B["panel"][2] // 2 + 10) if th == "dark" else B["panel"])
    for i, nm in enumerate(["system-file-manager", "elisa", "ferrous", "labplot", "kate"]):
        x = 14 + i * 100
        if nm == "ferrous":
            d.rounded_rectangle((x - 4, y + 6, x + 88, y + 100), radius=6, outline=(61, 174, 233), width=2)
        ic = app(nm, 64)
        U.paste(ic, (x + 12, y + 14), ic)
        tw = d.textlength(nm if nm != "ferrous" else "Ferrous", font=fr)
    y += 116
    d.text((10, y), "launcher grid, 48 px", fill=B["text"], font=fb)
    y += 20
    for i, nm in enumerate(row):
        cx, cy = 10 + i * 96, y  # (one row: the sheet stays within 1.15 MP)
        ic = app(nm, 48)
        U.paste(ic, (cx + 26, cy + 4), ic)
        lab = {"system-file-manager": "Files", "utilities-terminal": "Terminal", "ferrous": "Ferrous"}.get(nm, nm.capitalize())
        tw = d.textlength(lab, font=fr)
        d.text((cx + 50 - tw / 2, cy + 58), lab, fill=B["text"], font=fr)
    y += 92 + 6
    U = U.crop((0, 0, W_, y))
    _save_shot(r, U, "appicon", "surfaces", f"Plasma-like surfaces on Breeze {th}: task manager 22/32 px with the audio badge, "
                                            f"a 16 px menu, Alt-Tab 64 px, a 48 px grid; neighbours {', '.join(_NEIGHBOURS)}")
    # 3. sizes at 1:1, 2x renders, 4x of 48
    s64 = ico(a_root, 64, "installed")
    s44a = ico(a_root, 44, "installed", svg_px=1)   # (svg_px 1: no such folder: the scalable)
    s44b = ico(a_root, 44, "installed", svg_px=48)
    big = inst[48].resize((192, 192), Image.NEAREST)
    Z = Image.new("RGB", (20 + sum(sizes) + 18 * 7 + 20, 30 + 256 + 24 + 200 + 30), B["window"])
    d = ImageDraw.Draw(Z)
    d.text((10, 6), f"Breeze {th.capitalize()}: 1:1 at 16-256 px; below, device pixels at 2x and 48 px at 4x", fill=B["text"], font=fb)
    x = 10
    for px in sizes:
        Z.paste(inst[px], (x, 30 + 256 - px), inst[px])
        d.text((x, 30 + 256 + 4), str(px), fill=B["text"], font=fr)
        x += px + 18
    y = 30 + 256 + 24
    x = 10
    for im_, lb in ((s64, "32@2x: the 64 px file"), (s44a, "22@2x: scalable at 44"), (s44b, "22@2x: 48 px file at 44"),
                    (big, "48 px at 4x")):
        Z.paste(im_, (x, y + 192 - im_.height), im_)
        d.text((x, y + 194), lb, fill=B["text"], font=fr)
        x += max(im_.width, int(d.textlength(lb, font=fr))) + 24
    _save_shot(r, Z, "appicon", "sizes2x", f"The installed icon at 1:1, its 2x device-pixel renders and 48 px at 4x, Breeze {th}")
    I1 = Image.new("RGB", (148, 148), B["window"])
    I1.paste(inst[128], (10, 10), inst[128])
    _save_shot(r, I1, "appicon", "icon128", f"The installed icon at 128 px on Breeze {th}")
    I2 = Image.new("RGB", (68, 68), B["window"])
    I2.paste(inst[48], (10, 10), inst[48])
    _save_shot(r, I2, "appicon", "icon48", f"The installed icon at 48 px on Breeze {th}")


_KDE_LOOKUP = {}
_KDE_SIZES = (16, 22, 24, 32, 36, 40, 44, 48, 56, 64, 96, 128, 256)
_KDE_DPRS = (1.0, 1.25, 1.5, 1.75, 2.0)  # (1.75 too)


def _kde_lookup(root, mode="kde", sizes=_KDE_SIZES, dprs=_KDE_DPRS):
    """the real lookup, asked of Qt itself. Runs the icon probe (iconprobe.cpp, built by build_probes.sh)
    with Plasma's platform theme (QT_QPA_PLATFORMTHEME=kde, Breeze inheriting hicolor) on a copy of the system's
    hicolor index.theme and one solid SVG per folder `root` has; returns {(slot, dpr): (folder, device px)}.
    Offscreen, in its own dbus-run-session with private XDG dirs under TMPDIR; fails loudly on any unknown answer.
    mode "kiconloader" asks KIconLoader::loadScaledIcon directly (the loader behind Plasma's KIconEngine, to verify
    "kde"), "qt" Qt's own QIconLoader without the KDE platform theme (what a non-Plasma session takes)."""
    import shutil
    import subprocess
    import tempfile
    root = os.path.realpath(root)
    key = (root, mode, tuple(sizes), tuple(dprs))
    if key in _KDE_LOOKUP:
        return _KDE_LOOKUP[key]
    probe = ICONPROBE
    if not os.access(probe, os.X_OK):
        raise RuntimeError(f"_kde_lookup: no icon probe at {probe} (build_probes.sh builds it; SHOT_PROBE_DIR)")
    dirs = sorted(d for d in os.listdir(root) if os.path.isfile(os.path.join(root, d, "apps", "ferrous.svg")))
    if not dirs or len(dirs) > 31:
        raise RuntimeError(f"_kde_lookup: {len(dirs)} icon folders in {root}")
    name = "ferrous-harness-probe"
    T = tempfile.mkdtemp(prefix="iconprobe.", dir=os.environ["TMPDIR"])
    try:
        hi = os.path.join(T, "icons", "hicolor")
        os.makedirs(hi)
        for sub in ("config", "cache", "run"):
            os.makedirs(os.path.join(T, sub), mode=0o700)
        shutil.copy("/usr/share/icons/hicolor/index.theme", hi)
        for i, d in enumerate(dirs):
            m = re.match(r"^(\d+)x\d+(@(\d+))?$", d)
            px = 64 if d == "scalable" else int(m.group(1)) * int(m.group(3) or 1)
            os.makedirs(os.path.join(hi, d, "apps"))
            with open(os.path.join(hi, d, "apps", name + ".svg"), "w") as f:
                f.write(f"<svg xmlns='http://www.w3.org/2000/svg' width='{px}' height='{px}' viewBox='0 0 {px} {px}'>"
                        f"<rect width='{px}' height='{px}' fill='#{i * 8:02x}8040'/></svg>")
        with open(os.path.join(T, "config", "kdeglobals"), "w") as f:
            f.write("[Icons]\nTheme=breeze\n")
        env = {k: v for k, v in os.environ.items() if k not in (
            "WAYLAND_DISPLAY", "DISPLAY", "XDG_CURRENT_DESKTOP", "DBUS_SESSION_BUS_ADDRESS", "QT_PLUGIN_PATH",
            "QT_QPA_PLATFORMTHEME", "QT_STYLE_OVERRIDE")}
        env.update(QT_QPA_PLATFORM="offscreen", PROBE_MODE=mode, PROBE_ICON=name,
                   PROBE_SIZES=",".join(map(str, sizes)), PROBE_DPRS=",".join(f"{v:g}" for v in dprs),
                   XDG_DATA_HOME=T, XDG_DATA_DIRS=f"{T}:/usr/share", XDG_CONFIG_HOME=os.path.join(T, "config"),
                   XDG_CACHE_HOME=os.path.join(T, "cache"), XDG_RUNTIME_DIR=os.path.join(T, "run"))
        if mode in ("kde", "kiconloader"):
            env["QT_QPA_PLATFORMTHEME"] = "kde"
        elif mode == "qt":
            env["XDG_DATA_DIRS"] = T
        else:
            raise ValueError(f"_kde_lookup: mode {mode!r}")
        out = subprocess.run(["dbus-run-session", "--", probe, T, ",".join(dirs)], env=env, capture_output=True,
                             text=True, timeout=300)
        if out.returncode != 0:
            raise RuntimeError(f"_kde_lookup: probe exit {out.returncode}: {out.stderr[-400:]}")
    finally:
        shutil.rmtree(T, ignore_errors=True)
    tab = {}
    for ln in out.stdout.splitlines():
        cells = ln.split("\t")
        if not cells[0].isdigit():
            continue
        for dpr, c in zip(dprs, cells[1:]):
            m = re.match(r"^(.+)\((\d+)px\)$", c)
            if not m or m.group(1) not in dirs:
                raise RuntimeError(f"_kde_lookup: probe answer {c!r} for {cells[0]} px at {dpr:g}x ({root})")
            tab[(int(cells[0]), dpr)] = (m.group(1), int(m.group(2)))
    if len(tab) != len(sizes) * len(dprs):
        raise RuntimeError(f"_kde_lookup: {len(tab)} probe answers: {out.stdout[-400:]}")
    _KDE_LOOKUP[key] = tab
    return tab


def _qt_pick(root, slot, dpr):
    """the icon file Plasma's lookup takes for a `slot` px icon at `dpr` (QIcon::fromTheme under the KDE platform
    theme, real hicolor index; see _kde_lookup), drawn at the device px Qt asks of it. Returns (svg, label, device px);
    the label names the folder, with "scaled" when Qt draws it at another size than the folder's own."""
    folder, dev = _kde_lookup(root)[(slot, float(dpr))]
    m = re.match(r"^(\d+)x\d+(@(\d+))?$", folder)
    own = None if folder == "scalable" else int(m.group(1)) * int(m.group(3) or 1)
    lab = folder if own in (None, dev) else f"{folder}, scaled"
    return os.path.join(root, folder, "apps", "ferrous.svg"), lab, dev


def _trace_lums(im):
    """The traces at 50 % across (top to bottom): each one's luminance (0-255, Rec. 709) in greyscale."""
    import numpy as np
    a = np.asarray(im.convert("RGBA")).astype(int)
    rgb, op = a[..., :3], a[..., 3] > 200
    if not op.any():
        return []
    tile = _colours(rgb[op], n=1)[0][0]
    x = a.shape[1] // 2
    far = (np.abs(rgb[:, x] - np.array(tile)).sum(axis=1) > 90) & op[:, x]
    ys = np.nonzero(op[:, x])[0]
    if len(ys):  # (the rim: the opaque column's first and last 2 rows aside)
        far[:ys[0] + 2] = False
        far[ys[-1] - 1:] = False
    out, y = [], 0
    while y < len(far):
        if far[y]:
            y0 = y
            while y < len(far) and far[y]:
                y += 1
            c = rgb[(y0 + y) // 2, x]
            out.append(round(0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]))
        else:
            y += 1
    return out


def m_appicon3(r, arg):
    """appicon3: appicon/compare20 (before vs installed at 16-128 px; a greyscale squint row at 16/32/48/64 with each
    trace's luminance; 4x crops of the 22 and 32 px task-manager slots with the audio badge, and of 32 next to 48),
    appicon/scales (the slots at 1.25x and 1.5x device px, the file Plasma's lookup takes named; 16@2 and 22@2), appicon/large (128 and
    256 px beside Elisa and Kate), appicon/variants20 (installed vs the variants at 32-256 beside LabPlot and KmPlot,
    when they exist; SHOT_ICON_VARIANTS=1 requires them)."""
    r.s.draws_without_app_pixels()  # (SVGs and icon files; app pixels only from shots it names)
    from PIL import Image, ImageDraw, ImageFont, ImageFilter
    src = os.environ.get("SHOT_SRC", "")
    a_root = os.path.join(src, "ui/assets/icons/hicolor")
    b_root = None if not ICON_BEFORE_DIR else os.path.join(ICON_BEFORE_DIR, "hicolor")
    if b_root and not os.path.isdir(b_root):
        raise FileNotFoundError(f"appicon3: no 'before' icon at {b_root} (unset SHOT_ICON_BEFORE to leave it out)")
    raw = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "appicon-raw")
    os.makedirs(raw, exist_ok=True)
    th = r.s.tag
    B = _BREEZE[th]
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 13)
    fr = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 12)

    def rend(svg, px, tag):
        return _render_svg(r, svg, px, os.path.join(raw, f"{tag}-{px}.png"))

    def ico(root, px, tag):
        svg = _icon_svg(root, px)
        if not svg:
            raise FileNotFoundError(f"appicon3: no {tag} icon at {px} px in {root}")
        return rend(svg, px, f"{tag}-{os.path.basename(os.path.dirname(os.path.dirname(svg)))}")
    inst = {px: ico(a_root, px, "installed") for px in (16, 22, 32, 48, 64, 128, 256)}
    before = {px: ico(b_root, px, "before") for px in (16, 22, 32, 48, 64, 128)} if b_root else {}
    # 1. compare, squint (with each trace's luminance), 4x crops
    cs = [16, 22, 32, 48, 128]
    rows = ([(f"before ({ICON_BEFORE_LABEL})", before)] if before else []) + [("installed", inst)]
    S = Image.new("RGB", (900, 28 + len(rows) * 158 + 40 + 100 + 30 + 240), B["window"])
    d = ImageDraw.Draw(S)
    d.text((10, 6), f"Breeze {th.capitalize()}: 1:1", fill=B["text"], font=fb)
    for k, (lb, icons_) in enumerate(rows):
        y = 28 + k * 158
        d.text((10, y + 100), lb, fill=B["text"], font=fr)
        x = 110
        for px in cs:
            S.paste(icons_[px], (x, y + 128 - px), icons_[px])
            d.text((x, y + 132), str(px), fill=B["text"], font=fr)
            x += px + 20
    y = 28 + len(rows) * 158
    d.text((10, y), "squint: greyscale, blurred (radius px/24); each trace's grey level, top to bottom", fill=B["text"], font=fr)
    y += 20
    x = 10
    lum_facts = []
    for lb, icons_ in rows:
        for px in (16, 32, 48, 64):
            g = Image.new("RGB", (px + 8, px + 8), B["window"])
            g.paste(icons_[px], (4, 4), icons_[px])
            g = g.convert("L").filter(ImageFilter.GaussianBlur(px / 24)).convert("RGB")
            S.paste(g, (x, y + 72 - px))
            lums = _trace_lums(icons_[px])
            d.text((x, y + 76), "/".join(str(v) for v in lums) or "-", fill=B["text"], font=fr)
            if th == "dark":
                lum_facts.append(f"{lb} {px} px: " + (" / ".join(str(v) for v in lums) +
                                 (f" (gaps {', '.join(str(abs(a_ - b)) for a_, b in zip(lums, lums[1:]))})" if len(lums) > 1 else "")))
            x += max(px + 8, 84) + 12  # (room for the grey levels' caption)
        x += 20
    if lum_facts:
        _fact(r, "appicon-squint", "Traces' grey levels at 50 % across (top to bottom: the highest harmonic first; the gaps "
                                   "between neighbours): " + "; ".join(lum_facts))
    y += 100
    S = S.crop((0, 0, max(x + 10, 470), y))
    _save_shot(r, S, "appicon", "compare20", f"Before ({ICON_BEFORE_LABEL}) vs installed and the squint row, Breeze {th}")
    # the 4x crops, an image of their own
    S = Image.new("RGB", (900, 260), B["window"])
    d = ImageDraw.Draw(S)
    y = 4
    d.text((10, y), f"Breeze {th.capitalize()}, 4x: the 22 and 32 px task-manager slots with the audio badge; 32 px beside "
                    "48 px", fill=B["text"], font=fr)
    y += 20
    badge_svg = f"/usr/share/icons/{'breeze-dark' if th == 'dark' else 'breeze'}/status/16/audio-volume-high.svg"
    x = 10
    for px, ph in ((22, 44), (32, 56)):
        slot = Image.new("RGB", (ph, ph), B["panel"])
        o_ = (ph - px) // 2
        slot.paste(inst[px], (o_, o_), inst[px])
        bp = 12 if px == 22 else 16
        bd = rend(badge_svg, bp, f"badge{th}")
        dd = ImageDraw.Draw(slot)
        cx, cy = o_ + px - bp // 2 - 1, o_ - bp // 2 + 2
        dd.ellipse((cx - 2, cy - 2, cx + bp + 1, cy + bp + 1), fill=B["panel"])
        slot.paste(bd, (cx, cy), bd)
        z = slot.resize((ph * 4, ph * 4), Image.NEAREST)
        S.paste(z, (x, y + 224 - z.height))
        x += z.width + 16
    for px in (32, 48):
        z = Image.new("RGB", (px, px), B["window"])
        z.paste(inst[px], (0, 0), inst[px])
        z = z.resize((px * 4, px * 4), Image.NEAREST)
        S.paste(z, (x, y + 224 - z.height) if z.height <= 224 else (x, y))
        x += z.width + 16
    S = S.crop((0, 0, x + 4, y + 228))
    _save_shot(r, S, "appicon", "zoom20", f"4x: the 22 and 32 px task-manager slots with the audio badge; 32 beside 48, Breeze {th}")
    # 2. fractional scales and the @2 files
    Z = Image.new("RGB", (1400, 34 + 3 * 164), B["window"])
    d = ImageDraw.Draw(Z)
    d.text((10, 6), f"Breeze {th.capitalize()}: slot: device px; below, the folder Plasma's lookup takes "
                    "(\"scaled\": not at its own size)", fill=B["text"], font=fb)
    picks = []
    xmax = 0
    for k, dpr in enumerate((1.25, 1.5, 2.0)):
        y = 34 + k * 164
        d.text((10, y + 40), f"{dpr}x", fill=B["text"], font=fb)
        x = 70
        for slot in (16, 22, 24, 32, 36, 48, 64):  # (24, 36 and 64 too)
            svg, lab, dpx = _qt_pick(a_root, slot, dpr)
            im_ = rend(svg, dpx, f"scale-{slot}-{dpr}")
            Z.paste(im_, (x, y + 128 - im_.height), im_)
            d.text((x, y + 131), f"{slot}: {dpx}", fill=B["text"], font=fr)
            d.text((x, y + 145), lab, fill=B["text"], font=fr)
            picks.append(f"{slot} px at {dpr}x: {dpx} device px from {lab}")
            x += max(im_.width, int(d.textlength(lab, font=fr))) + 14
        xmax = max(xmax, x)
    Z = Z.crop((0, 0, xmax + 6, 34 + 3 * 164))
    _save_shot(r, Z, "appicon", "scales", f"The icon at fractional and 2x scales, Breeze {th}: " + "; ".join(picks))
    if th == "dark":
        _fact(r, "appicon-scales", "Scaled slots (the file Plasma's icon lookup takes on the real hicolor index, drawn at the device size): " + "; ".join(picks))
    # 3. large: 128 and 256 beside Elisa and Kate
    L = Image.new("RGB", (3 * (256 + 24) + 30, 30 + 256 + 24 + 128 + 30), B["window"])
    d = ImageDraw.Draw(L)
    d.text((10, 6), f"Breeze {th.capitalize()}: 256 and 128 px beside Elisa and Kate", fill=B["text"], font=fb)
    x = 14
    for nm in ("elisa", "ferrous", "kate"):
        for px, y0 in ((256, 30), (128, 30 + 256 + 20)):
            im_ = inst[px] if nm == "ferrous" else rend(_app_svg(nm, px) or _app_svg(nm, 48), px, f"app-{nm}")
            L.paste(im_, (x + (256 - px) // 2, y0), im_)
        x += 256 + 24
    _save_shot(r, L, "appicon", "large", f"The icon at 256 and 128 px beside Elisa and Kate, Breeze {th}")
    # tight versions for the combined sheet: the three at 256 px in a row; the three at 128 px in a 2x2 grid
    L2 = Image.new("RGB", (3 * 256 + 2 * 12, 256), B["window"])
    L3 = Image.new("RGB", (2 * 128 + 8, 2 * 128 + 8), B["window"])
    for k, nm in enumerate(("elisa", "ferrous", "kate")):
        i256 = inst[256] if nm == "ferrous" else rend(_app_svg(nm, 256) or _app_svg(nm, 48), 256, f"app-{nm}")
        i128 = inst[128] if nm == "ferrous" else rend(_app_svg(nm, 128) or _app_svg(nm, 48), 128, f"app-{nm}")
        L2.paste(i256, (k * 268, 0), i256)
        L3.paste(i128, ((k % 2) * 136, (k // 2) * 136), i128)
    _save_shot(r, L2, "appicon", "large256", f"Elisa, Ferrous, Kate at 256 px, Breeze {th}")
    _save_shot(r, L3, "appicon", "large128", f"Elisa, Ferrous (top), Kate (below) at 128 px, Breeze {th}")
    # 4. variants (the comparison)
    have = [v for v in ICON_VARIANTS if os.path.isdir(os.path.join(ICON_VARIANTS_DIR, v, "hicolor"))]
    if os.environ.get("SHOT_ICON_VARIANTS") == "1" and len(have) < len(ICON_VARIANTS):
        raise FileNotFoundError(f"appicon3: variants requested but missing: {[v for v in ICON_VARIANTS if v not in have]} "
                                f"under {ICON_VARIANTS_DIR}")
    if have:
        sets = [("installed", lambda px: inst[px] if px in inst else ico(a_root, px, "installed"))] + \
            [(v, (lambda root: (lambda px: ico(root, px, f"var-{os.path.basename(os.path.dirname(root))}")))(
                os.path.join(ICON_VARIANTS_DIR, v, "hicolor"))) for v in have] + \
            [(nm, (lambda nm_: (lambda px: rend(_app_svg(nm_, px) or _app_svg(nm_, 48), px, f"app-{nm_}")))(nm))
             for nm in ("labplot", "kmplot")]
        vsz = [32, 48, 64, 128] + ([256] if th == "dark" else [])
        cw_ = 256 + 16 if th == "dark" else 128 + 16
        V = Image.new("RGB", (30 + len(sets) * cw_, 30 + sum(vsz) + 22 * len(vsz) + 24), B["window"])
        d = ImageDraw.Draw(V)
        d.text((10, 6), f"Breeze {th.capitalize()}: installed vs {', '.join(have)}, beside LabPlot and KmPlot, 1:1", fill=B["text"], font=fb)
        for k, (nm, get) in enumerate(sets):
            x = 20 + k * cw_
            d.text((x, 26), nm, fill=B["text"], font=fb)
            y = 46
            for px in vsz:
                im_ = get(px)
                V.paste(im_, (x, y), im_)
                if th == "dark" and nm in have:
                    _fact(r, f"appicon-{nm}-{px}", f"Variant {nm} at {px} px: {_icon_measure2(im_, [('Breeze Dark window', _BREEZE['dark']['window'])])}")
                y += px + 22
        _save_shot(r, V, "appicon", "variants20", f"Installed vs {', '.join(have)} beside LabPlot and KmPlot, Breeze {th}")
    else:
        _fact(r, "appicon-variants", f"Variants: none rendered yet (no {', '.join(ICON_VARIANTS)} under {ICON_VARIANTS_DIR})"
              if ICON_VARIANTS else "Variants: none (SHOT_ICON_VARIANTS_DIR and SHOT_ICON_VARIANT_NAMES unset)")


def _icon_measure3(im):
    """the traces' geometry: per trace at 50 % across its vertical and perpendicular width (the vertical one times
    the cosine of its slope between 45 % and 55 %), its colour and contrast to the tile; whether a 4th trace shows; the
    top trace followed rightwards (its run in each column, overlapping the last): where it ends (exits the right inner
    edge or not), its rows there and the top air above it at that column; the rim's width."""
    import math
    import numpy as np
    a = np.asarray(im.convert("RGBA")).astype(int)
    rgb, op = a[..., :3], a[..., 3] > 200
    if not op.any():
        return "nothing opaque"
    H, W = op.shape
    tile = _colours(rgb[op], n=1)[0][0]
    stroke = op & (np.abs(rgb - np.array(tile)).sum(axis=2) > 90)
    # the rim: the opaque edge's band of a colour off the tile (2 px max at these sizes): strip it from the strokes
    ins = op.copy()
    for _ in range(max(1, H // 64)):
        ins = ins & np.roll(ins, 1, 0) & np.roll(ins, -1, 0) & np.roll(ins, 1, 1) & np.roll(ins, -1, 1)
    edge_band = op & ~ins
    rim_c = _colours(rgb[edge_band & stroke], n=1)[0][0] if (edge_band & stroke).any() else None
    stroke = stroke & ins
    def runs_at(x):
        c = stroke[:, x]
        out, y = [], 0
        while y < H:
            if c[y]:
                y0 = y
                while y < H and c[y]:
                    y += 1
                out.append((y0, y))
            else:
                y += 1
        return out
    xm, xl, xr = W // 2, int(W * 0.45), int(W * 0.55)
    rm, rl, rr = runs_at(xm), runs_at(xl), runs_at(xr)
    parts = []
    for k, (y0, y1) in enumerate(rm):
        cy = (y0 + y1) / 2
        near = lambda rs: min(rs, key=lambda q: abs((q[0] + q[1]) / 2 - cy)) if rs else None
        a_, b_ = near(rl), near(rr)
        slope = ((b_[0] + b_[1]) / 2 - (a_[0] + a_[1]) / 2) / max(1, xr - xl) if a_ and b_ else 0.0
        perp = (y1 - y0) * math.cos(math.atan(slope))
        c = tuple(int(v) for v in rgb[(y0 + y1) // 2, xm])
        parts.append(f"{_hexs(c)} {_contrast(c, tile):.2f}:1, {y1 - y0} px tall, {perp:.1f} px across (slope {slope:+.2f})")
    fourth = "4th trace present" if len(rm) >= 4 else f"{len(rm)} traces (no 4th)"
    top_txt = ""
    if rm:
        cur, x = rm[0], xm
        while x + 1 < W:
            nxt = [q for q in runs_at(x + 1) if q[0] <= cur[1] + 1 and q[1] >= cur[0] - 1]
            if not nxt:
                break
            cur, x = min(nxt, key=lambda q: q[0]), x + 1
        inner_cols = np.nonzero(ins[cur[0]])[0]
        at_edge = len(inner_cols) and x >= inner_cols[-1] - 1
        col_in = np.nonzero(ins[:, x])[0]
        air = cur[0] - col_in[0] if len(col_in) else 0
        top_txt = (f"; the top trace {'exits the right edge' if at_edge else 'ends'} at x {x} (of {W}) on rows {cur[0]}-{cur[1] - 1}, "
                   f"{air} px of air above it there")
    rim_txt = f"rim {_hexs(rim_c)}" if rim_c else "no rim colour"
    return f"tile {_hexs(tile)}, {rim_txt}; at 50 %: " + "; ".join(parts) + f"; {fourth}{top_txt}"


def _rim_width_px(im):
    import numpy as np
    a = np.asarray(im.convert("RGBA")).astype(int)
    rgb, op = a[..., :3], a[..., 3] > 200
    if not op.any():
        return None
    tile = _colours(rgb[op], n=1)[0][0]
    H, W = op.shape
    row = H // 2
    xs = np.nonzero(op[row])[0]
    if not len(xs):
        return None
    k = xs[0]
    while k < W and op[row, k] and np.abs(rgb[row, k] - np.array(tile)).sum() > 40:
        k += 1
    return k - xs[0]


def m_appicon4(r, arg):
    """appicon4: appicon/context4x (4x of the 16 px task-manager slot with the audio badge, the 24 px slot as Qt
    picks its file, a 48 px launcher cell and a 64 px Alt-Tab tile; below, Qt-style disabled renderings at 16-48 px
    beside Elisa's: KIconEffect's default for the disabled state, to grey and semi-transparent), appicon/wallpaper (a
    blue-violet desktop: desktop icons, an Overview-like grid, a 44 px floating panel with icons at 32 and 36 px);
    facts: per size the traces' geometry (_icon_measure3), the rim's width in device px per slot incl. @2."""
    r.s.draws_without_app_pixels()  # (SVGs and icon files; app pixels only from shots it names)
    from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageEnhance
    src = os.environ.get("SHOT_SRC", "")
    a_root = os.path.join(src, "ui/assets/icons/hicolor")
    raw = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "appicon-raw")
    os.makedirs(raw, exist_ok=True)
    th = r.s.tag
    B = _BREEZE[th]
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 13)
    fr = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 12)

    def rend(svg, px, tag):
        return _render_svg(r, svg, px, os.path.join(raw, f"{tag}-{px}.png"))

    def ico(px, dpr=1.0):
        svg, lab, dpx = _qt_pick(a_root, px, dpr)
        return rend(svg, dpx, f"pick-{lab.replace(', ', '-')}-{px}"), lab

    def app(nm, px):
        return rend(_app_svg(nm, px) or _app_svg(nm, 48), px, f"app-{nm}")
    if th == "dark":
        for px in (16, 22, 32, 48, 64, 128, 256):
            im_, lab = ico(px)
            _fact(r, f"appicon-geometry-{px}", f"Installed icon at {px} px ({lab}): {_icon_measure3(im_)}")
        rims = []
        for px in (16, 22, 24, 32, 48):
            for dpr in (1.0, 2.0):
                im_, lab = ico(px, dpr)
                rims.append(f"{px} px at {dpr:g}x ({lab}, {int(px * dpr)} device px): {_rim_width_px(im_)} px")
        _fact(r, "appicon-rims", "Rim width in device px per slot (the file Plasma's icon lookup takes): " + "; ".join(rims))
    # context at 4x, and the disabled renderings
    badge_svg = f"/usr/share/icons/{'breeze-dark' if th == 'dark' else 'breeze'}/status/16/audio-volume-high.svg"
    tiles = []
    s16 = Image.new("RGB", (28, 28), B["panel"])
    i16, _ = ico(16)
    s16.paste(i16, (6, 6), i16)
    bd = rend(badge_svg, 10, f"badge{th}")
    ImageDraw.Draw(s16).ellipse((6 + 16 - 6, 6 - 5 - 1, 6 + 16 + 6, 6 + 7), fill=B["panel"])
    s16.paste(bd, (6 + 16 - 5, 6 - 5), bd)
    tiles.append(("16 px slot + badge", s16))
    # (the 32 px slot of a 44 px panel in place of the 24 px one; 24 stays 1:1 in appicon/slots)
    i32, l32 = ico(32)
    s32 = Image.new("RGB", (44, 44), B["panel"])
    s32.paste(i32, (6, 6), i32)
    tiles.append((f"32 px slot ({l32})", s32))
    # the 32 px slot playing: Breeze's audio-volume-high at 0.4 of the icon (13 px), top right (the 48 px
    # launcher cell it replaces is 1:1 in appicon/panel)
    s32b = s32.copy()
    bd32 = rend(badge_svg.replace("/16/", "/22/") if os.path.exists(badge_svg.replace("/16/", "/22/")) else badge_svg,
                round(32 * 0.4), f"badge{th}-13")
    s32b.paste(bd32, (6 + 32 - bd32.width + 2, 6 - 2), bd32)
    tiles.append(("32 px slot, playing", s32b))
    i64, _ = ico(64)
    t64 = Image.new("RGB", (84, 84), B["panel"])
    ImageDraw.Draw(t64).rounded_rectangle((1, 1, 82, 82), radius=5, outline=(61, 174, 233), width=2)
    t64.paste(i64, (10, 10), i64)
    tiles.append(("64 px Alt-Tab tile", t64))
    W_ = 20 + sum(t.width * 4 + 16 for _, t in tiles)
    C = Image.new("RGB", (W_, 30 + 84 * 4 + 24 + 30 + 60 + 40), B["window"])
    d = ImageDraw.Draw(C)
    d.text((10, 6), f"Breeze {th.capitalize()}, 4x: the icon in its slots", fill=B["text"], font=fb)
    x = 10
    for lb, t in tiles:
        z = t.resize((t.width * 4, t.height * 4), Image.NEAREST)
        C.paste(z, (x, 30 + 84 * 4 - z.height))
        d.text((x, 30 + 84 * 4 + 4), lb, fill=B["text"], font=fr)
        x += z.width + 16
    y = 30 + 84 * 4 + 26
    picks = "; ".join(f"{dpr:g}x: {_qt_pick(a_root, 32, dpr)[1]}" for dpr in (1.0, 1.25, 1.5, 2.0))
    d.text((10, y), "disabled (KIconEffect default: to grey, semi-transparent), on the panel: Ferrous then Elisa, "
                    "16/22/32/48 px, normal and disabled", fill=B["text"], font=fr)
    y += 18
    d.rectangle((0, y, W_, y + 60), fill=B["panel"])
    x = 10
    for nm in ("ferrous", "elisa"):
        for px in (16, 22, 32, 48):
            im_ = ico(px)[0] if nm == "ferrous" else app(nm, px)
            g = im_.convert("LA").convert("RGBA")
            al = g.split()[3].point(lambda v: int(v * 0.5))
            g.putalpha(al)
            C.paste(im_, (x, y + 6 + 48 - px), im_)
            C.paste(g, (x + px + 4, y + 6 + 48 - px), g)
            x += 2 * px + 16
        x += 20
    C = C.crop((0, 0, W_, y + 64))
    _save_shot(r, C, "appicon", "context4x", f"4x of the 16 px slot (badge), the 32 px slot ({picks}) without and with the badge, a 64 px "
                                             f"tile; disabled renderings, Breeze {th}")
    # the wallpaper: desktop icons, an Overview-like grid, a 44 px floating panel
    import numpy as np
    Wp, Hp = 880, 520
    yy, xx = np.mgrid[0:Hp, 0:Wp]
    t_ = (xx / Wp * 0.6 + yy / Hp * 0.4)[..., None]
    bg = (np.array([24, 38, 92]) * (1 - t_) + np.array([96, 42, 140]) * t_)
    for cx_, cy_, rad, col in ((650, 140, 260, (140, 80, 200)), (180, 420, 220, (40, 90, 180))):
        g_ = np.exp(-(((xx - cx_) ** 2 + (yy - cy_) ** 2) / (2 * rad ** 2)))[..., None]
        bg = bg * (1 - 0.45 * g_) + np.array(col) * 0.45 * g_
    Wpi = Image.fromarray(np.clip(bg, 0, 255).astype("uint8")).filter(ImageFilter.GaussianBlur(2))
    d = ImageDraw.Draw(Wpi, "RGBA")
    d.text((12, 8), f"A Plasma-like desktop ({th} panel): desktop icons, an Overview-like grid, a floating panel",
           fill=(255, 255, 255), font=fb)
    for k, nm in enumerate(("system-file-manager", "ferrous", "elisa")):  # desktop icons, white labels with a shadow
        im_ = ico(48)[0] if nm == "ferrous" else app(nm, 48)
        y0 = 40 + k * 86
        Wpi.paste(im_, (30, y0), im_)
        lab = {"system-file-manager": "Files", "ferrous": "Ferrous", "elisa": "Elisa"}[nm]
        tw = d.textlength(lab, font=fr)
        d.text((54 - tw / 2 + 1, y0 + 53), lab, fill=(0, 0, 0, 160), font=fr)
        d.text((54 - tw / 2, y0 + 52), lab, fill=(255, 255, 255), font=fr)
    d.rounded_rectangle((180, 60, 840, 340), radius=12, fill=(20, 22, 28, 150))
    for k, nm in enumerate(["system-file-manager", "utilities-terminal", "elisa", "ferrous", "labplot", "kate", "okular", "systemsettings"]):
        im_ = ico(64)[0] if nm == "ferrous" else app(nm, 64)
        cx_, cy_ = 210 + (k % 4) * 150, 80 + (k // 4) * 130
        Wpi.paste(im_, (cx_ + 30, cy_), im_)
    pc = B["panel"]
    d.rounded_rectangle((120, Hp - 8 - 44, Wp - 120, Hp - 8), radius=10, fill=pc + (235,))
    x = 140
    for px, names in ((32, ("system-file-manager", "ferrous", "elisa", "kate")), (36, ("ferrous", "elisa", "labplot", "okular"))):
        for nm in names:
            im_ = ico(px)[0] if nm == "ferrous" else app(nm, px)
            yy0 = Hp - 8 - 44 + (44 - px) // 2
            Wpi.paste(im_, (x, yy0), im_)
            if nm == "ferrous":
                bdg = rend(badge_svg, 14, f"badge{th}")
                d.ellipse((x + px - 9, yy0 - 6, x + px + 7, yy0 + 10), fill=pc + (255,))
                Wpi.paste(bdg, (x + px - 8, yy0 - 5), bdg)
            x += px + 14
        d.text((x, Hp - 8 - 30), f"{px} px", fill=B["text"], font=fr)
        x += 60
    _save_shot(r, Wpi.convert("RGB"), "appicon", "wallpaper", f"The icon on a blue-violet desktop ({th} panel): desktop icons "
                                                              f"(48 px), an Overview-like grid (64 px), a 44 px floating panel (32 and 36 px)")


_CVD = {  # Machado, Oliveira and Fernandes 2009, severity 1.0, on linear RGB
    "deuteranopia": [[0.367322, 0.860646, -0.227968], [0.280085, 0.672501, 0.047413], [-0.011820, 0.042940, 0.968881]],
    "protanopia": [[0.152286, 1.052583, -0.204868], [0.114503, 0.786281, 0.099216], [-0.003882, -0.048116, 1.051998]],
}


def _cvd(img, kind):
    import numpy as np
    from PIL import Image
    a = np.asarray(img.convert("RGB")).astype(np.float64) / 255
    lin = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    out = np.clip(lin @ np.array(_CVD[kind]).T, 0, 1)
    srgb = np.where(out <= 0.0031308, out * 12.92, 1.055 * out ** (1 / 2.4) - 0.055)
    return Image.fromarray((srgb * 255 + 0.5).astype("uint8"))


def _icon_geom(im):
    """the installed icon's numbers for the table: rim width (px), trace count, each trace's width across (px, from
    its slope), its contrast to the tile, the top trace's exit rows and the air above it."""
    import math
    import numpy as np
    a = np.asarray(im.convert("RGBA")).astype(int)
    rgb, op = a[..., :3], a[..., 3] > 200
    H, W = op.shape
    tile = _colours(rgb[op], n=1)[0][0]
    stroke = op & (np.abs(rgb - np.array(tile)).sum(axis=2) > 90)
    ins = op.copy()
    for _ in range(max(1, H // 64)):
        ins = ins & np.roll(ins, 1, 0) & np.roll(ins, -1, 0) & np.roll(ins, 1, 1) & np.roll(ins, -1, 1)
    stroke = stroke & ins

    def runs_at(x):
        c = stroke[:, x]
        out, y = [], 0
        while y < H:
            if c[y]:
                y0 = y
                while y < H and c[y]:
                    y += 1
                out.append((y0, y))
            else:
                y += 1
        return out
    xm, xl, xr = W // 2, int(W * 0.45), int(W * 0.55)
    rm, rl, rr = runs_at(xm), runs_at(xl), runs_at(xr)
    widths, cons = [], []
    for y0, y1 in rm:
        cy = (y0 + y1) / 2
        near = lambda rs: min(rs, key=lambda q: abs((q[0] + q[1]) / 2 - cy)) if rs else None
        a_, b_ = near(rl), near(rr)
        slope = ((b_[0] + b_[1]) / 2 - (a_[0] + a_[1]) / 2) / max(1, xr - xl) if a_ and b_ else 0.0
        widths.append(round((y1 - y0) * math.cos(math.atan(slope)), 1))
        cons.append(round(_contrast(tuple(int(v) for v in rgb[(y0 + y1) // 2, xm]), tile), 2))
    exit_rows, air, exits = None, None, None
    if rm:
        cur, x = rm[0], xm
        while x + 1 < W:
            nxt = [q for q in runs_at(x + 1) if q[0] <= cur[1] + 1 and q[1] >= cur[0] - 1]
            if not nxt:
                break
            cur, x = min(nxt, key=lambda q: q[0]), x + 1
        ic = np.nonzero(ins[cur[0]])[0]
        col_in = np.nonzero(ins[:, x])[0]
        air = int(cur[0] - col_in[0]) if len(col_in) else None
        # where it leaves: the right inner edge, or the top one (the run reaches the column's first inner row)
        exits = "right" if len(ic) and x >= ic[-1] - 1 else ("top" if air is not None and air <= 1 else None)
        exit_rows = f"{cur[0]}-{cur[1] - 1}" if exits != "top" else f"column {x}"
    rw = _rim_width_px(im)
    return dict(rim=None if rw is None else int(rw), n=len(rm), widths=[float(v) for v in widths], contrast=[float(v) for v in cons],
                exit_rows=exit_rows, exits=exits, air=None if air is None else int(air))


def m_appicon5(r, arg):
    """appicon5: appicon/hinted (every hand-drawn file at nearest-neighbour 8x (16-24 px), 4x (32-48 px and the
    @2s up to 48 device px) or 2x (larger @2s), on the theme's window colour), appicon/slots (the 24 and 36 px slots at
    1:1 on a Breeze Dark panel beside Breeze icons at the same size; the title bar as KWin's Breeze decoration draws it,
    cut from this run's About window, 1:1 and 4x, with the icon's measured size; the icon at 48 and 128 px on the dark
    panel as seen with deuteranopia and protanopia); facts: the installed-only table rows (appicon-table-*), CVD
    contrasts, the title-bar icon size."""
    r.s.draws_without_app_pixels()  # (SVGs and icon files; app pixels only from shots it names)
    from PIL import Image, ImageDraw, ImageFont
    import numpy as np
    src = os.environ.get("SHOT_SRC", "")
    a_root = os.path.join(src, "ui/assets/icons/hicolor")
    raw = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "appicon-raw")
    os.makedirs(raw, exist_ok=True)
    th = r.s.tag
    B = _BREEZE[th]
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 13)
    fr = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 12)
    fs = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 11)
    rend = lambda svg, px, tag: _render_svg(r, svg, px, os.path.join(raw, f"{tag}-{px}.png"))
    # the table (installed only), once
    if th == "dark":
        for px in (16, 22, 24, 32, 36, 48, 64, 128, 256):
            svg, lab, dpx = _qt_pick(a_root, px, 1.0)
            g = _icon_geom(rend(svg, dpx, f"tab-{lab.replace(', ', '-')}"))
            # the centre lines from the SVG: angles, true widths, the fan's opening
            cl = _centre_lines(r, svg, dpx, raw, f"tab-{lab.replace(', ', '-')}-{px}")
            mids = [v["widths"][1] for v in sorted(cl["lines"].values(), key=lambda v: -v["rise"])]
            angs = [v["angles"][1] for v in sorted(cl["lines"].values(), key=lambda v: -v["rise"])]
            # the width ratios amber : rose : magenta at 50 % (magenta = 1); the top air from the coverage
            ln_ = cl["lines"]
            w3 = ln_.get("h3", {}).get("widths", [0, 0])[1]
            ratio = (" : ".join(f"{ln_[t]['widths'][1] / w3:.2f}" for t in ("fundamental", "h2", "h3") if t in ln_)
                     if w3 else None)
            ta = _top_air(r, svg, dpx, raw, f"tab-{lab.replace(', ', '-')}-{px}")
            g["air"] = None
            _fact(r, f"appicon-table-{px:03d}", f"{px} px", px=px, file=lab, **g, perp=mids, angles=angs,
                  fan=cl["fan"]["ratio"] if cl["fan"] else None, ratio=ratio, air_clear=ta["clear"],
                  air_faint=ta["faint"], air_col=ta["cols"],
                  spacing=[v for _, _, v in cl["spacing"]["pairs"]][::-1])  # (top pair first)
            _fact(r, f"appicon-lines-{px:03d}", f"Centre lines at {px} px ({lab}, from the SVG at ~1024 px): "
                  + _centre_lines_text(cl))
    # 1. the hinted files at nearest-neighbour zoom
    # (smaller zooms, so both themes fit one image: 4x up to 24 px, 3x for 32-48, 2x for the @2 files)
    ones = [(16, 3), (22, 3), (24, 3), (32, 2), (36, 2), (48, 2)]
    twos = [(16, 2), (22, 2), (24, 2), (32, 2), (36, 2), (48, 2), (64, 1)]
    rh = (96, 192)
    S = Image.new("RGB", (1300, 30 + rh[0] + 46 + rh[1] + 46 + 10), B["window"])
    d = ImageDraw.Draw(S)
    d.text((10, 6), f"Breeze {th.capitalize()}: every hand-drawn file, nearest neighbour (3x up to 24 px, 2x for 32-48 px "
                    "and the @2 files, 1x for 64@2); missing folders named", fill=B["text"], font=fb)
    missing = []
    xmax = 0
    for k, (row, scale) in enumerate(((ones, 1), (twos, 2))):
        y = 30 + (0 if k == 0 else rh[0] + 46)
        x = 10
        for px, z in row:
            folder = f"{px}x{px}" + (f"@{scale}" if scale > 1 else "")
            p_ = os.path.join(a_root, folder, "apps", "ferrous.svg")
            if not os.path.exists(p_):
                missing.append(folder)
                continue
            im_ = rend(p_, px * scale, f"hint-{folder}")
            bg = Image.new("RGB", im_.size, B["window"])
            bg.paste(im_, (0, 0), im_)
            zz = bg.resize((bg.width * z, bg.height * z), Image.NEAREST)
            S.paste(zz, (x, y + rh[k] - zz.height))
            d.text((x, y + rh[k] + 2), f"{folder} file, {z}x", fill=B["text"], font=fr)
            # the file vs the slot: what the px px slot takes at this scale, when it is another file
            taken = _kde_lookup(a_root)[(px, float(scale))][0] if (px, float(scale)) in _kde_lookup(a_root) else folder
            if taken != folder:
                ks = re.search(r"kSizes\{([\d,\s]+)\}", open(os.path.join(src, "ui/src/AppIcon.h")).read())
                win = scale == 1 and ks and px in [int(v) for v in ks.group(1).split(",")]
                d.text((x, y + rh[k] + 16), f"{px} px slot{' at 2x' if scale > 1 else ''}: {taken}", fill=B["text"], font=fs)
                if win:
                    d.text((x, y + rh[k] + 29), "(the window icon's file)", fill=B["text"], font=fs)
            x += max(zz.width, 120) + 14
        xmax = max(xmax, x)
    if missing:
        d.text((10, S.height - 16), "not drawn: " + ", ".join(missing), fill=B["text"], font=fr)
    S = S.crop((0, 0, max(xmax, 500), S.height))
    _save_shot(r, S, "appicon", "hinted", f"Every hand-drawn file at nearest-neighbour zoom, Breeze {th}"
                                          + (f"; not drawn: {', '.join(missing)}" if missing else ""))
    # 2. slots on the panel (24 and 36 beside Breeze icons), the title bar (per theme); colour vision (dark)
    D = _BREEZE[th]
    U = Image.new("RGB", (1180, 640), D["window"])
    d = ImageDraw.Draw(U)
    d.text((10, 6), f"Breeze {th.capitalize()} panel, 1:1: the 24 and 36 px slots beside Breeze icons at the same size",
           fill=D["text"], font=fb)
    y = 28
    for px in (24, 36):
        d.rectangle((0, y, 700, y + px + 16), fill=D["panel"])
        x = 12
        svg, lab, _ = _qt_pick(a_root, px, 1.0)
        for nm in ("system-file-manager", "elisa", "ferrous", "kate", "labplot", "okular"):
            im_ = rend(svg, px, f"slot-{lab.replace(', ', '-')}-{px}") if nm == "ferrous" else rend(_app_svg(nm, px) or _app_svg(nm, 48), px, f"app-{nm}")
            U.paste(im_, (x, y + 8), im_)
            x += px + 18
        d.text((x + 10, y + px // 2), f"{px} px (Ferrous from {lab})", fill=D["text"], font=fr)
        y += px + 28
    # the title bar from this run's About window (KWin's capture with its Breeze decoration)
    ap = os.path.join(os.environ["SHOT_OUT"], f"dialog_about-window_{r.s.suffix}.png")
    tb_note = "no About window shot"
    if os.path.exists(ap):
        r.s.source_shot(os.path.basename(ap))
        A = Image.open(ap).convert("RGB")
        a_ = np.asarray(A).astype(int)
        top = a_[:48, :80]
        # the icon: its purple (blue well above red and green), the window's 1-2 px border aside
        purple = (top[..., 2] > top[..., 0] + 30) & (top[..., 2] > top[..., 1] + 60)
        purple[:, :2] = False
        purple[:2, :] = False
        cols = np.nonzero(purple.any(axis=0))[0]
        rows_ = np.nonzero(purple.any(axis=1))[0]
        if len(cols) and len(rows_):
            iw, ih = int(cols.max() - cols.min() + 1), int(rows_.max() - rows_.min() + 1)
            tb_note = f"the window icon in KWin's Breeze title bar: {iw}x{ih} px (its tile) at x {cols.min()}, y {rows_.min()}"
        tb = A.crop((0, 0, 220, 36))
        U.paste(tb, (10, y + 18))
        U.paste(tb.resize((tb.width * 4, tb.height * 4), Image.NEAREST), (250, y + 18))
        d.text((10, y), "the title bar, 1:1 and 4x: " + tb_note, fill=D["text"], font=fr)
        y += 36 * 4 + 30
    if th == "dark":
        _fact(r, "appicon-titlebar", "Title bar: " + tb_note + " (KWin's own Breeze decoration in this headless KWin at "
                                     "scale 1 and the default font: what a default Plasma session draws; other button sizes differ)")
    U = U.crop((0, 0, 1140, y))
    _save_shot(r, U, "appicon", "slots", f"The 24 and 36 px slots beside Breeze icons, the title bar, Breeze {th}")
    if th != "dark":
        return
    D = _BREEZE["dark"]
    U = Image.new("RGB", (1180, 360), D["window"])
    d = ImageDraw.Draw(U)
    y = 6
    d.text((10, y), "Colour vision, on the dark panel: as seen, deuteranopia, protanopia (Machado 2009, severity 1); "
                    "16 and 22 px as seen and with protanopia", fill=D["text"], font=fb)
    y += 22
    x, xmax = 10, 0
    cvd_facts = []
    # (16 and 22 px with protanopia too; 128 px on its own row)
    for px in (16, 22, 32, 48, 128):
        if px == 128:
            xmax, x, y = max(xmax, x), 10, y + 48 + 16 + 40
        svg, lab, dpx = _qt_pick(a_root, px, 1.0)
        im_ = rend(svg, dpx, f"cvd-{lab.replace(', ', '-')}-{px}")
        base = Image.new("RGB", (px + 16, px + 16), D["panel"])
        base.paste(im_, (8, 8), im_)
        for kind in ((None, "protanopia") if px < 32 else (None, "deuteranopia", "protanopia")):
            v = base if kind is None else _cvd(base, kind)
            U.paste(v, (x, y))
            d.text((x, y + px + 18), kind or f"{px} px, as seen", fill=D["text"], font=fr)
            if px in (16, 22, 32, 128):
                vv = Image.new("RGBA", (px, px))
                vv.paste(v.crop((8, 8, 8 + px, 8 + px)))
                vv.putalpha(im_.split()[3])
                g = _icon_geom(vv)
                cvd_facts.append(f"{px} px {kind or 'as seen'}: traces {g['contrast']}:1 to the tile")
            x += max(px + 16, 80) + 14
        x += 20
    _fact(r, "appicon-cvd", "Colour vision at 16, 22, 32 and 128 px on the dark panel (each trace's contrast to the tile, top "
          "first): " + "; ".join(cvd_facts))
    U = U.crop((0, 0, max(x, xmax, 720), y + 128 + 40))
    _save_shot(r, U, "appicon", "cvd", "The icon at 16-128 px as seen and with protanopia (deuteranopia from 32 px), Breeze dark")


_TRACE_IDS = ("fundamental", "h2", "h3", "h4")


def _hex2rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def _svg_ids(svg):
    import xml.etree.ElementTree as ET
    return [e.get("id") for e in ET.parse(svg).getroot().iter() if e.get("id")]


def _svg_attr(svg, eid, attr):
    import xml.etree.ElementTree as ET
    for e in ET.parse(svg).getroot().iter():
        if e.get("id") == eid:
            return e.get(attr)
    return None


def _svg_only(svg, keep_ids, out):
    """a copy of an icon SVG drawing only the elements `keep_ids`, filled (and stroked) white: rendered, its alpha
    is their coverage. Definitions (defs, clip paths, masks, gradients) stay; every other drawable element goes."""
    import xml.etree.ElementTree as ET
    ET.register_namespace("", "http://www.w3.org/2000/svg")
    tree = ET.parse(svg)
    keep_ids = set(keep_ids)
    defs = {"defs", "clipPath", "mask", "linearGradient", "radialGradient", "style", "title", "desc", "metadata"}
    def prune(el):
        for ch in list(el):
            ln = ch.tag.split("}")[-1]
            if ln in defs:
                continue
            if ch.get("id") in keep_ids:
                ch.set("fill", "#ffffff")
                ch.attrib.pop("opacity", None)
                ch.attrib.pop("fill-opacity", None)
                if ch.get("stroke") not in (None, "none"):
                    ch.set("stroke", "#ffffff")
            elif any(e.get("id") in keep_ids for e in ch.iter()):
                prune(ch)
            else:
                el.remove(ch)
    prune(tree.getroot())
    tree.write(out)
    return out


def _cov(r, svg, ids, px, raw, tag):
    """The coverage (0-1, float array) of the elements `ids` of `svg` drawn at `px`."""
    import numpy as np
    m = _svg_only(svg, ids, os.path.join(raw, f"{tag}-only.svg"))
    return np.asarray(_render_svg(r, m, px, os.path.join(raw, f"{tag}-only-{px}.png"))).astype(float)[..., 3] / 255.0


def _viewbox_scale(svg, px):
    import xml.etree.ElementTree as ET
    root = ET.parse(svg).getroot()
    vb = root.get("viewBox")
    w = float(vb.split()[2]) if vb else float(root.get("width"))
    return px / w


def _trace_colour(r, svg, px, raw, tag, tid="h3"):
    """a trace's declared fill and its rendered colour: the mean of the pixels it covers at least half, apart
    from the other traces (each over 5 % covered there); also how many of its pixels it covers fully."""
    import numpy as np
    ids = _svg_ids(svg)
    if tid not in ids:
        return None
    real = np.asarray(_render_svg(r, svg, px, os.path.join(raw, f"{tag}-{px}.png"))).astype(float)
    c = _cov(r, svg, [tid], px, raw, f"{tag}-{tid}")
    others = [i for i in _TRACE_IDS if i in ids and i != tid]
    o = _cov(r, svg, others, px, raw, f"{tag}-not{tid}") if others else np.zeros_like(c)
    sel = (c >= 0.5) & (o < 0.05)
    if not sel.any():
        return None
    mean = tuple(int(round(v)) for v in real[..., :3][sel].mean(axis=0))
    ys, xs = np.nonzero(c > 0.05)
    return dict(fill=_svg_attr(svg, tid, "fill"), tile=_svg_attr(svg, "tile", "fill"), mean=mean, n=int(sel.sum()),
                full=int((c >= 0.98).sum()), centre=(int(xs.mean()), int(ys.mean())), real=real.astype("uint8"))


def _h4_end(r, svg, px, raw, tag):
    """the 4th harmonic's end at the tile's top edge, from its own coverage: per column its top edge (the first
    row it covers, less that row's uncovered part) against the tile's top inner edge (the tile rect's y): the air under
    the rim. The cut: the columns where the air is under 0.5 px; the approach: the 12 columns before it; the end: its
    last column, the gap to the right inner edge and how far down its lower edge reaches there. (dict, crop box)"""
    import numpy as np
    if "h4" not in _svg_ids(svg):
        return None
    k = _viewbox_scale(svg, px)
    yt = float(_svg_attr(svg, "tile", "y")) * k
    xr = (float(_svg_attr(svg, "tile", "x")) + float(_svg_attr(svg, "tile", "width"))) * k
    c = _cov(r, svg, ["h4"], px, raw, f"{tag}-h4")
    cols = np.nonzero(c.max(axis=0) > 0.05)[0]
    air, low = {}, {}
    for x in cols:
        col = c[:, x]
        ys = np.nonzero(col > 0.02)[0]
        y0, y1 = ys[0], ys[-1]
        air[int(x)] = round(float(y0 + (1 - col[y0]) - yt), 2)
        low[int(x)] = round(float(y1 + col[y1] - yt), 2)
    xs = sorted(air)
    near = [x for x in xs if air[x] < 2.0]
    out = dict(px=px, top=round(yt, 2), end=int(xs[-1]), gap_right=round(xr - (xs[-1] + 1), 2), end_low=low[xs[-1]])
    if near:
        # under the end: from the first column within 2 px of the tile's top through the last column it covers
        a = near[0]
        cols_ = [x for x in xs if x >= a]
        prof = [air[x] for x in cols_]
        cut = [x for x in cols_ if air[x] < 0.5]
        out.update(start=a, profile=prof, wedge=round(sum(max(0.0, v) for v in prof), 2), air_max=max(prof),
                   at_max=cols_[prof.index(max(prof))], cut=_ranges(cut) if cut else None, cut_px=len(cut),
                   over=round(-min(prof), 2) if min(prof) < 0 else 0.0)
        lo = max(low[x] for x in cols_)
        box = (max(0, a - 12), max(0, int(yt) - 6), min(c.shape[1], int(xs[-1]) + 10), min(c.shape[0], int(yt + lo) + 4))
    else:
        out.update(start=None, end_air=air[xs[-1]])
        box = (max(0, int(xs[-1]) - 30), max(0, int(yt) - 6), min(c.shape[1], int(xs[-1]) + 10), min(c.shape[0], int(yt) + 20))
    return out, box


def _h4_end_text(m):
    if m.get("start") is None:
        return (f"{m['px']} px: never within 2 px of the tile's top; it ends at x {m['end']}, {m['end_air']} px under "
                f"it, {m['gap_right']} px short of the right inner edge")
    t = (f"{m['px']} px, the tile's top inner edge at y {m['top']}: under the end (x {m['start']}-{m['end']}, from where it "
         f"comes within 2 px of that edge through its last column) the air between the edge and the trace, per column: "
         f"{', '.join(f'{v:g}' for v in m['profile'])} px; the wedge of tile there {m['wedge']} px², at most "
         f"{m['air_max']} px (x {m['at_max']}); ")
    t += (f"flush (under 0.5 px) over {m['cut_px']} px (x {m['cut']})" if m["cut"] else "flush nowhere (never under 0.5 px)")
    if m["over"]:
        t += f", over the edge onto the rim by up to {m['over']} px"
    return t + (f"; its last column {m['gap_right']} px short of the right inner edge, its lower edge {m['end_low']} px "
                "under the tile's top there")


def _ranges(v):
    v = sorted(v)
    out, a = [], None
    for i, x in enumerate(v):
        if a is None:
            a = x
        if i + 1 == len(v) or v[i + 1] != x + 1:
            out.append(f"{a}" if a == x else f"{a}-{x}")
            a = None
    return ", ".join(out)


def m_appicon6(r, arg):
    """appicon6: appicon/h3strip (once, dark: the 3rd harmonic in every file, in order, the before snapshot's
    below: the file 1:1 (a 64 px window around the trace in the larger ones), its declared fill as a solid bar on the
    tile and its rendered colour (the mean of the pixels it covers at least half) as a bar, each with its contrast to
    the tile; then the 4th harmonic's end at 4x where a file has one, with the air under the rim across the cut);
    appicon/about128 (per theme: an About page in KDE's layout, the icon at 128 px as Plasma's lookup takes it, beside
    the app's own About header). Facts (once): appicon-h3-*, appicon-h4end-*, appicon-lookup-files (the files the
    lookup never takes at the folder sizes, where else it takes them; the window icon's and the About dialog's
    files), appicon-lookup-engine (what the lookup emulates, checked against KIconLoader itself; where Qt's own
    QIconLoader differs)."""
    r.s.draws_without_app_pixels()  # (SVGs and icon files; app pixels only from shots it names)
    from PIL import Image, ImageDraw, ImageFont
    import subprocess
    src = os.environ.get("SHOT_SRC", "")
    a_root = os.path.join(src, "ui/assets/icons/hicolor")
    b_root = None if not ICON_BEFORE_DIR else os.path.join(ICON_BEFORE_DIR, "hicolor")
    if b_root and not os.path.isdir(b_root):
        raise FileNotFoundError(f"appicon6: no 'before' icon at {b_root} (unset SHOT_ICON_BEFORE to leave it out)")
    raw = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "appicon-raw")
    os.makedirs(raw, exist_ok=True)
    th = r.s.tag
    B = _BREEZE[th]
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 13)
    fr = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 12)
    fs = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 11)

    def folders(root):
        fs_ = [d for d in os.listdir(root) if os.path.isfile(os.path.join(root, d, "apps", "ferrous.svg"))]
        def key(d):
            if d == "scalable":
                return (10 ** 6, 0)
            m = re.match(r"^(\d+)x\d+(@(\d+))?$", d)
            return (int(m.group(1)), int(m.group(3) or 1))
        return sorted(fs_, key=key)

    def own_px(d):
        if d == "scalable":
            return 64
        m = re.match(r"^(\d+)x\d+(@(\d+))?$", d)
        return int(m.group(1)) * int(m.group(3) or 1)
    if th == "dark":
        # 1. the lookup: the files it never takes at the folder sizes; the engine, checked
        sizes = tuple(range(16, 257))
        dprs = (1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0)
        kde = _kde_lookup(a_root, "kde", sizes, dprs)
        kil = _kde_lookup(a_root, "kiconloader", sizes, dprs)
        qt = _kde_lookup(a_root, "qt", _KDE_SIZES, _KDE_DPRS)
        slot_sizes = sorted({own_px(d) // (2 if "@" in d else 1) for d in folders(a_root) if d != "scalable"} | {96})
        at_slots, elsewhere = {}, {}
        for (sz, dpr), (f, _) in kde.items():
            (at_slots if sz in slot_sizes else elsewhere).setdefault(f, {}).setdefault(dpr, []).append(sz)
        app_icon_h = open(os.path.join(src, "ui/src/AppIcon.h")).read()
        m = re.search(r"kSizes\{([\d,\s]+)\}", app_icon_h)
        if not m:
            raise LookupError("appicon6: no kSizes in ui/src/AppIcon.h")
        ksizes = [int(v) for v in m.group(1).split(",")]
        about_qml = open(os.path.join(src, "ui/qml/dialogs/AboutDialog.qml")).read()
        about_files = sorted(set(re.findall(r"qrc:/icons/hicolor/([^/\"]+)/apps/ferrous\.svg", about_qml)))
        rows = []
        for f in folders(a_root):
            used = at_slots.get(f)
            other = elsewhere.get(f, {})
            where = "; ".join(f"{d:g}x: {_ranges(v)} px" for d, v in sorted(other.items()))
            extra = []
            if f in [f"{s}x{s}" for s in ksizes]:
                extra.append("the window icon's own pixmap (FerrousAppIcon::icon, kSizes)")
            if f in about_files:
                extra.append("the About dialog (its qrc path)")
            if used:
                continue
            rows.append(f"{f}: never at the folder sizes; elsewhere only at {where or 'no size 16-256 px'}"
                        + (f"; used by {', '.join(extra)}" if extra else "; used by nothing else"))
        _fact(r, "appicon-lookup-files",
              f"Files Plasma's icon lookup never takes at the folder sizes ({', '.join(map(str, slot_sizes))} px at "
              f"{', '.join(f'{d:g}' for d in dprs)}x): " + (" | ".join(rows) if rows else "none")
              + f". The window icon: kSizes {ksizes} as pixmaps of the 1x files; the About dialog: {', '.join(about_files) or 'no qrc file'}.")
        plug = "/usr/lib64/qt6/plugins/platformthemes/KDEPlasmaPlatformTheme6.so"
        syms = subprocess.run(["nm", "-DC", plug], capture_output=True, text=True).stdout if os.path.exists(plug) else ""
        uses_kie = "KIconEngine::KIconEngine(QString const&, KIconLoader*)" in syms and "KIconLoader::global()" in syms
        if not uses_kie:
            raise LookupError(f"appicon6: {plug} does not show KIconEngine on KIconLoader::global() (nm)")
        diff = [c for c in kde if kde[c] != kil[c]]
        qd = [f"{s} px at {d:g}x: Qt {qt[(s, d)][0]}, Plasma {kde[(s, d)][0]}" for (s, d) in sorted(qt) if qt[(s, d)][0] != kde[(s, d)][0]]
        _fact(r, "appicon-lookup-engine",
              "The lookup the harness names is QIcon::fromTheme under Plasma's platform theme (QT_QPA_PLATFORMTHEME=kde: "
              "KDEPlasmaPlatformTheme6 creates a KIconEngine on KIconLoader::global(), as its symbols show), so KDE's "
              f"KIconLoader decides, not Qt's QIconLoader. Checked: KIconLoader::loadScaledIcon called directly takes "
              f"the same file in {len(kde) - len(diff)} of {len(kde)} cases (16-256 px at {', '.join(f'{d:g}' for d in dprs)}x)"
              + (f"; differs at {', '.join(f'{s}@{d:g}' for s, d in diff[:12])}" if diff else "")
              + ". Qt's own QIconLoader (a session without the KDE platform theme) differs at: " + ("; ".join(qd) or "nothing"))
        if diff:
            raise AssertionError(f"appicon6: KIconLoader and QIcon::fromTheme under Plasma disagree at {diff[:6]}")
        # 2. the 3rd harmonic's colour in every file, in order; the 4th harmonic's end
        sets = [("installed", a_root)]  # (installed only; the history is in one image)
        cw, lw = 60, 76  # (narrower, to share a sheet)
        nmax = max(len(folders(rt)) for _, rt in sets)
        bh = 16 + 64 + 6 + 2 * (24 + 28) + 8
        S = Image.new("RGB", (lw + nmax * cw + 10, 30 + len(sets) * bh + 10), B["window"])
        d = ImageDraw.Draw(S)
        d.text((10, 6), "The 3rd harmonic in every installed file, in order: the file 1:1 (a 64 px window in larger ones); "
                        "declared fill and rendered colour as bars on the tile, with contrast", fill=B["text"], font=fb)
        ends = []
        for k, (nm, rt) in enumerate(sets):
            y0 = 30 + k * bh
            d.text((10, y0 + 18), nm, fill=B["text"], font=fb)
            d.text((10, y0 + 16 + 64 + 14), "declared", fill=B["text"], font=fs)
            d.text((10, y0 + 16 + 64 + 6 + 52 + 8), "rendered", fill=B["text"], font=fs)
            facts = []
            for j, f in enumerate(folders(rt)):
                svg = os.path.join(rt, f, "apps", "ferrous.svg")
                px = own_px(f) if f != "scalable" else 64
                tag = f"h3-{'b' if rt == b_root else 'a'}-{f}"
                tc = _trace_colour(r, svg, px, raw, tag)
                x = lw + j * cw
                d.text((x, y0), f, fill=B["text"], font=fs)
                if tc is None:
                    d.text((x, y0 + 30), "no 3rd", fill=B["text"], font=fs)
                    facts.append(f"{f}: no h3")
                    continue
                im_ = Image.fromarray(tc["real"], "RGBA")
                if im_.width > 64:
                    cx, cy = tc["centre"]
                    l_ = min(max(0, cx - 32), im_.width - 64)
                    t_ = min(max(0, cy - 32), im_.height - 64)
                    im_ = im_.crop((l_, t_, l_ + 64, t_ + 64))
                bg = Image.new("RGB", im_.size, B["window"])
                bg.paste(im_, (0, 0), im_)
                S.paste(bg, (x, y0 + 16 + 64 - bg.height))
                tile = _hex2rgb(tc["tile"])
                fill = _hex2rgb(tc["fill"])
                for q, (col, lab) in enumerate(((fill, tc["fill"]), (tc["mean"], _hexs(tc["mean"])))):
                    yy = y0 + 16 + 64 + 6 + q * 52
                    d.rectangle((x, yy, x + cw - 8, yy + 24), fill=tile)
                    d.rectangle((x + 4, yy + 8, x + cw - 12, yy + 16), fill=col)
                    d.text((x, yy + 26), f"{_contrast(col, tile):.2f}:1", fill=B["text"], font=fs)
                facts.append(f"{f}: {tc['fill']} {_contrast(fill, tile):.2f}:1 declared, {_hexs(tc['mean'])} "
                             f"{_contrast(tc['mean'], tile):.2f}:1 rendered ({tc['n']} px at least half covered, {tc['full']} fully)")
                e = _h4_end(r, svg, px, raw, tag) if f in ("128x128", "256x256") else None
                if e:
                    full = _render_svg(r, svg, px, os.path.join(raw, f"{tag}-end-{px}.png"))
                    ends.append((f"{nm}, {f}", e[0], full.crop(e[1])))
            _fact(r, f"appicon-h3-{'before' if rt == b_root else 'installed'}",
                  f"The 3rd harmonic ({nm}), the tile {_svg_attr(os.path.join(rt, '16x16', 'apps', 'ferrous.svg'), 'tile', 'fill')}: "
                  + "; ".join(facts))
        for lab, e, _ in ends:
            _fact(r, f"appicon-h4end-{'b' if 'before' in lab else 'a'}-{e['px']:03d}", f"4th harmonic's end ({lab}): " + _h4_end_text(e))
        # the ends at 4x below the strip
        # (the 4th harmonic's end is magnified in appicon/h4true; its facts stay)
        _save_shot(r, S, "appicon", "h3strip", "The 3rd harmonic's colour in every installed file (declared vs rendered, "
                                               "contrast to the tile)")
    # 3. an About page at 128 px (KDE's layout), beside the app's own About header
    svg, lab, dpx = _qt_pick(a_root, 128, 1.0)
    ic = _render_svg(r, svg, dpx, os.path.join(raw, f"about128-{dpx}.png"))
    ver = re.search(r'^version\s*=\s*"([^"]+)"', open(os.path.join(src, "Cargo.toml")).read(), re.M)
    desc = re.search(r'"@info", "(A music player[^"]*)"', open(os.path.join(src, "ui/qml/dialogs/AboutDialog.qml")).read())
    A = Image.new("RGB", (620, 176), B["window"])
    d = ImageDraw.Draw(A)
    A.paste(ic, (24, 24), ic)
    ft = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 22)
    d.text((24 + 128 + 20, 40), "Ferrous", fill=B["text"], font=ft)
    d.text((24 + 128 + 20, 76), f"Version {ver.group(1) if ver else '?'}", fill=B.get("muted", B["text"]), font=fr)
    d.text((24 + 128 + 20, 104), desc.group(1) if desc else "", fill=B["text"], font=fr)
    ap = os.path.join(os.environ["SHOT_OUT"], f"dialog_about-window_{r.s.suffix}.png")
    if os.path.exists(ap):
        r.s.source_shot(os.path.basename(ap))
        real = Image.open(ap).convert("RGB")
        hdr = real.crop((0, 36, min(real.width, 300), 104))
        A2 = Image.new("RGB", (A.width + hdr.width + 30, A.height + 20), B["window"])
        A2.paste(A, (0, 0))
        A2.paste(hdr, (A.width + 20, 24))
        ImageDraw.Draw(A2).text((A.width + 20, 6), "the app's own About header (48 px)", fill=B["text"], font=fs)
        ImageDraw.Draw(A2).text((24, A.height), f"About page, 128 px ({lab}): name, version, description in KDE's "
                                                "header layout", fill=B["text"], font=fs)
        A = A2
    _save_shot(r, A, "appicon", "about128", f"An About page with the icon at 128 px ({lab}), beside the app's own About "
                                            f"header, Breeze {th}")


def _centre_lines(r, svg, dpx, raw, tag):
    """each trace's centre line from the SVG itself: its own coverage (an SVG copy drawing only it) rendered at
    ~1024 px, per column the coverage-weighted centre and the covered height, in the slot's device px (dpx). Columns
    under 90 % of the trace's median height (the round cap, a cut end) are left out. Per trace at 10/50/90 % of that
    span: the rise angle (from a straight fit over 12 % of the span) and the true (perpendicular) width = covered height
    x cos(angle); the rise over the span. The fan: the traces' centre-line spacing at the first column where all are
    full (start) and at the last one where all that reach the right inner edge are (exit), the ratio exit / start."""
    import math
    import numpy as np
    ids = [t for t in _TRACE_IDS if t in _svg_ids(svg)]
    S = max(4, int(math.ceil(1024 / dpx)))
    k = _viewbox_scale(svg, dpx)
    tx1 = (float(_svg_attr(svg, "tile", "x")) + float(_svg_attr(svg, "tile", "width"))) * k
    lines = {}
    for t in ids:
        c = _cov(r, svg, [t], dpx * S, raw, f"{tag}-cl-{t}")
        cs = c.sum(axis=0)
        allc = np.nonzero(cs > 0.05 * S)[0]
        if not len(allc):
            continue
        last_px = (allc[-1] + 1) / S
        # the round cap at the start: leave out one median width from the first column; a cut end (the top edge):
        # leave out the trailing columns under half the height half way along (the traces taper, so no global floor)
        med = float(np.median(cs[allc]))
        cols = allc[allc >= allc[0] + int(math.ceil(med))]
        mid_h = cs[cols[len(cols) // 2]]
        while len(cols) > 3 and cs[cols[-1]] < 0.5 * mid_h:
            cols = cols[:-1]
        ys = np.arange(c.shape[0])[:, None] + 0.5
        cen = (c[:, cols] * ys).sum(axis=0) / cs[cols] / S
        xv = (cols + 0.5) / S
        tv = cs[cols] / S
        span = xv[-1] - xv[0]
        def at(f):
            x0 = xv[0] + f * span
            sel = np.abs(xv - x0) <= 0.06 * span
            sl = np.polyfit(xv[sel], cen[sel], 1)[0] if sel.sum() >= 3 else 0.0
            ang = math.degrees(math.atan(-sl))
            w = float(np.interp(x0, xv, tv)) * math.cos(math.atan(sl))
            return round(ang, 1), round(w, 2)
        pts = [at(f) for f in (0.1, 0.5, 0.9)]
        lines[t] = dict(x0=round(float(xv[0]), 2), x1=round(float(xv[-1]), 2), angles=[p[0] for p in pts],
                        widths=[p[1] for p in pts], rise=round(float(cen[0] - cen[-1]), 2),
                        reaches_right=bool(last_px >= tx1 - 0.5), _x=xv, _c=cen)
    fan = None
    full = [t for t in lines if lines[t]["reaches_right"]]
    if len(full) >= 2:
        xs_ = max(lines[t]["x0"] for t in full)
        xe_ = min(lines[t]["x1"] for t in full)
        ys_s = sorted(float(np.interp(xs_, lines[t]["_x"], lines[t]["_c"])) for t in full)
        ys_e = sorted(float(np.interp(xe_, lines[t]["_x"], lines[t]["_c"])) for t in full)
        sp_s = [round(b - a, 2) for a, b in zip(ys_s, ys_s[1:])]
        sp_e = [round(b - a, 2) for a, b in zip(ys_e, ys_e[1:])]
        fan = dict(x_start=round(xs_, 2), x_exit=round(xe_, 2), start=sp_s, exit=sp_e,
                   ratio=round((ys_e[-1] - ys_e[0]) / max(1e-6, ys_s[-1] - ys_s[0]), 2),
                   pairs=[round(e / max(1e-6, s_), 2) for s_, e in zip(sp_s, sp_e)], traces=full)
    # the spacing between neighbouring centre lines half way across the tile's inner width
    tx0 = float(_svg_attr(svg, "tile", "x")) * k
    xm = (tx0 + tx1) / 2
    at_m = sorted((float(np.interp(xm, v["_x"], v["_c"])), t) for t, v in lines.items() if v["_x"][0] <= xm <= v["_x"][-1])
    spacing = dict(x=round(xm, 2), pairs=[(b_[1], a_[1], round(b_[0] - a_[0], 2)) for a_, b_ in zip(at_m, at_m[1:])])
    arrays = {t: (v.pop("_x"), v.pop("_c")) for t, v in lines.items()}
    return dict(lines=lines, fan=fan, spacing=spacing, arrays=arrays)


def _centre_lines_text(cl):
    names = {"fundamental": "glide", "h2": "2nd", "h3": "3rd", "h4": "4th"}
    parts = []
    for t, v in cl["lines"].items():
        parts.append(f"{names.get(t, t)}: rise angle {v['angles'][0]}/{v['angles'][1]}/{v['angles'][2]} deg at 10/50/90 %, "
                     f"width ⟂ {v['widths'][0]}/{v['widths'][1]}/{v['widths'][2]} px, rises {v['rise']} px over x "
                     f"{v['x0']}-{v['x1']}" + ("" if v["reaches_right"] else " (leaves before the right edge)"))
    f = cl["fan"]
    fan = (f"; fan ({'/'.join(names.get(t, t) for t in f['traces'])}): spacing at x {f['x_start']} {f['start']} px, at x "
           f"{f['x_exit']} {f['exit']} px, opening {f['ratio']}x (per pair {f['pairs']})") if f else "; no fan (fewer than 2 traces reach the right edge)"
    sp = cl.get("spacing")
    spt = (f"; spacing half way across (x {sp['x']}): " + ", ".join(f"{names.get(a, a)}-{names.get(b, b)} {v} px"
                                                                     for a, b, v in sp["pairs"])) if sp and sp["pairs"] else ""
    return "; ".join(parts) + fan + spt


def _glyph_lines(svg, px):
    """the mode glyph's strokes from its SVG paths (M then cubic C segments): stroke width and rise in device px
    at `px`, the rise angle at its start, middle and end (from the curve's tangents), the gap between the strokes'
    starts. Bottom stroke first."""
    import math
    import xml.etree.ElementTree as ET
    root = ET.parse(svg).getroot()
    vb = root.get("viewBox")
    k = px / (float(vb.split()[2]) if vb else float(root.get("width")))
    out = []
    for e in root.iter():
        if not e.tag.endswith("path") or not e.get("d"):
            continue
        nums = [float(v) for v in re.findall(r"-?\d*\.?\d+(?:e-?\d+)?", e.get("d"))]
        if not re.match(r"^\s*M", e.get("d")) or "C" not in e.get("d") or (len(nums) - 2) % 6:
            continue
        p0 = (nums[0], nums[1])
        segs, cur = [], p0
        for i in range(2, len(nums), 6):
            c1, c2, p3 = (nums[i], nums[i + 1]), (nums[i + 2], nums[i + 3]), (nums[i + 4], nums[i + 5])
            segs.append((cur, c1, c2, p3))
            cur = p3
        def tan(sg, t):
            a, b, c, d = sg
            dx = 3 * (1 - t) ** 2 * (b[0] - a[0]) + 6 * (1 - t) * t * (c[0] - b[0]) + 3 * t ** 2 * (d[0] - c[0])
            dy = 3 * (1 - t) ** 2 * (b[1] - a[1]) + 6 * (1 - t) * t * (c[1] - b[1]) + 3 * t ** 2 * (d[1] - c[1])
            return round(math.degrees(math.atan2(-dy, dx)), 1)
        mid = segs[len(segs) // 2]
        out.append(dict(start=(round(p0[0] * k, 2), round(p0[1] * k, 2)), end=(round(cur[0] * k, 2), round(cur[1] * k, 2)),
                        width=round(float(e.get("stroke-width", "1")) * k, 2), rise=round((p0[1] - cur[1]) * k, 2),
                        angles=[tan(segs[0], 0.0), tan(mid, 0.5), tan(segs[-1], 1.0)]))
    out.sort(key=lambda v: -v["start"][1])
    return out


def _glyph_lines_text(gl, px):
    names = ["lower", "upper", "third"]
    t = [f"{names[i] if i < 3 else i}: {v['width']} px stroke, rises {v['rise']} px (angle {v['angles'][0]}/{v['angles'][1]}/"
         f"{v['angles'][2]} deg at start/middle/end), from ({v['start'][0]}, {v['start'][1]}) to ({v['end'][0]}, {v['end'][1]})"
         for i, v in enumerate(gl)]
    if len(gl) >= 2:
        t.append(f"starts {round(gl[0]['start'][1] - gl[1]['start'][1], 2)} px apart, ends {round(gl[0]['end'][1] - gl[1]['end'][1], 2)} px apart")
    return f"at {px} px: " + "; ".join(t)


def _plasma_frame(r, raw, prefix, w, h, th):
    """a Plasma frame (FrameSvg's nine parts, `prefix`-topleft … -center) from the Breeze Plasma theme's
    widgets/tasks.svgz, its ColorScheme classes resolved to Breeze's colours for the theme (Text per theme;
    ButtonFocus #3daee9, ButtonHover #93cee9), each part rendered alone at the theme's 1 unit = 1 px and cut to its
    drawn pixels, then laid out as FrameSvg does: corners as they are, edges stretched, the centre filling."""
    import gzip
    import xml.etree.ElementTree as ET
    from PIL import Image
    import numpy as np
    src = "/usr/share/plasma/desktoptheme/default/widgets/tasks.svgz"
    txt = gzip.open(src).read().decode()
    cols = {"ColorScheme-Text": "#fcfcfc" if th == "dark" else "#232629", "ColorScheme-ButtonFocus": "#3daee9",
            "ColorScheme-ButtonHover": "#93cee9", "ColorScheme-PositiveText": "#27ae60", "ColorScheme-NeutralText": "#f67400"}
    ET.register_namespace("", "http://www.w3.org/2000/svg")
    root = ET.fromstring(txt)
    vb = [float(v) for v in root.get("viewBox").split()]
    side = int(max(vb[2], vb[3]))
    parts = {}
    for part in ("topleft", "top", "topright", "left", "center", "right", "bottomleft", "bottom", "bottomright"):
        rt = ET.fromstring(txt)
        eid = f"{prefix}-{part}"
        if not any(e.get("id") == eid for e in rt.iter()):
            raise LookupError(f"plasma frame: no {eid} in {src}")
        def own(el, colour):
            cl = next((cols[c] for c in (el.get("class") or "").split() if c in cols), colour)
            for at in ("fill", "stroke"):
                if el.get(at) == "currentColor":
                    el.set(at, cl or "#000000")
            if "currentColor" in (el.get("style") or ""):
                el.set("style", el.get("style").replace("currentColor", cl or "#000000"))
            return cl

        def recolor(el, colour):
            cl = own(el, colour)
            for ch in el:
                recolor(ch, cl)

        def walk(el, colour):
            for ch in list(el):
                if ch.tag.split("}")[-1] in ("style", "defs"):
                    continue
                if ch.get("id") == eid:
                    recolor(ch, colour)
                elif any(e.get("id") == eid for e in ch.iter()):
                    walk(ch, own(ch, colour))
                else:
                    el.remove(ch)
        walk(rt, None)
        rt.set("viewBox", f"{vb[0]} {vb[1]} {side} {side}")
        rt.set("width", str(side))
        rt.set("height", str(side))
        out = os.path.join(raw, f"frame-{th}-{eid}.svg")
        ET.ElementTree(rt).write(out)
        im = _render_svg(r, out, side, out[:-4] + ".png")
        a = np.asarray(im)[..., 3]
        ys, xs = np.nonzero(a > 0)
        parts[part] = im.crop((xs.min(), ys.min(), xs.max() + 1, ys.max() + 1)) if len(xs) else Image.new("RGBA", (1, 1))
    F = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    lw, rw_ = parts["topleft"].width, parts["topright"].width
    th_, bh = parts["topleft"].height, parts["bottomleft"].height
    iw, ih = max(1, w - lw - rw_), max(1, h - th_ - bh)
    put = lambda im, x, y: F.alpha_composite(im, (x, y))
    put(parts["center"].resize((iw, ih)), lw, th_)
    put(parts["top"].resize((iw, th_)), lw, 0)
    put(parts["bottom"].resize((iw, bh)), lw, h - bh)
    put(parts["left"].resize((lw, ih)), 0, th_)
    put(parts["right"].resize((rw_, ih)), w - rw_, th_)
    put(parts["topleft"], 0, 0)
    put(parts["topright"], w - rw_, 0)
    put(parts["bottomleft"], 0, h - bh)
    put(parts["bottomright"], w - rw_, h - bh)
    return F


def m_appicon7(r, arg):
    """appicon7: appicon/panel per theme, all 1:1: a 44 px panel's task manager at 32 px (Breeze apps and
    Ferrous; Ferrous the active task in Plasma's own Breeze focus frame, then Elisa active with Ferrous playing, the
    audio badge on it); the audio badge on Ferrous at 22, 32, 40 and 48 px slots; a 48 px launcher grid beside
    Breeze app icons with labels. The badge: Breeze's audio-volume-high at 0.4 of the icon (at most 22 px), on its
    top-right corner (Plasma 6's task manager rule as the harness knows it: the applet's QML is compiled in)."""
    r.s.draws_without_app_pixels()  # (SVGs and icon files; app pixels only from shots it names)
    from PIL import Image, ImageDraw, ImageFont
    src = os.environ.get("SHOT_SRC", "")
    a_root = os.path.join(src, "ui/assets/icons/hicolor")
    raw = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "appicon-raw")
    os.makedirs(raw, exist_ok=True)
    th = r.s.tag
    B = _BREEZE[th]
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 13)
    fr = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 12)
    fs = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 11)

    def ico(nm, px):
        if nm == "ferrous":
            svg, lab, dpx = _qt_pick(a_root, px, 1.0)
            return _render_svg(r, svg, dpx, os.path.join(raw, f"panel-ferrous-{px}.png"))
        p_ = _app_svg(nm, px)
        if not p_:
            raise FileNotFoundError(f"appicon7: no Breeze icon {nm}")
        return _render_svg(r, p_, px, os.path.join(raw, f"panel-{nm}-{px}.png"))
    badge_svg = f"/usr/share/icons/{'breeze-dark' if th == 'dark' else 'breeze'}/status/22/audio-volume-high.svg"
    if not os.path.exists(badge_svg):
        badge_svg = f"/usr/share/icons/{'breeze-dark' if th == 'dark' else 'breeze'}/status/16/audio-volume-high.svg"

    def badge(px):
        b = round(min(px * 0.4, 22))
        return _render_svg(r, badge_svg, b, os.path.join(raw, f"badge-{th}-{b}.png"))
    row = ["system-file-manager", "utilities-terminal", "kate", "ferrous", "elisa", "okular", "systemsettings"]
    cell, ph, px = 48, 44, 32
    W = 10 + len(row) * cell + 300
    P = Image.new("RGB", (W, 30 + 2 * (ph + 22) + 22 + 4 * 66 + 30 + 2 * 96 + 20), B["window"])
    d = ImageDraw.Draw(P)
    d.text((10, 6), f"Breeze {th.capitalize()}, 1:1: a 44 px panel's task manager at 32 px; the audio badge; a 48 px launcher",
           fill=B["text"], font=fb)
    y = 30
    for active, playing, note in (("ferrous", False, "Ferrous active (Plasma's Breeze focus frame)"),
                                  ("elisa", True, "Elisa active, Ferrous playing (audio badge)")):
        d.rectangle((0, y, W, y + ph - 1), fill=B["panel"])
        x = 10
        for nm in row:
            if nm == active:
                fr_ = _plasma_frame(r, raw, "focus", cell, ph, th)
                bgc = Image.new("RGBA", (cell, ph), B["panel"] + (255,))
                bgc.alpha_composite(fr_)
                P.paste(bgc.convert("RGB"), (x, y))
            im_ = ico(nm, px)
            P.paste(im_, (x + (cell - px) // 2, y + (ph - px) // 2), im_)
            if playing and nm == "ferrous":
                b = badge(px)
                P.paste(b, (x + (cell - px) // 2 + px - b.width + 2, y + (ph - px) // 2 - 2), b)
            x += cell
        d.text((x + 12, y + ph // 2 - 8), note, fill=B["text"], font=fr)
        y += ph + 22
    d.text((10, y - 4), "the audio badge on Ferrous at 22, 32, 40 and 48 px (0.4 of the icon, at most 22 px)",
           fill=B["text"], font=fr)
    y += 14
    x = 10
    for bpx in (22, 32, 40, 48):
        sh = bpx + 12
        d.rectangle((x, y, x + bpx + 24, y + sh), fill=B["panel"])
        im_ = ico("ferrous", bpx)
        P.paste(im_, (x + 12, y + 6), im_)
        b = badge(bpx)
        P.paste(b, (x + 12 + bpx - b.width + 2, y + 4), b)
        d.text((x, y + sh + 2), f"{bpx} px, badge {b.width}", fill=B["text"], font=fs)
        x += max(bpx + 24, 84) + 14
    y += 48 + 12 + 30
    d.text((10, y - 4), "a launcher grid at 48 px", fill=B["text"], font=fr)
    y += 14
    labs = {"system-file-manager": "Dolphin", "utilities-terminal": "Konsole", "kate": "Kate", "ferrous": "Ferrous",
            "elisa": "Elisa", "okular": "Okular", "systemsettings": "System Settings", "gwenview": "Gwenview"}
    grid = ["system-file-manager", "elisa", "ferrous", "kate", "okular", "gwenview", "utilities-terminal", "systemsettings"]
    for i, nm in enumerate(grid):
        cx, cy = 10 + (i % 4) * 104, y + (i // 4) * 96
        im_ = ico(nm, 48)
        P.paste(im_, (cx + 28, cy + 6), im_)
        tw = d.textlength(labs[nm], font=fr)
        d.text((cx + 52 - tw / 2, cy + 60), labs[nm], fill=B["text"], font=fr)
    P = P.crop((0, 0, W, y + 2 * 96 + 6))
    _save_shot(r, P, "appicon", "panel", f"1:1 on Breeze {th}: the task manager at 32 px on a 44 px panel (Ferrous active in "
                                         "Plasma's focus frame; Elisa active, Ferrous with the audio badge), the badge at "
                                         "22-48 px, a 48 px launcher grid")


def _top_air(r, svg, dpx, raw, tag, n=4):
    """the air above the top trace where it leaves, from its own coverage at the slot's device px (the pixels as
    drawn): the exit column (its last column at least half covered), the tile's top in that column (its first row
    the tile covers at least half: the rounded corner counts), then the rows between them: clear (the trace under
    10 %), faint (10-50 %); 0/0 when it leaves through the top."""
    import math
    import numpy as np
    ids = [t for t in _TRACE_IDS if t in _svg_ids(svg)]
    top = ids[-1]
    c = _cov(r, svg, [top], dpx, raw, f"{tag}-air-{top}")
    k = _viewbox_scale(svg, dpx)
    yt = float(_svg_attr(svg, "tile", "y")) * k
    cols = np.nonzero(c.max(axis=0) >= 0.5)[0]
    x = int(cols[-1])
    tcov = _cov(r, svg, ["tile"], dpx, raw, f"{tag}-air-tile")
    clear, faint, tops = [], [], []
    for xx in range(max(0, x - n + 1), x + 1):  # (the last 4 columns: the exit column can sit in the tile's rounded corner)
        col = c[:, xx]
        # the tile's own top in that column (its rounded corner lowers it near the right edge)
        tc = tcov[:, xx]
        y0 = int(np.nonzero(tc >= 0.5)[0][0]) if (tc >= 0.5).any() else int(math.ceil(yt - 1e-6))
        sol = np.nonzero(col >= 0.5)[0]
        solid = int(sol[0]) if len(sol) else y0
        tops.append(y0)
        clear.append(int(sum(1 for y in range(y0, solid) if col[y] < 0.1)))
        faint.append(int(sum(1 for y in range(y0, solid) if 0.1 <= col[y] < 0.5)))
    return dict(trace=top, col=x, cols=f"{max(0, x - n + 1)}-{x}", clear=clear, faint=faint, tops=tops)


def m_appicon8(r, arg):
    """appicon8: appicon/reading per theme, 1:1 unless marked: (1) Ferrous between LabPlot and KmPlot at 32 and
    48 px, on the window and on the panel (does it read as a chart?); (2) the 64 px drawing drawn at 32 px (Qt's SVG
    renderer, as a lookup would scale it) beside the hand-drawn 32 px file, 1:1 and 4x; (3) a squint row per size
    (the file the lookup takes at 1x; greyscale, blurred by px/24), each trace's grey level. Facts (dark):
    appicon-drift-32 (the two 32 px drawings' centre lines compared), appicon-squint-sizes."""
    r.s.draws_without_app_pixels()  # (SVGs and icon files; app pixels only from shots it names)
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
    src = os.environ.get("SHOT_SRC", "")
    a_root = os.path.join(src, "ui/assets/icons/hicolor")
    raw = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "appicon-raw")
    os.makedirs(raw, exist_ok=True)
    th = r.s.tag
    B = _BREEZE[th]
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 13)
    fr = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 12)
    fs = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 11)

    def ferrous(px):
        svg, lab, dpx = _qt_pick(a_root, px, 1.0)
        return _render_svg(r, svg, dpx, os.path.join(raw, f"read-ferrous-{px}.png"))

    def app(nm, px):
        p_ = _app_svg(nm, px)
        if not p_:
            raise FileNotFoundError(f"appicon8: no Breeze icon {nm}")
        return _render_svg(r, p_, px, os.path.join(raw, f"read-{nm}-{px}.png"))
    W = 770
    R = Image.new("RGB", (W, 700), B["window"])
    d = ImageDraw.Draw(R)
    y = 6
    d.text((10, y), f"Breeze {th.capitalize()}: between LabPlot and KmPlot at 32 and 48 px, on the window and on the panel",
           fill=B["text"], font=fb)
    y += 22
    x = 10
    for ground in ("window", "panel"):
        for px in (32, 48):
            cw = 3 * (px + 12) + 12
            d.rectangle((x, y, x + cw, y + px + 16), fill=B[ground])
            for j, im_ in enumerate((app("labplot", px), ferrous(px), app("kmplot", px))):
                R.paste(im_, (x + 12 + j * (px + 12), y + 8), im_)
            d.text((x, y + px + 18), f"{px} px, {ground}", fill=B["text"], font=fs)
            x += cw + 16
    y += 48 + 16 + 34
    # 2. the 64 px drawing at 32 px beside the 32 px file
    y_drift = y
    d.text((10, y), "The 64 px drawing at 32 px vs the 32 px file, 1:1 and 4x", fill=B["text"], font=fb)
    y += 22
    f32 = os.path.join(a_root, "32x32", "apps", "ferrous.svg")
    f64 = os.path.join(a_root, "64x64", "apps", "ferrous.svg")
    x = 10
    for lab, svg in (("the 32 px file", f32), ("the 64 px drawing at 32 px", f64)):
        im_ = _render_svg(r, svg, 32, os.path.join(raw, f"drift-{os.path.basename(os.path.dirname(os.path.dirname(svg)))}-32.png"))
        bg = Image.new("RGB", (32, 32), B["window"])
        bg.paste(im_, (0, 0), im_)
        R.paste(bg, (x, y + 128 - 32))
        R.paste(bg.resize((128, 128), Image.NEAREST), (x + 44, y))
        d.text((x, y + 132), lab, fill=B["text"], font=fs)
        x += 44 + 128 + 40
    if th == "dark":
        c32 = _centre_lines(r, f32, 32, raw, "drift-32")
        c64 = _centre_lines(r, f64, 32, raw, "drift-64at32")
        names = {"fundamental": "glide", "h2": "2nd", "h3": "3rd", "h4": "4th"}
        rows = []
        for t in _TRACE_IDS:
            a_, b_ = c32["lines"].get(t), c64["lines"].get(t)
            if not a_ or not b_:
                continue
            rows.append(f"{names[t]}: angle at 50 % {a_['angles'][1]} vs {b_['angles'][1]} deg, width ⟂ {a_['widths'][1]} vs "
                        f"{b_['widths'][1]} px, rise {a_['rise']} vs {b_['rise']} px, x {a_['x0']}-{a_['x1']} vs {b_['x0']}-{b_['x1']}")
        fan = lambda c: f"{c['fan']['ratio']}x (start {c['fan']['start']}, exit {c['fan']['exit']} px)" if c["fan"] else "-"
        _fact(r, "appicon-drift-32", "Composition drift at 32 px, the hand-drawn file vs the 64 px drawing drawn at 32 px "
              "(centre lines from each SVG): " + "; ".join(rows) + f"; fan {fan(c32)} vs {fan(c64)}")
    y += 150 + 16
    # 3. the squint row per size
    y_squint = y
    d.text((10, y), "Squint: each size as the lookup takes it at 1x, grey, blurred by px/24; grey per trace, top first",
           fill=B["text"], font=fb)
    y += 22
    x = 10
    lums_f = []
    for px in (16, 22, 24, 32, 36, 48, 64, 128):
        im_ = ferrous(px)
        g = Image.new("RGB", (px + 8, px + 8), B["window"])
        g.paste(im_, (4, 4), im_)
        g = g.convert("L").filter(ImageFilter.GaussianBlur(px / 24)).convert("RGB")
        R.paste(g, (x, y + 136 - px))
        lums = _trace_lums(im_)
        d.text((x, y + 140), f"{px}", fill=B["text"], font=fs)
        d.text((x, y + 154), "/".join(str(v) for v in lums) or "-", fill=B["text"], font=fs)
        lums_f.append(f"{px} px: " + " / ".join(str(v) for v in lums))
        x += max(px + 8, 70) + 10
    if th == "dark":
        _fact(r, "appicon-squint-sizes", "Squint row (installed, the file the lookup takes at 1x): traces' grey levels at "
                                         "50 % across, top to bottom: " + "; ".join(lums_f))
    R = R.crop((0, 0, W, y + 172))
    # the drift and the squint row as their own shots too
    _save_shot(r, R.crop((0, y_drift, 470, y_squint - 8)), "appicon", "drift",
               f"The 64 px drawing at 32 px beside the 32 px file, 1:1 and 4x, Breeze {th}")
    _save_shot(r, R.crop((0, y_squint, W, R.height)), "appicon", "squint", f"The squint row per size, Breeze {th}")
    _save_shot(r, R, "appicon", "reading", f"Breeze {th}: between LabPlot and KmPlot at 32/48 px; the 64 px drawing at "
                                           "32 px beside the 32 px file; the squint row per size")


def m_appicon9(r, arg):
    """appicon9: the real-context sheets, all 1:1, per theme. appicon/ctx-small: the title bar (this run's
    About window), a menu at 16 px (Ferrous plain, then hovered), a task-switcher list at 22 px (another window
    selected, then Ferrous), a 44 px panel's task manager at 32 px (plain; playing, with the audio badge; active and
    playing, in Plasma's focus frame), 36 px on a 52 px panel, and apart from them a 2x crop of the playing cell with
    the badge's box outlined. appicon/ctx-large: a 48 px launcher grid (with LabPlot and KmPlot), a 64 px Alt-Tab
    list (Ferrous selected), an About page at 128 px. appicon/ctx-hidpi: the 32 px slot at 1.5x and 2x (device px,
    the files Plasma's lookup takes) among Breeze icons on 66 and 88 px panels. appicon/ctx-discover: a Discover-like
    tile with the icon at 256 px. The drawn menus, lists and frames follow Breeze's colours; only the focus frame is
    Plasma's own artwork."""
    from PIL import Image, ImageDraw, ImageFont
    src = os.environ.get("SHOT_SRC", "")
    a_root = os.path.join(src, "ui/assets/icons/hicolor")
    raw = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "appicon-raw")
    os.makedirs(raw, exist_ok=True)
    th = r.s.tag
    B = _BREEZE[th]
    HL = (61, 174, 233)
    view = (27, 30, 32) if th == "dark" else (255, 255, 255)
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 13)
    fr = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 12)
    f10 = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 10)
    mix = lambda a, b, t: tuple(int(round(a[i] * (1 - t) + b[i] * t)) for i in range(3))

    def ico(nm, px, dpr=1.0):
        if nm == "ferrous":
            svg, lab, dpx = _qt_pick(a_root, px, dpr)
            return _render_svg(r, svg, dpx, os.path.join(raw, f"ctx-ferrous-{px}-{dpr:g}.png"))
        p_ = _app_svg(nm, px)
        if not p_:
            raise FileNotFoundError(f"appicon9: no Breeze icon {nm}")
        dpx = int(round(px * dpr))
        return _render_svg(r, p_, dpx, os.path.join(raw, f"ctx-{nm}-{dpx}.png"))
    badge_svg = f"/usr/share/icons/{'breeze-dark' if th == 'dark' else 'breeze'}/status/22/audio-volume-high.svg"

    def badge(px):
        b = round(min(px * 0.4, 22))
        return _render_svg(r, badge_svg, b, os.path.join(raw, f"ctx-badge-{th}-{b}.png"))
    labs = {"system-file-manager": "Dolphin", "utilities-terminal": "Konsole", "kate": "Kate", "ferrous": "Ferrous",
            "elisa": "Elisa", "okular": "Okular", "systemsettings": "System Settings", "gwenview": "Gwenview",
            "labplot": "LabPlot", "kmplot": "KmPlot", "juk": "JuK", "amarok": "Amarok", "dragonplayer": "Dragon Player",
            "kaffeine": "Kaffeine", "mpv": "mpv"}
    # ---- small sizes
    S = Image.new("RGB", (1000, 760), B["window"])
    d = ImageDraw.Draw(S)
    d.text((10, 6), f"Breeze {th.capitalize()}, 1:1: the icon at 16-36 px where Plasma shows it", fill=B["text"], font=fb)
    MX = (10, 220, 440, 700)  # the two menus, the two task-switcher lists
    y = 30
    ap = os.path.join(os.environ["SHOT_OUT"], f"dialog_about-window_{r.s.suffix}.png")
    if not os.path.exists(ap):
        raise FileNotFoundError(f"appicon9: no About window shot {ap}")
    r.s.source_shot(os.path.basename(ap))
    tb = Image.open(ap).convert("RGB").crop((0, 0, 330, 34))
    d.text((10, y), "title bar (KWin's Breeze decoration, this run's About window), 16 px", fill=B["text"], font=fr)
    S.paste(tb, (10, y + 16))
    y_top = y
    # 24 px on a 32 px toolbar-like strip, beside the title bar
    tr = ["system-file-manager", "utilities-terminal", "kate", "ferrous", "elisa", "okular", "systemsettings"]
    d.text((380, y), "24 px on a 32 px strip", fill=B["text"], font=fr)
    d.rectangle((370, y + 16, 380 + len(tr) * 32 + 10, y + 16 + 31), fill=B["panel"])
    for i, nm in enumerate(tr):
        im_ = ico(nm, 24)
        S.paste(im_, (380 + i * 32 + 4, y + 16 + 4), im_)
    y += 16 + 34 + 16
    # a menu at 16 px, twice: Ferrous plain (another item hovered), Ferrous hovered
    menu = ["system-file-manager", "elisa", "ferrous", "kate", "okular", "labplot"]
    d.text((10, y), "a menu at 16 px (Open With): another item hovered, then Ferrous", fill=B["text"], font=fr)
    d.text((MX[2], y), "a task-switcher list at 22 px: another window selected, then Ferrous", fill=B["text"], font=fr)
    y0 = y + 18
    for k, hov in enumerate(("elisa", "ferrous")):
        x0 = MX[k]
        d.rounded_rectangle((x0, y0, x0 + 196, y0 + 8 + len(menu) * 24), radius=4, fill=view, outline=mix(view, B["text"], 0.2))
        for i, nm in enumerate(menu):
            yy = y0 + 4 + i * 24
            if nm == hov:
                d.rounded_rectangle((x0 + 3, yy, x0 + 193, yy + 23), radius=3, fill=mix(view, HL, 0.3), outline=HL)
            im_ = ico(nm, 16)
            S.paste(im_, (x0 + 10, yy + 4), im_)
            d.text((x0 + 34, yy + 4), labs[nm], fill=B["text"], font=fr)
    wins = [("system-file-manager", "Home — Dolphin"), ("ferrous", "Ferrous"), ("kate", "notes.txt — Kate"),
            ("okular", "Manual.pdf — Okular"), ("elisa", "Elisa")]
    for k, sel in enumerate(("kate", "ferrous")):
        x0 = MX[2 + k]
        d.rounded_rectangle((x0, y0, x0 + 246, y0 + 10 + len(wins) * 30), radius=6, fill=B["window"], outline=mix(B["window"], B["text"], 0.25))
        for i, (nm, cap) in enumerate(wins):
            yy = y0 + 5 + i * 30
            if nm == sel:
                d.rounded_rectangle((x0 + 4, yy, x0 + 242, yy + 29), radius=3, fill=mix(B["window"], HL, 0.3), outline=HL)
            im_ = ico(nm, 22)
            S.paste(im_, (x0 + 10, yy + 4), im_)
            d.text((x0 + 40, yy + 7), cap, fill=B["text"], font=fr)
    y = y0 + 10 + len(wins) * 30 + 12
    # the 44 px panel at 32 px: plain, playing, active and playing; 36 px on a 52 px panel
    row = ["system-file-manager", "utilities-terminal", "kate", "ferrous", "elisa", "okular", "systemsettings"]
    d.text((10, y), "32 px on a 44 px panel: plain; playing; active + playing", fill=B["text"], font=fr)
    y_panel = y
    y += 18
    cell, ph = 48, 44
    play_cell = None
    for active, playing in ((False, False), (False, True), (True, True)):
        d.rectangle((0, y, 10 + len(row) * cell + 10, y + ph - 1), fill=B["panel"])
        x = 10
        for nm in row:
            if nm == "ferrous" and active:
                fr_ = _plasma_frame(r, raw, "focus", cell, ph, th)
                bgc = Image.new("RGBA", (cell, ph), B["panel"] + (255,))
                bgc.alpha_composite(fr_)
                S.paste(bgc.convert("RGB"), (x, y))
            im_ = ico(nm, 32)
            S.paste(im_, (x + 8, y + 6), im_)
            if nm == "ferrous" and playing:
                b = badge(32)
                bx, by = x + 8 + 32 - b.width + 2, y + 4
                S.paste(b, (bx, by), b)
                if not active:
                    play_cell = (x, y, (bx - x, by - y, b.width, b.height))
            x += cell
        y += ph + 6
    # the 2x crop of the playing cell with the badge's box outlined (apart from the 1:1 rows)
    if play_cell:
        x_, y_, (bx, by, bw, bh) = play_cell
        z = S.crop((x_, y_, x_ + cell, y_ + ph)).resize((cell * 2, ph * 2), Image.NEAREST)
        ImageDraw.Draw(z).rectangle((bx * 2 - 1, by * 2 - 1, (bx + bw) * 2, (by + bh) * 2), outline=(218, 68, 83))
        zx, zy = 410, y_panel + 18 + 52 + 10
        S.paste(z, (zx, zy))
        d.text((zx + cell * 2 + 10, zy + 4), f"2x crop (not 1:1): the playing cell\nof the 32 px panel at left, the badge's\n"
                                             f"box outlined ({bw}x{bh} px)", fill=B["text"], font=fr)
    # 36 px on a 52 px panel, right of the 32 px rows
    yb = y_panel
    d.text((410, yb), "36 px on a 52 px panel", fill=B["text"], font=fr)
    d.rectangle((400, yb + 18, 400 + len(row) * 56 + 10, yb + 18 + 51), fill=B["panel"])
    x = 410
    for nm in row:
        im_ = ico(nm, 36)
        S.paste(im_, (x + 10, yb + 18 + 8), im_)
        x += 56
    y = max(y, y_panel + 18 + 52 + 10 + ph * 2) + 6
    S = S.crop((0, 0, 1000, y))
    _save_shot(r, S, "appicon", "ctx-small", f"1:1 on Breeze {th}: the title bar, a 16 px menu, a 22 px task-switcher list, "
                                             "the 44 px panel at 32 px (plain, playing, active), 36 px on a 52 px panel; "
                                             "a 2x crop of the playing cell")
    # ---- large sizes: a 48 px launcher grid, a 64 px Alt-Tab list, an About page at 128 px
    L = Image.new("RGB", (1000, 520), B["window"])
    d = ImageDraw.Draw(L)
    d.text((10, 6), f"Breeze {th.capitalize()}, 1:1: a launcher at 48 px, Alt-Tab at 64 px, an About page at 128 px",
           fill=B["text"], font=fb)
    grid = ["system-file-manager", "labplot", "ferrous", "kmplot", "elisa", "kate", "okular", "systemsettings"]
    y = 30
    d.rounded_rectangle((10, y, 10 + 4 * 104 + 10, y + 2 * 96 + 14), radius=6, fill=view)
    for i, nm in enumerate(grid):
        cx, cy = 15 + (i % 4) * 104, y + 6 + (i // 4) * 96
        im_ = ico(nm, 48)
        L.paste(im_, (cx + 28, cy + 6), im_)
        tw = d.textlength(labs[nm], font=fr)
        d.text((cx + 52 - tw / 2, cy + 60), labs[nm], fill=B["text"], font=fr)
    alt = ["system-file-manager", "ferrous", "kate", "elisa", "okular"]
    ax = 10 + 4 * 104 + 40
    d.rounded_rectangle((ax, y, ax + 5 * 100 + 16, y + 132), radius=8, fill=B["window"], outline=mix(B["window"], B["text"], 0.3))
    for i, nm in enumerate(alt):
        cx = ax + 8 + i * 100
        if nm == "ferrous":
            d.rounded_rectangle((cx, y + 8, cx + 98, y + 124), radius=5, fill=mix(B["window"], HL, 0.25), outline=HL, width=2)
        im_ = ico(nm, 64)
        L.paste(im_, (cx + 17, y + 18), im_)
        tw = d.textlength(labs[nm], font=fr)
        d.text((cx + 49 - tw / 2, y + 92), labs[nm], fill=B["text"], font=fr)
    y += 2 * 96 + 14 + 20
    ic = ico("ferrous", 128)
    ver = re.search(r'^version\s*=\s*"([^"]+)"', open(os.path.join(src, "Cargo.toml")).read(), re.M)
    desc = re.search(r'"@info", "(A music player[^"]*)"', open(os.path.join(src, "ui/qml/dialogs/AboutDialog.qml")).read())
    d.rounded_rectangle((10, y, 10 + 640, y + 168), radius=6, fill=B["window"], outline=mix(B["window"], B["text"], 0.2))
    L.paste(ic, (30, y + 20), ic)
    d.text((30 + 128 + 20, y + 34), "Ferrous", fill=B["text"], font=ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 22))
    d.text((30 + 128 + 20, y + 70), f"Version {ver.group(1) if ver else '?'}", fill=B["muted"], font=fr)
    d.text((30 + 128 + 20, y + 98), desc.group(1) if desc else "", fill=B["text"], font=fr)
    # a player among players: the music and media players' icons this system has, at 32 and 48 px
    players = [p_ for p_ in ("elisa", "juk", "amarok", "ferrous", "dragonplayer", "kaffeine", "kmplayer", "mpv")
               if p_ == "ferrous" or _app_svg(p_, 48)]
    px0 = 670
    d.text((px0, y), "media players at 32 and 48 px (this system's)", fill=B["text"], font=f10)
    d.rectangle((px0, y + 16, px0 + len(players) * 38 + 8, y + 16 + 40), fill=B["panel"])
    for i, nm in enumerate(players):
        im_ = ico(nm, 32)
        L.paste(im_, (px0 + 6 + i * 38, y + 16 + 4), im_)
    for i, nm in enumerate(players):
        im_ = ico(nm, 48)
        L.paste(im_, (px0 + (i % 4) * 60, y + 64 + (i // 4) * 54), im_)
    if r.s.tag == "dark":
        _fact(r, "appicon-players", "Players beside Ferrous (the icons this system has): " + ", ".join(players))
    y += 168 + 10
    L = L.crop((0, 0, max(ax + 5 * 100 + 16 + 10, px0 + len(players) * 38 + 20), y))
    _save_shot(r, L, "appicon", "ctx-large", f"1:1 on Breeze {th}: a 48 px launcher with LabPlot and KmPlot, a 64 px "
                                             "Alt-Tab list with Ferrous selected, an About page at 128 px")
    # ---- the 32 px slot at 1.5x and 2x, device px
    row_h = ["system-file-manager", "kate", "ferrous", "elisa", "okular"]  # (five, so dark and light sit side by side)
    Hh = Image.new("RGB", (10 + 5 * 96 + 140, 400), B["window"])
    d = ImageDraw.Draw(Hh)
    d.text((10, 6), f"Breeze {th.capitalize()}: the 32 px slot at 1.25x-2x, device px (55-88 px panels)",
           fill=B["text"], font=fb)
    y = 28
    for dpr in (1.25, 1.5, 1.75, 2.0):  # (1.25x and 1.75x too)
        ph_, cw_ = int(44 * dpr), int(48 * dpr)
        d.rectangle((0, y, 10 + len(row_h) * cw_ + 10, y + ph_ - 1), fill=B["panel"])
        x = 10
        for nm in row_h:
            im_ = ico(nm, 32, dpr)
            Hh.paste(im_, (x + (cw_ - im_.width) // 2, y + (ph_ - im_.height) // 2), im_)
            x += cw_
        d.text((x + 8, y + ph_ // 2 - 16), f"{dpr:g}x\n{_qt_pick(a_root, 32, dpr)[1]}", fill=B["text"], font=fr)
        y += ph_ + 10
    Hh = Hh.crop((0, 0, 10 + len(row_h) * int(48 * 2.0) + 140, y))
    _save_shot(r, Hh, "appicon", "ctx-hidpi", f"The 32 px slot at 1.25x to 2x among Breeze icons, Breeze {th}")
    # ---- a Discover-like tile at 256 px
    # (a narrow card, the name and category under the icon)
    D_ = Image.new("RGB", (304, 352), B["window"])
    d = ImageDraw.Draw(D_)
    d.rounded_rectangle((4, 4, 298, 346), radius=8, fill=view, outline=mix(view, B["text"], 0.18))
    ic = ico("ferrous", 256)
    D_.paste(ic, (22, 18), ic)
    d.text((22, 282), "Ferrous", fill=B["text"], font=ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 22))
    d.text((22, 314), "Multimedia · Audio", fill=B["muted"], font=fr)
    d.rounded_rectangle((200, 300, 284, 328), radius=4, fill=mix(view, HL, 0.15), outline=HL)
    d.text((222, 306), "Install", fill=B["text"], font=fr)
    _save_shot(r, D_, "appicon", "ctx-discover", f"A Discover-like tile with the icon at 256 px, Breeze {th}")
    # 128 and 256 px at 2x (device px), in context: an About header at 2x, a quarter of the Discover tile at 2x
    if th == "dark":
        X = Image.new("RGB", (1000, 420), B["window"])
        d = ImageDraw.Draw(X)
        d.text((10, 6), f"At 2x: About ({_qt_pick(a_root, 128, 2.0)[1]}), store ({_qt_pick(a_root, 256, 2.0)[1]})",
               fill=B["text"], font=fb)
        a2 = ico("ferrous", 128, 2.0)
        # (compact: the About icon at 2x with the name under it, beside a quarter of the 2x Discover icon)
        d.rounded_rectangle((10, 30, 10 + a2.width + 40, 30 + a2.height + 56), radius=12, fill=B["window"],
                            outline=mix(B["window"], B["text"], 0.2))
        X.paste(a2, (30, 44), a2)
        d.text((30, 44 + a2.height + 2), "Ferrous", fill=B["text"],
               font=ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 28))
        t2 = ico("ferrous", 256, 2.0)
        qq = t2.crop((t2.width // 2, 0, t2.width, t2.height // 2))
        q = Image.new("RGB", (qq.width + 24, qq.height + 24), view)
        q.paste(qq, (0, 24), qq)
        qx = 10 + a2.width + 60
        X.paste(q, (qx, 30))
        d.text((qx, 30 + q.height + 4), "a quarter of the 512 px\nstore icon", fill=B["text"], font=fr)
        X = X.crop((0, 0, qx + q.width + 10, max(30 + a2.height + 60, 30 + q.height + 4 + 34)))
        _save_shot(r, X, "appicon", "ctx-2x", "128 and 256 px at 2x in context (About header; a quarter of the Discover tile)")


def m_appicon10(r, arg):
    """appicon10 (dark, once): appicon/h4true: 128 and 256 px, the upper right of the tile magnified (4x, 2x)
    with the true 3rd and 4th harmonics drawn over it: from the glide's and the 2nd's centre lines per column (zero
    line y0 = 2 y1 - y2; the n-th at y0 - n (y0 - y1)), thin lines; facts appicon-true-NNN (the drawn 3rd and 4th
    against the true ones at 25/50/75 % across), appicon-air-032/036 (top air per column over the top trace's last
    10 columns, the tile's top in each column: its rounded corner), appicon-weight-1x2x (each slot's traces' widths
    at 1x and at 2x, halved)."""
    r.s.draws_without_app_pixels()  # (SVGs and icon files; app pixels only from shots it names)
    from PIL import Image, ImageDraw, ImageFont
    import numpy as np
    src = os.environ.get("SHOT_SRC", "")
    a_root = os.path.join(src, "ui/assets/icons/hicolor")
    raw = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "appicon-raw")
    # the 4th harmonic's end at 4x, clean (no overlay), per theme: where it crosses the tile's top edge
    Bt = _BREEZE[r.s.tag]
    fs_ = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 11)
    crops = []
    for px in (128, 256):
        svg, lab, dpx = _qt_pick(a_root, px, 1.0)
        e = _h4_end(r, svg, dpx, raw, f"end27-{px}")
        if not e:
            continue
        x0_, y0_, x1_, y1_ = e[1]
        x0_, x1_ = max(0, x0_ - 6), min(dpx, x1_ + 6)
        y0_, y1_ = max(0, y0_ - 2), min(dpx, max(y1_, y0_ + 24) + 4)
        im_ = _render_svg(r, svg, dpx, os.path.join(raw, f"end27-{px}-{dpx}.png"))
        bg = Image.new("RGB", im_.size, Bt["window"])
        bg.paste(im_, (0, 0), im_)
        c_ = bg.crop((x0_, y0_, x1_, y1_))
        crops.append((f"{px} px ({lab}), 4x", c_.resize((c_.width * 4, c_.height * 4), Image.NEAREST)))
    if crops:
        E = Image.new("RGB", (sum(c_.width + 20 for _, c_ in crops) + 10, max(c_.height for _, c_ in crops) + 26), Bt["window"])
        dE = ImageDraw.Draw(E)
        x = 10
        for lab_, c_ in crops:
            E.paste(c_, (x, 4))
            dE.text((x, 4 + c_.height + 4), lab_, fill=Bt["text"], font=fs_)
            x += c_.width + 20
        _save_shot(r, E, "appicon", "h4end", f"The 4th harmonic's end at the tile's top edge, 4x, no overlay, Breeze {r.s.tag}")
    if r.s.tag != "dark":
        return
    B = _BREEZE["dark"]
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 13)
    fs = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 11)
    names = {"fundamental": "glide", "h2": "2nd", "h3": "3rd", "h4": "4th"}
    tiles = []
    for px, z in ((128, 4), (256, 2)):
        svg, lab, dpx = _qt_pick(a_root, px, 1.0)
        cl = _centre_lines(r, svg, dpx, raw, f"true-{px}")
        arr = cl["arrays"]
        if "fundamental" not in arr or "h2" not in arr:
            continue
        k = _viewbox_scale(svg, dpx)
        tx0 = float(_svg_attr(svg, "tile", "x")) * k
        tx1 = tx0 + float(_svg_attr(svg, "tile", "width")) * k
        ty0 = float(_svg_attr(svg, "tile", "y")) * k
        xs = np.linspace(max(arr["fundamental"][0][0], arr["h2"][0][0]), min(arr["fundamental"][0][-1], arr["h2"][0][-1]), 400)
        y1 = np.interp(xs, *arr["fundamental"])
        y2 = np.interp(xs, *arr["h2"])
        y0 = 2 * y1 - y2
        true = {n: y0 - n * (y0 - y1) for n in (3, 4)}
        rows = []
        for n, t in ((3, "h3"), (4, "h4")):
            if t not in arr:
                rows.append(f"no drawn {names[t]}")
                continue
            ax_, ay_ = arr[t]
            offs = []
            for f in (0.25, 0.5, 0.75):
                x_ = tx0 + f * (tx1 - tx0)
                if ax_[0] <= x_ <= ax_[-1]:
                    offs.append(f"{int(f * 100)} %: {round(float(np.interp(x_, ax_, ay_) - np.interp(x_, xs, true[n])), 2):+g} px")
                else:
                    offs.append(f"{int(f * 100)} %: not drawn there")
            rows.append(f"the drawn {names[t]} against the true one (+ = lower): " + ", ".join(offs))
        sp = cl["spacing"]
        _fact(r, f"appicon-true-{px:03d}", f"True harmonics at {px} px (from the glide's and the 2nd's centre lines): "
              + "; ".join(rows) + f"; spacing half way across (x {sp['x']}): "
              + ", ".join(f"{names.get(a, a)}-{names.get(b, b)} {v} px" for a, b, v in sp["pairs"]))
        im_ = _render_svg(r, svg, dpx, os.path.join(raw, f"true-{px}-{dpx}.png"))
        bg = Image.new("RGB", im_.size, B["window"])
        bg.paste(im_, (0, 0), im_)
        box = (int(tx0 + 0.45 * (tx1 - tx0)), max(0, int(ty0) - 4), min(dpx, int(tx1) + 4), int(ty0 + 0.5 * (tx1 - tx0)))
        cz = bg.crop(box).resize(((box[2] - box[0]) * z, (box[3] - box[1]) * z), Image.NEAREST)
        dz = ImageDraw.Draw(cz)
        for n, col in ((3, (120, 230, 255)), (4, (255, 255, 255))):
            pts = [((x_ - box[0]) * z, (y_ - box[1]) * z) for x_, y_ in zip(xs, true[n]) if box[0] <= x_ <= box[2] and y_ >= box[1] - 2]
            for i in range(len(pts) - 1):
                if (i // 5) % 2 == 0:
                    dz.line([pts[i], pts[i + 1]], fill=col, width=1)
        tiles.append((f"{px} px ({lab}), {z}x", cz))
    if tiles:
        W = sum(t.width + 20 for _, t in tiles) + 10
        H = max(t.height for _, t in tiles) + 50
        T = Image.new("RGB", (W, H), B["window"])
        d = ImageDraw.Draw(T)
        d.text((10, 6), "128 and 256 px, magnified: true 3rd (cyan), true 4th (white), dashed", fill=B["text"], font=fb)
        x = 10
        for lab, t in tiles:
            T.paste(t, (x, 26))
            d.text((x, 26 + t.height + 4), lab, fill=B["text"], font=fs)
            x += t.width + 20
        _save_shot(r, T, "appicon", "h4true", "The 4th harmonic at 128 and 256 px magnified, the true 3rd and 4th drawn over it")
    # the top air per column at 32 and 36 px, the corner's curve included
    for px in (32, 36):
        svg, lab, dpx = _qt_pick(a_root, px, 1.0)
        ta = _top_air(r, svg, dpx, raw, f"air10-{px}", n=10)
        _fact(r, f"appicon-air-{px:03d}", f"Top air at {px} px ({lab}), per column over the top trace's last {len(ta['clear'])} "
              f"columns (x {ta['cols']}): the tile's top row in each (its rounded corner: {', '.join(map(str, ta['tops']))}); "
              f"clear rows {', '.join(map(str, ta['clear']))}; faint rows {', '.join(map(str, ta['faint']))}")
    # 1x vs 2x weight per slot
    rows = []
    for px in (16, 22, 24, 32, 36, 48, 64):
        out = []
        for dpr in (1.0, 2.0):
            svg, lab, dpx = _qt_pick(a_root, px, dpr)
            cl = _centre_lines(r, svg, dpx, raw, f"w12-{px}-{dpr:g}")
            ws = [round(v["widths"][1] / dpr, 2) for v in sorted(cl["lines"].values(), key=lambda v: -v["rise"])]
            out.append(f"{dpr:g}x ({lab}) {'/'.join(f'{w:g}' for w in ws)}")
        rows.append(f"{px} px: " + " vs ".join(out))
    _fact(r, "appicon-weight-1x2x", "Trace widths ⟂ at 50 % along, top trace first, in logical px (2x halved), at 1x and 2x: "
          + "; ".join(rows))


def m_appicon11(r, arg):
    """appicon11 (only with SHOT_ICON_VARIANTS=1; loud when a variant's hicolor or mark PNGs are missing): the
    installed icon vs each variant (ICON_VARIANTS under ICON_VARIANTS_DIR) at 1:1 in real contexts, per
    theme. appicon/var-small: the task manager at 32 px on a 44 px panel (each icon plain and playing), a 48 px
    launcher row, a 64 px Alt-Tab row, About headers at 128 px; appicon/var-large: Discover-like cards at 256 px;
    appicon/var-mark-<name>-<360|152> (dark): the variant's no-art mark PNGs as rendered with the variant. The files a
    variant gives come from the same lookup as the installed ones (its own hicolor tree)."""
    r.s.draws_without_app_pixels()  # (SVGs and icon files; app pixels only from shots it names)
    from PIL import Image, ImageDraw, ImageFont
    import glob as glob_
    if os.environ.get("SHOT_ICON_VARIANTS") != "1":
        return
    vs = []
    for v in ICON_VARIANTS:
        root_ = os.path.join(ICON_VARIANTS_DIR, v, "hicolor")
        marks = {sz: sorted(glob_.glob(os.path.join(ICON_VARIANTS_DIR, v, "mark", f"*{sz}*.png"))) for sz in (360, 152)}
        if not os.path.isdir(root_) or not all(marks.values()):
            raise FileNotFoundError(f"appicon11: variant {v} requested (SHOT_ICON_VARIANTS=1) but missing: "
                                    f"{root_ if not os.path.isdir(root_) else ''} "
                                    f"{[sz for sz, m in marks.items() if not m]} mark PNG(s) under {os.path.join(ICON_VARIANTS_DIR, v, 'mark')}")
        vs.append((v, root_, marks))
    src = os.environ.get("SHOT_SRC", "")
    a_root = os.path.join(src, "ui/assets/icons/hicolor")
    raw = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "appicon-raw")
    th = r.s.tag
    B = _BREEZE[th]
    HL = (61, 174, 233)
    view = (27, 30, 32) if th == "dark" else (255, 255, 255)
    mix = lambda a, b, t: tuple(int(round(a[i] * (1 - t) + b[i] * t)) for i in range(3))
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 13)
    fr = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 12)
    fbig = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 20)
    icons = [("Ferrous (installed)", a_root)] + [(v, rt) for v, rt, _ in vs]

    def mine(rt, px):
        svg, lab, dpx = _qt_pick(rt, px, 1.0)
        tag = "a" if rt == a_root else os.path.basename(os.path.dirname(rt))
        return _render_svg(r, svg, dpx, os.path.join(raw, f"var-{tag}-{px}.png"))

    def app(nm, px):
        return _render_svg(r, _app_svg(nm, px), px, os.path.join(raw, f"var-app-{nm}-{px}.png"))
    badge_svg = f"/usr/share/icons/{'breeze-dark' if th == 'dark' else 'breeze'}/status/22/audio-volume-high.svg"
    bd = _render_svg(r, badge_svg, round(32 * 0.4), os.path.join(raw, f"var-badge-{th}.png"))
    S = Image.new("RGB", (620, 900), B["window"])
    d = ImageDraw.Draw(S)
    d.text((10, 6), f"Breeze {th.capitalize()}, 1:1: installed vs {', '.join(v for v, _, _ in vs)}", fill=B["text"], font=fb)
    y = 30
    d.text((10, y), "32 px on a 44 px panel: each plain, then playing", fill=B["text"], font=fr)
    y += 18
    for lab, rt in icons:
        for playing in (False, True):
            d.rectangle((0, y, 10 + 5 * 48 + 10, y + 43), fill=B["panel"])
            for i, nm in enumerate(("system-file-manager", "utilities-terminal", "ferrous", "elisa", "kate")):
                im_ = mine(rt, 32) if nm == "ferrous" else app(nm, 32)
                S.paste(im_, (10 + i * 48 + 8, y + 6), im_)
                if nm == "ferrous" and playing:
                    S.paste(bd, (10 + i * 48 + 8 + 32 - bd.width + 2, y + 4), bd)
            d.text((10 + 5 * 48 + 20, y + 14), f"{lab}{', playing' if playing else ''}", fill=B["text"], font=fr)
            y += 48
    y += 6
    d.text((10, y), "a launcher row at 48 px", fill=B["text"], font=fr)
    y += 18
    row = [("Dolphin", None, "system-file-manager")] + [(lab.split(" (")[0] if rt == a_root else lab, rt, None) for lab, rt in icons] + [("Elisa", None, "elisa")]
    d.rounded_rectangle((10, y, 10 + len(row) * 100 + 8, y + 92), radius=6, fill=view)
    for i, (lab, rt, nm) in enumerate(row):
        im_ = mine(rt, 48) if rt else app(nm, 48)
        S.paste(im_, (14 + i * 100 + 26, y + 6), im_)
        tw = d.textlength(lab, font=fr)
        d.text((14 + i * 100 + 50 - tw / 2, y + 62), lab, fill=B["text"], font=fr)
    y += 92 + 12
    d.text((10, y), "Alt-Tab at 64 px (the installed icon selected)", fill=B["text"], font=fr)
    y += 18
    d.rounded_rectangle((10, y, 10 + len(row) * 100 + 16, y + 132), radius=8, fill=B["window"], outline=mix(B["window"], B["text"], 0.3))
    for i, (lab, rt, nm) in enumerate(row):
        cx = 18 + i * 100
        if rt == a_root:
            d.rounded_rectangle((cx, y + 8, cx + 98, y + 124), radius=5, fill=mix(B["window"], HL, 0.25), outline=HL, width=2)
        im_ = mine(rt, 64) if rt else app(nm, 64)
        S.paste(im_, (cx + 17, y + 18), im_)
        tw = d.textlength(lab, font=fr)
        d.text((cx + 49 - tw / 2, y + 92), lab, fill=B["text"], font=fr)
    y += 132 + 12
    d.text((10, y), "About headers at 128 px", fill=B["text"], font=fr)
    y += 18
    for i, (lab, rt) in enumerate(icons):
        x0 = 10 + i * 300
        d.rounded_rectangle((x0, y, x0 + 290, y + 160), radius=6, fill=B["window"], outline=mix(B["window"], B["text"], 0.2))
        im_ = mine(rt, 128)
        S.paste(im_, (x0 + 14, y + 16), im_)
        d.text((x0 + 152, y + 50), "Ferrous", fill=B["text"], font=fbig)
        d.text((x0 + 152, y + 80), lab.split(" (")[1].rstrip(")") if rt == a_root else lab, fill=B["muted"], font=fr)
    y += 160 + 10
    S = S.crop((0, 0, max(10 + len(row) * 100 + 30, 10 + len(icons) * 300 + 10), y))
    _save_shot(r, S, "appicon", "var-small", f"1:1 on Breeze {th}: installed vs {', '.join(v for v, _, _ in vs)} at 32 (plain, "
                                             "playing), 48, 64 and 128 px")
    # Discover-like cards at 256 px
    D_ = Image.new("RGB", (10 + len(icons) * 300, 330), B["window"])
    d = ImageDraw.Draw(D_)
    for i, (lab, rt) in enumerate(icons):
        x0 = 10 + i * 300
        d.rounded_rectangle((x0, 6, x0 + 288, 322), radius=8, fill=view, outline=mix(view, B["text"], 0.18))
        im_ = mine(rt, 256)
        D_.paste(im_, (x0 + 16, 16), im_)
        d.text((x0 + 16, 280), "Ferrous", fill=B["text"], font=fbig)
        d.text((x0 + 110, 286), lab.split(" (")[1].rstrip(")") if rt == a_root else lab, fill=B["muted"], font=fr)
    _save_shot(r, D_, "appicon", "var-large", f"Discover-like cards at 256 px: installed vs {', '.join(v for v, _, _ in vs)}, Breeze {th}")
    if th == "dark":
        for v, _, marks in vs:
            for sz, files in marks.items():
                im_ = Image.open(files[0]).convert("RGB")
                _save_shot(r, im_, "appicon", f"var-mark-{v}-{sz}", f"The {v} variant's no-art mark at {sz} px "
                                                                     f"({os.path.basename(files[0])}), 1:1")


def _harness_settings():
    """the display settings every session but firstrun-defaults starts with (driver.SETTINGS), for the facts."""
    from driver import SETTINGS
    keep = ("spectrogram_view_mode", "spectrogram_display_mode", "show_level_meter", "show_spectrogram_scale",
            "show_spectrogram_crosshair", "channel_buttons_visibility", "visualization_mode", "volume")
    kv = dict(l.split("=", 1) for l in SETTINGS.splitlines() if "=" in l)
    return ", ".join(f"{k}={kv[k]}" for k in keep if k in kv)


def _bridge_default(src):
    """The view and display modes BridgeSettings::default() sets (src/frontend_bridge/mod.rs)."""
    p_ = os.path.join(src, "src/frontend_bridge/mod.rs")
    t = open(p_).read() if os.path.exists(p_) else ""
    m = re.search(r"impl Default for BridgeSettings.*?spectrogram_view_mode:\s*SpectrogramViewMode::(\w+).*?"
                  r"spectrogram_display_mode:\s*SpectrogramDisplayMode::(\w+)", t, re.S)
    return f"view {m.group(1)}, display {m.group(2)}" if m else "not found"


def m_firstrunview(r, arg):
    """firstrunview: the view a new install starts in, read from the app at first run (the firstrun-defaults
    session: settings.txt holds only the window's size, so the app's own defaults apply) and from the source
    (BridgeSettings::default; the enums' #[default] variants); fact firstrun-view."""
    vm = r.s.ev("root.uiBridge.spectrogramViewMode")
    dm = r.s.ev("root.uiBridge.spectrogramDisplayMode")
    vis = r.s.ev("viewerController.visualizationMode")
    cfgdir = os.path.join(r.s.a.iso, "config")
    src = os.environ.get("SHOT_SRC", "")
    mod = open(os.path.join(src, "src/analysis/mod.rs")).read() if os.path.exists(os.path.join(src, "src/analysis/mod.rs")) else ""
    dflt = lambda enum: (re.search(r"pub enum " + enum + r"\s*\{[^}]*?#\[default\]\s*(\w+)", mod) or [None, "?"])[1]
    views = {"0": "the mixed view (one plot for all channels)", "1": "per channel (a lane per channel)"}
    vises = {"0": "the spectrogram", "1": "the waveform"}
    st = os.path.join(cfgdir, "ferrous", "settings.txt")
    written = open(st).read().strip().replace("\n", "; ") if os.path.exists(st) else "none"
    _fact(r, f"firstrun-view-{r.s.tag}", f"A new install starts in {views.get(str(vm), f'view mode {vm}')} and "
          f"{vises.get(str(vis), f'visualization {vis}')} (read from the app at first run: spectrogramViewMode {vm}, "
          f"spectrogramDisplayMode {dm}, visualizationMode {vis}; the source's defaults: SpectrogramViewMode::"
          f"{dflt('SpectrogramViewMode')}, SpectrogramDisplayMode::{dflt('SpectrogramDisplayMode')}; BridgeSettings::default: "
          f"{_bridge_default(src)}). The settings file at that moment: {written}. Every other image uses the harness's "
          f"fixed settings: {_harness_settings()}.")


def m_motif(r, arg):
    """motif: the no-art mark (the now-playing cover without art, 1:1) and the spectrogram mode glyph (the mode
    switch's first segment, 1:1 and 4x), beside which the icon is judged; details/motif-mark, details/motif-glyph."""
    from PIL import Image
    sc = r.s.scale
    im = _grab_window(r, "motif-mark")
    rs = r.s.visible_rects("nowPlayingCover")
    if not rs:
        raise LookupError("motif: no nowPlayingCover shown")
    _, x, y, w, h = max(rs, key=lambda v: v[3] * v[4])
    _save_shot(r, im.crop(tuple(int(round(v * sc)) for v in (x, y, x + w, y + h))), "details", "motif-mark",
               f"The no-art mark, the now-playing cover without art, 1:1 ({w:.0f}x{h:.0f})")
    # the mark where it lives: the whole now-playing panel, 1:1
    rr = r.s.rect("nowPlayingPanel")
    if not rr:
        raise LookupError("motif: no nowPlayingPanel")
    m_ = 16  # (its surroundings: the window around it)
    npp = im.crop(tuple(int(round(v * sc)) for v in (max(0, rr[0] - m_), max(0, rr[1] - m_), rr[0] + rr[2] + m_, rr[1] + rr[3] + m_)))
    _save_shot(r, npp, "details", "motif-panel", f"The now-playing panel with the no-art mark, 1:1 ({npp.width}x{npp.height})")
    pane = r.s.rect("spectrogramPane")
    r.s.move(pane[0] + pane[2] * 0.5, pane[1] + pane[3] * 0.5)
    time.sleep(0.6)
    b = r.s.rect("visualizationModeButton0")
    if not b:
        raise LookupError("motif: no visualizationModeButton0")
    g = _grab_window(r, "motif-glyph").crop(tuple(int(round(v * sc)) for v in (b[0], b[1], b[0] + b[2], b[1] + b[3])))
    G = Image.new("RGB", (g.width * 5 + 30, g.height * 4 + 20), (78, 82, 88))
    G.paste(g, (10, 10))
    G.paste(g.resize((g.width * 4, g.height * 4), Image.NEAREST), (20 + g.width, 10))
    _save_shot(r, G, "details", "motif-glyph", "The spectrogram mode glyph (the mode switch's first segment), 1:1 and 4x")
    # their strokes measured as the icon's are (the mark on the floor, the glyph on its segment's fill)
    # (2 px in from the crops' edges: the light theme's 1 px edge and the segment's plate edge are not strokes)
    mk = im.crop(tuple(int(round(v * sc)) for v in (x + 2, y + 2, x + w - 2, y + h - 2))).convert("RGBA")
    _fact(r, f"motif-mark-{int(w)}", f"No-art mark at {w:.0f} px ({r.s.tag}): {_icon_measure2(mk, [])}")
    gi = g.crop((2, 2, g.width - 2, g.height - 2)).convert("RGBA")
    _fact(r, "motif-glyph", f"Mode glyph ({r.s.tag}, {g.width}x{g.height} px, 2 px in): {_icon_measure2(gi, [])}")
    # the mark's top trace followed to its exit (the 4th harmonic's, at 360 px: the top edge or the right one)
    gm = _icon_geom(mk)
    _fact(r, f"motif-mark-exit-{int(w)}", f"No-art mark at {w:.0f} px ({r.s.tag}): {gm['n']} traces at 50 % across; the top "
          f"one exits {({'right': 'the right edge, rows ', 'top': 'the top edge, '}.get(gm['exits'], 'short of the edges, rows '))}"
          f"{gm['exit_rows']}, {gm['air']} px of air above it there")
    # the glyph before (the snapshot's file swapped into the real button) and after (the installed one), with the
    # spectrogram segment selected and with the waveform one, 1:1 and 3x; its strokes measured from the two SVGs
    src = os.environ.get("SHOT_SRC", "")
    g_after = os.path.join(src, "ui/qml/assets/spectrogram-symbolic.svg")
    g_before = None if not ICON_BEFORE_DIR else os.path.join(ICON_BEFORE_DIR, "glyph", "spectrogram-symbolic.svg")
    if g_before and not os.path.exists(g_before):
        raise FileNotFoundError(f"motif: no 'before' glyph at {g_before} (unset SHOT_ICON_BEFORE to leave it out)")
    # only when the glyph changed: an unchanged glyph has no history to show
    if g_before and open(g_before, "rb").read() == open(g_after, "rb").read():
        if r.s.tag == "dark" and "font24" not in (r.s.suffix or ""):
            _fact(r, "motif-glyph-lines", f"Mode glyph unchanged since {ICON_BEFORE_LABEL}: " + _glyph_lines_text(_glyph_lines(g_after, 16), 16))
        r.s.move(r.s.size[0] - 5, r.s.size[1] * 0.45)
        return
    btn0 = f"{FIND}('visualizationModeButton0')"
    src0 = r.s.ev(f"String({btn0}.icon.source)")
    gpx = int(float(r.s.ev(f"{btn0}.icon.width") or 0) or 16)
    mode0 = r.s.ev("viewerController.visualizationMode")
    cells = []
    for gname, gfile in ([(f"before ({ICON_BEFORE_LABEL})", g_before)] if g_before else []) + [("installed", None)]:
        r.s.ev(f"{btn0}.icon.source = {json.dumps('file://' + gfile if gfile else src0)}")
        for mode, lab in ((0, "glyph selected"), (1, "waveform selected")):
            r.s.ev(f"viewerController.setVisualizationMode({mode})")
            r.s.ready(timeout=3)
            r.s.move(pane[0] + pane[2] * 0.4, pane[1] + pane[3] * 0.5)
            time.sleep(0.8)
            sw = r.s.rect("visualizationModeSwitch")
            if not sw:
                raise LookupError("motif: no visualizationModeSwitch")
            ck = [i for i in (0, 1) if r.s.ev(f"String({FIND}('visualizationModeButton{i}').checked)") == "true"]
            if ck != [mode]:
                raise LookupError(f"motif: switch shows {ck} checked, want [{mode}]")
            im2 = _grab_window(r, f"motif-switch-{mode}-{'b' if gfile else 'a'}")
            cells.append((f"{gname}, {lab}", im2.crop(tuple(int(round(v * sc)) for v in (sw[0] - 4, sw[1] - 4, sw[0] + sw[2] + 4, sw[1] + sw[3] + 4)))))
    r.s.ev(f"{btn0}.icon.source = {json.dumps(src0)}")
    if r.s.ev(f"String({btn0}.icon.source)") != src0:
        raise RuntimeError("motif: the glyph's source did not come back")
    r.s.ev(f"viewerController.setVisualizationMode({mode0})")
    r.s.ready(timeout=3)
    gl_a = _glyph_lines(g_after, gpx)
    gl_b = _glyph_lines(g_before, gpx) if g_before else None
    if r.s.tag == "dark" and "font24" not in (r.s.suffix or ""):
        _fact(r, "motif-glyph-lines", f"Mode glyph strokes from the SVG, drawn at {gpx} px in the switch: installed "
              + _glyph_lines_text(gl_a, gpx) + (f" | before ({ICON_BEFORE_LABEL}) " + _glyph_lines_text(gl_b, gpx) if gl_b else ""))
    from PIL import ImageDraw, ImageFont
    fr = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 12)
    Z = 3
    cw_, ch_ = max(c.width for _, c in cells), max(c.height for _, c in cells)
    colw = max(cw_ * Z, 200) + 14
    short = lambda gl: "; ".join(f"{['lower', 'upper'][i] if i < 2 else i}: {v['width']} px, rises {v['rise']} px"
                                 for i, v in enumerate(gl))
    notes = ([f"before: {short(gl_b)}"] if gl_b else []) + [f"installed: {short(gl_a)}"]
    S = Image.new("RGB", (10 + len(cells) * colw, 22 + ch_ + 6 + ch_ * Z + 8 + 16 * len(notes) + 8), (78, 82, 88))
    d = ImageDraw.Draw(S)
    for k, (lab, c) in enumerate(cells):
        x0 = 10 + k * colw
        d.text((x0, 4), lab, fill=(255, 220, 120), font=fr)
        S.paste(c, (x0, 22))
        S.paste(c.resize((c.width * Z, c.height * Z), Image.NEAREST), (x0, 22 + ch_ + 6))
    for j, t in enumerate(notes):
        d.text((10, 22 + ch_ + 6 + ch_ * Z + 6 + 16 * j), t + f" (at {gpx} px)", fill=(230, 230, 230), font=fr)
    _save_shot(r, S, "details", "glyph-history", f"The mode glyph before and installed in the real switch, each segment "
                                                 f"selected, 1:1 and {Z}x ({r.s.tag})")
    r.s.move(r.s.size[0] - 5, r.s.size[1] * 0.45)


def m_icons_ab(r, arg):
    """When variant B icons exist (ICON_B_DIR), one sheet: A (installed, from the source tree) vs B at every
    size, on Breeze dark and light, 1x and (16-64 px) 4x nearest-neighbour."""
    r.s.draws_without_app_pixels()  # (drawn from the icon's SVGs)
    from PIL import Image, ImageDraw, ImageFont
    if r.s.tag != "dark":  # (the icons session runs both themes; this sheet has both grounds already, made once)
        return
    if ICON_AB and not os.path.isdir(_icon_b_root()):
        raise FileNotFoundError(f"icon A/B: no variant B at {_icon_b_root()}")  # loudly, never a sheet without it
    variants = ("A", "B") if ICON_AB else ("A",)
    src = os.environ.get("SHOT_SRC", "")
    out = os.environ["SHOT_OUT"]
    raw = os.path.join(os.path.dirname(out.rstrip("/")), "icons-raw")
    os.makedirs(raw, exist_ok=True)
    sizes = [16, 22, 32, 48, 64, 128, 256]
    files = {}
    for var in variants:
        for px in sizes:
            svg = (os.path.join(src, f"ui/assets/icons/hicolor/{px}x{px}/apps/ferrous.svg") if var == "A" else icon_b_svg(px))
            if var == "A" and not os.path.exists(svg):
                svg = os.path.join(src, "ui/assets/icons/hicolor/scalable/apps/ferrous.svg")
            if not svg:
                if var == "A":
                    raise FileNotFoundError(f"icons: no installed icon at {px} px in {src}")
                raise FileNotFoundError(f"icons: variant B has no {px} px icon (nor a scalable one) in {_icon_b_root()}")
            o = os.path.join(raw, f"ferrous-{var}-{px}.png")
            res = r.s.cmd(f"svg {svg} {px} {o}")
            if not res.startswith("ok"):  # loudly: never a sheet with an icon left out
                raise RuntimeError(f"icons: {svg} at {px} px did not render: {res}")
            files[(var, px)] = o
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 22)
    f = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 16)
    fbs = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 16)
    grounds = (("Breeze Dark window", (32, 35, 38), (220, 220, 220)), ("Breeze Light window", (239, 240, 241), (35, 38, 41)))
    # Per ground: 1x at 16-128 px on the left, 4x of 16-48 px on the right; A above B. No side over the
    # reviewers' 1568 px, so the 1x icons stay 1x.
    # (with B beside A, the 4x row stops at 32 px so the sheet stays 1:1 within 1.15 MP; 48 px is at 1x)
    one, four = [16, 22, 32, 48, 64, 128], ([16, 22, 32] if ICON_AB else [16, 22, 32, 48])
    gap = 22
    w1 = sum(p + gap for p in one)
    w4 = sum(p * 4 + gap for p in four)
    lab_h = 22
    rh1, rh4 = lab_h + 128 + 24, lab_h + 48 * 4 + 24
    rh = max(rh1, rh4)
    pw = 20 + w1 + 40 + w4 + 10
    ph = 30 + len(variants) * rh + 10
    S = Image.new("RGB", (pw + 40, 60 + len(grounds) * (ph + 16) + 10), (78, 82, 88))
    d = ImageDraw.Draw(S)
    d.text((14, 12), f"1x at 16-128 px (left), 4x of 16-{four[-1]} px (right), on Breeze Dark and Light windows",
           fill=(255, 255, 255), font=fb)
    for gi, (gname, bg, fg) in enumerate(grounds):
        x0, y = 20, 60 + gi * (ph + 16)
        d.rectangle((x0, y, x0 + pw, y + ph), fill=bg)
        d.text((x0 + 10, y + 6), gname, fill=fg, font=f)
        yy = y + 30
        for var, vlabel in (("A", ICON_A_LABEL), ("B", ICON_B_LABEL))[:len(variants)]:
            d.text((x0 + 10, yy), vlabel, fill=fg, font=fbs)
            for zoom, sz_list, xs in ((1, one, x0 + 10), (4, four, x0 + 20 + w1 + 30)):
                x = xs
                if zoom == 4:
                    d.text((x, yy), "4x", fill=fg, font=fbs)
                for px in sz_list:
                    if (var, px) in files:
                        ic = Image.open(files[(var, px)]).convert("RGBA")
                        if zoom != 1:
                            ic = ic.resize((px * zoom, px * zoom), Image.NEAREST)
                        S.paste(ic, (x, yy + lab_h + (rh - lab_h - 24 - px * zoom)), ic)
                    d.text((x, yy + rh - 22), f"{px}", fill=fg, font=f)
                    x += px * zoom + gap
            yy += rh
    name = "iconab_a-vs-b_dark"
    S.save(os.path.join(out, name + ".png"))
    r.s.index.append(dict(file=name + ".png", area="iconab", state="a-vs-b", theme="dark", scale=1,
                          size=f"{S.width}x{S.height}", renderer=r.s.renderer, **r.s.composite_evidence(name + ".png"),
                          desc=(f"App icon, {ICON_A_LABEL} (this source) vs {ICON_B_LABEL} ({ICON_B_DIR}), 1x at 16-128 "
                                "px and 4x of 16-32 px on Breeze Dark and Light (Qt's SVG renderer)") if ICON_AB else
                          "App icon (installed, this source), 1x at 16-128 px and 4x of 16-48 px on Breeze Dark and Light"
                          " (Qt's SVG renderer)"))


def _tint(im, box, inset=3):
    """The most frequent colour inside box (inset), as (r, g, b): a flat control's fill, its glyph aside."""
    from collections import Counter
    x, y, w, h = [int(round(v)) for v in box]
    c = im.crop((x + inset, y + inset, x + w - inset, y + h - inset))
    return Counter(getattr(c, "get_flattened_data", c.getdata)()).most_common(1)[0][0]


def _contrast(a, b):
    def lum(c):
        v = [x / 255 for x in c]
        v = [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in v]
        return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2]
    la, lb = sorted((lum(a), lum(b)), reverse=True)
    return (la + 0.05) / (lb + 0.05)


@contextlib.contextmanager
def tooltip_held(r, name):
    """The item's tooltip held back (its delay a minute) while grabs of the item without it are taken, then its own
    delay again. The grabs take as long as the machine is busy (each encodes a PNG and walks the layout): against
    the tooltip's 700 ms they lost when eight apps started together, and the states after them found the button
    still pressed."""
    item = f"{FIND}('{name}')"
    delay = r.s.ev(f"{item}.ToolTip.delay")
    if not isinstance(delay, (int, float)):
        raise RuntimeError(f"tooltip_held: {name} gave no tooltip delay ({delay})")
    held = r.s.ev(f"(function(){{var b={item};b.ToolTip.delay=60000;return b.ToolTip.delay}})()")
    try:
        if held != 60000:
            raise RuntimeError(f"tooltip_held: {name}'s tooltip delay stayed {held}")
        yield
    finally:
        r.s.ev(f"(function(){{{item}.ToolTip.delay={int(delay)};return true}})()")


def m_closeups(r, arg):
    """Close-ups at 4x nearest-neighbour, one image: a transport button at rest, hovered and pressed (held,
    released off it) with each fill's sampled colour and its ratio to the bar; the level meter's top at 2x on the
    spectrogram (scale on and off), on the waveform and paused; and the queue's thumb above the meter in context."""
    from PIL import Image, ImageDraw, ImageFont
    out = os.environ["SHOT_OUT"]
    raw = os.path.join(os.path.dirname(out.rstrip("/")), "closeup-raw")
    os.makedirs(raw, exist_ok=True)
    sc = r.s.scale
    Z = 3  # 3x (was 4x), the sheet within ~1 MP

    def grab(tag, settle=True):
        p = os.path.join(raw, f"{tag}_{r.s.suffix}.png")
        if settle:
            r.s.settle_opacity()
        res = r.s.raw_grab(p)
        if not str(res).startswith("ok"):  # (never an older grab at this path, without its walk)
            raise RuntimeError(f"closeups: grab {tag}: {res}")
        tips = [q for q in r.s.popups() if q.get("tooltip") and q["opacity"] > 0.02]
        if tips:  # loudly: a "1 px band" in a pressed crop was once the tooltip's edge fading in
            raise RuntimeError(f"closeups: a tooltip shows in the {tag} grab (opacity {tips[0]['opacity']:.2f}, "
                               f"{tips[0].get('name') or tips[0].get('cls')} at {tips[0].get('rect')})")
        return Image.open(p).convert("RGB")
    def need(name):  # a missing item fails the macro: never a silent skip
        rect = r.s.rect(name)
        if not rect:
            raise LookupError(f"closeups: no item {name}")
        return rect
    btn = "transportButton_next"
    br = need(btn)
    bar = need("transportBar")
    shots = []
    if br:
        cx, cy = br[0] + br[2] / 2, br[1] + br[3] / 2
        r.s.away()
        time.sleep(0.25)
        shots.append(("at rest", grab("btn-rest")))
        # rest, hover and pressed all without the button's tooltip (hover 0.2 s, press 0.15 s, no settle wait), from a
        # start with no tooltip shown (one of an earlier state's may still be fading out)
        end = time.time() + 1.5
        while any(q.get("tooltip") and q["opacity"] > 0.02 for q in r.s.popups()) and time.time() < end:
            time.sleep(0.05)
        with tooltip_held(r, btn):
            r.s.move(cx, cy)
            time.sleep(0.2)  # the hover tint's fade (100 ms)
            shots.append(("hovered", grab("btn-hover", settle=False)))
            r.s.cmd(f"press {cx:.1f} {cy:.1f}")
            time.sleep(0.15)
            shots.append(("pressed (held)", grab("btn-press", settle=False)))
            if [round(v) for v in (need(btn) or [])] != [round(v) for v in br]:  # one crop rectangle for all three
                raise RuntimeError(f"closeups: {btn} moved while pressed ({need(btn)} vs {br})")
            ax, ay = r.s.size[0] - 5, r.s.size[1] * 0.45
            r.s.cmd(f"dmove {ax:.1f} {ay:.1f}")
            r.s.cmd(f"release {ax:.1f} {ay:.1f}")  # released off the button: no click
            r.s.away()
    ctx = None  # (the queue thumb in context is out of the core sheet; the meter edge sheet has its place)
    # The meter (part of the plot; on the spectrogram its fill takes the colour map's colour of its level;
    # ticks at 0/-12/-24/-48 dB only while the plot shows its scale): its column's top at 2x, per state, with the
    # frequency scale shown (the meter's scaleShown is gone) and the plot's mode (colorMapFill is gone too), so a
    # tile's ticks and fill can be checked against what it claims.
    meter_states = (("spectrogram, scale on", "viewerController.setVisualizationMode(0);root.uiBridge.setShowSpectrogramScale(true)", 0.8),
                    ("spectrogram, scale off", "root.uiBridge.setShowSpectrogramScale(false)", 0.5),
                    ("waveform", "viewerController.setVisualizationMode(1)", 1.2),
                    ("paused, spectrogram, scale on",
                     "viewerController.setVisualizationMode(0);root.uiBridge.setShowSpectrogramScale(true);root.uiBridge.pause()", 1.8))
    was = r.s.ev("JSON.stringify([root.uiBridge.showSpectrogramScale, viewerController.visualizationMode])")
    meter_shots = []
    for label, js, wait in meter_states:
        r.s.ev(f"(function(){{{js};return true}})()")
        time.sleep(wait)
        props = r.s.ev(f"(function(){{var m={FIND}('levelMeter');return m?JSON.stringify([root.uiBridge.showSpectrogramScale&&viewerController.visualizationMode===0,viewerController.visualizationMode===0]):''}})()")
        if not props:
            raise LookupError("closeups: no item levelMeter")
        g_ = grab("meter-" + re.sub(r"[^a-z]+", "-", label).strip("-"))
        # each lane's level and peak (dB) as the meter has them at the grab (laneReadout: -inf is silence)
        rd_ = r.s.ev(f"(function(){{var m={FIND}('levelMeter');var o=[];for(var i=0;i<m.laneCount;i++){{var x=m.laneReadout(i);"
                     f"o.push([isFinite(x.level)?x.level:null,isFinite(x.peak)?x.peak:null])}}return JSON.stringify(o)}})()")
        lanes = json.loads(rd_) if rd_ else None
        if lanes is None:
            raise LookupError("closeups: the meter gave no lane readout")
        meter_shots.append((label, g_, need("levelMeter"), json.loads(props) + [lanes]))
    want = {"spectrogram, scale on": [True, True], "spectrogram, scale off": [False, True], "waveform": [False, False]}
    for label, _, _, props in meter_shots:  # loudly: the states set up are the states shown
        if label in want and props[:2] != want[label]:
            raise RuntimeError(f"closeups: meter '{label}' has scaleShown/spectrogram {props}, expected {want[label]}")
    try:
        sc0, mode0 = json.loads(was)
        r.s.ev(f"(function(){{viewerController.setVisualizationMode({int(mode0)});"
               f"root.uiBridge.setShowSpectrogramScale({'true' if sc0 else 'false'});return true}})()")
    except (TypeError, ValueError):
        raise RuntimeError(f"closeups: could not read the plot's mode and scale ({was})")
    r.s.ev("root.uiBridge.play()")
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 20)
    f = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 15)
    tiles = []  # (heading, note lines, crop)
    bar_fill = None
    if br and shots:
        bx, by, bw, bh = [v * sc for v in br]
        pad = 6 * sc
        if bar:  # the bar's own fill, left of the transport buttons
            bar_fill = _tint(shots[0][1], (bar[0] * sc + 2, bar[1] * sc + 2, 10 * sc, bar[3] * sc - 4), inset=0)
        for label, im in shots:
            fill = _tint(im, (bx, by, bw, bh))
            crop = im.crop((int(bx - pad), int(by - pad), int(bx + bw + pad), int(by + bh + pad)))
            note = ["fill #%02x%02x%02x" % fill]
            if bar_fill:
                note.append(f"{_contrast(fill, bar_fill):.2f}:1 to the bar" + (" #%02x%02x%02x" % bar_fill if label == "at rest" else ""))
            tiles.append((label, note, crop))
    for label, im, mr, props in meter_shots:
        x, y, w, h = [v * sc for v in mr]
        top = min(h, 110 * sc)
        plot, head = {"spectrogram, scale on": ("spectrogram", "scale on"), "spectrogram, scale off": ("spectrogram", "scale off"),
                      "waveform": ("waveform", "waveform")}.get(label, ("spectrogram", "paused"))
        dbs = ["L", "R", "C", "LFE", "Ls", "Rs"] if len(props[2]) == 6 else ["L", "R"] if len(props[2]) == 2 else \
            [str(i + 1) for i in range(len(props[2]))]
        fmt = lambda v: "-inf" if v is None else f"{v:.1f}"
        tiles.append((head, [plot, f"{'ticks' if props[0] else 'no ticks'}, {'spectrogram' if props[1] else 'waveform'} fill"]
                      + [f"{dbs[i]} {fmt(lv)} dB (pk {fmt(pk)})" for i, (lv, pk) in enumerate(props[2][:2])],
                      # (clamped to the window: past its right edge the crop was a black strip)
                      im.crop((max(0, int(x - 40 * sc)), int(y), min(im.width, int(x + w + 4 * sc)), int(y + top))), 2))
    if ctx is not None:
        tiles.append(("in context, 2x", ["the queue thumb's foot", "above the meter (playing)"], ctx, 2))
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 20)
    f = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 15)
    fh = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 15)
    probe = ImageDraw.Draw(Image.new("RGB", (8, 8)))
    cols = []
    tiles = [t if len(t) == 4 else t + (Z,) for t in tiles]  # (heading, notes, crop, zoom)
    for head, note, c, z in tiles:
        tw = max([c.width * z] + [probe.textlength(t, font=f) for t in note] + [probe.textlength(head, font=fh)])
        cols.append(int(tw) + 18)
    th = max((c.height * z for _, _, c, z in tiles), default=0)
    W = max(sum(cols) + 28, 900)
    nl = max(len(n) for _, n, _, _ in tiles)
    S = Image.new("RGB", (W, 46 + 22 + 20 * nl + 8 + th + 16), (78, 82, 88))
    d = ImageDraw.Draw(S)
    d.text((14, 10), f"{r.s.tag.capitalize()}: {btn} at {Z}x, one crop for all three (x {int(br[0] - 6)}, y {int(br[1] - 6)}, "
                       f"{int(br[2] + 12)}x{int(br[3] + 12)} px); meter top at 2x, dB at the grab", fill=(255, 255, 255), font=fb)
    x = 14
    for (head, note, c, z), cw in zip(tiles, cols):
        d.text((x, 46), head, fill=(255, 220, 120), font=fh)
        for i, t in enumerate(note):
            d.text((x, 68 + 20 * i), t, fill=(230, 230, 230), font=f)
        S.paste(c.resize((c.width * z, c.height * z), Image.NEAREST), (x, 46 + 22 + 20 * nl + 8))
        x += cw
    name = f"closeup_controls_{r.s.suffix}"
    S.save(os.path.join(out, name + ".png"))
    r.s.index.append(dict(file=name + ".png", area="closeup", state="controls", theme=r.s.tag, scale=r.s.scale,
                          size=f"{S.width}x{S.height}", renderer=r.s.renderer, window="main", **r.s.composite_evidence(name + ".png"),
                          desc=f"Close-ups at {Z}x: a transport button at rest, hovered and pressed with sampled fills; "
                               "the level meter's top at 2x on the spectrogram (scale on, off), the waveform and paused; the queue thumb above it"))


RING_STATES = [  # (label, injected Last.fm state); sending and scrobbled side by side
    ("listening 0 %", '{"lastFmScrobbleListenedMs":0}'), ("listening 25 %", '{"lastFmScrobbleListenedMs":30000}'),
    ("listening 50 %", '{"lastFmScrobbleListenedMs":60000}'), ("listening 90 %", '{"lastFmScrobbleListenedMs":108000}'),
    ("paused", '{"lastFmScrobblingPaused":true}'),
    ("sending", '{"lastFmScrobbleState":2}'), ("scrobbled", '{"lastFmScrobbleState":3}'),
    ("queued (offline)", '{"lastFmPendingScrobbleCount":3,"lastFmScrobbleMessage":"network unreachable"}'),
    ("failed", '{"lastFmScrobbleState":4,"lastFmScrobbleMessage":"Invalid session key"}'),
]


def m_ring_progression(r, arg):
    """Every scrobble-indicator state (listening at 0/25/50/90 %, paused, sending, scrobbled, queued, failed), beside
    a running library scan's bar (injected, total known), so the two bars can be told apart: per state, the status
    bar's right end at 1x (actual pixels: the scan bar to the indicator) and the indicator (the 16 px glyph and the
    2 px bar beneath it) at 6x nearest-neighbour; once on top, "Playing" at 1x. No wider than the reviewers'
    1568 px, so the 1x crops stay 1x. A missing item fails the macro (never another item in its place)."""
    from PIL import Image, ImageDraw, ImageFont
    out = os.environ["SHOT_OUT"]
    raw = os.path.join(os.path.dirname(out.rstrip("/")), "ring-raw")
    os.makedirs(raw, exist_ok=True)
    m_fakebridge(r, 'statusBar {"libraryScanInProgress":true,"libraryScanProcessed":57,"libraryScanTotal":180}')
    sc = r.s.scale

    def need(name):
        rect = r.s.rect(name)
        if not rect:
            raise LookupError(f"ring_progression: no item {name}")
        return rect
    cells, top = [], None
    try:
        for label, over in RING_STATES:
            m_lastfm(r, over)
            r.s.away()
            r.s.settle_opacity()
            box, bar, scan = need("scrobbleIndicatorBox"), need("statusBar"), need("statusBarScanBar")
            p = os.path.join(raw, f"ring_{r.s.suffix}_{len(cells)}.png")
            res = r.s.raw_grab(p)
            if not str(res).startswith("ok"):  # (never an older grab at this path, without its walk)
                raise RuntimeError(f"ring_progression: grab {label}: {res}")
            im = Image.open(p).convert("RGB")
            x, y, w, h = [int(round(v * sc)) for v in box]
            pad = 4 * sc
            ind = im.crop((x - pad, y - pad, x + w + pad, y + h + pad))
            bx, by, bw, bh = [int(round(v * sc)) for v in bar]
            sx = int(round(scan[0] * sc))
            right = im.crop((sx - 10 * sc, by, min(bx + bw, x + w + 12 * sc), by + bh))
            if top is None:
                top = (im.crop((bx, by, bx + 104 * sc, by + bh)), right)
            cells.append((label, ind, right))
    finally:
        m_lastfm(r, '{"lastFmShowScrobbleProgress":false}')
        m_fakebridge(r, "statusBar reset")
    fb = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Bold.ttf", 20)
    f = ImageFont.truetype("/usr/share/fonts/google-noto/NotoSans-Regular.ttf", 15)
    Z, per_row = 6, 5
    rw = max(c[1].width for c in cells) * Z
    rh = max(c[1].height for c in cells) * Z
    one_w = max(c[2].width for c in cells)
    bar_h = max(c[2].height for c in cells)
    col = max(rw, one_w) + 22
    cell_h = 24 + bar_h + 8 + rh + 16
    rows = -(-len(cells) // per_row)
    top_h = 30 + top[0].height + 14
    S = Image.new("RGB", (max(per_row * col + 24, top[0].width + top[1].width + 120), 46 + top_h + rows * cell_h + 8),
                  (78, 82, 88))
    d = ImageDraw.Draw(S)
    d.text((14, 10), f"{r.s.tag.capitalize()}: a library scan running beside it (injected); 1x, then {Z}x; states injected",
           fill=(255, 255, 255), font=fb)
    d.text((14, 46), "1x: the bar's left end (\"Playing\") … its right end (the scan bar, then the indicator), listening 0 %",
           fill=(230, 230, 230), font=f)
    S.paste(top[0], (14, 70))
    d.text((14 + top[0].width + 6, 70 + top[0].height // 2 - 10), "…", fill=(255, 255, 255), font=f)
    S.paste(top[1], (14 + top[0].width + 30, 70))
    for i, (label, ind, right) in enumerate(cells):
        x0, y0 = 14 + (i % per_row) * col, 46 + top_h + (i // per_row) * cell_h
        d.text((x0, y0), label, fill=(255, 220, 120), font=f)
        S.paste(right, (x0, y0 + 24))
        S.paste(ind.resize((ind.width * Z, ind.height * Z), Image.NEAREST), (x0, y0 + 24 + bar_h + 8))
    name = f"scrobblering_progression_{r.s.suffix}"
    S.save(os.path.join(out, name + ".png"))
    r.s.index.append(dict(file=name + ".png", area="scrobblering", state="progression", theme=r.s.tag,
                          scale=r.s.scale, size=f"{S.width}x{S.height}", renderer=r.s.renderer, window="main",
                          **r.s.composite_evidence(name + ".png"),
                          desc="Scrobble indicator, every state, beside a running library scan's bar: the status bar's "
                               "right end at 1x, the indicator (glyph and bar) at 6x (states injected)"))


MACROS = dict(dialogmenu=m_dialogmenu, onetoone=m_onetoone, edge4x=m_edge4x, crop11=m_crop11, artqueue=m_artqueue, platehover=m_platehover, kwinprobe=m_kwinprobe, treekeys=m_treekeys, glyphline=m_glyphline, searchfield=m_searchfield, largelib=m_largelib, inwindow=m_inwindow, rollprobe=m_rollprobe, floorprobe=m_floorprobe, appicon5=m_appicon5, appicon6=m_appicon6, appicon7=m_appicon7, appicon8=m_appicon8, appicon9=m_appicon9, appicon10=m_appicon10, appicon11=m_appicon11, appicon4=m_appicon4, appicon3=m_appicon3, appicon2=m_appicon2, appicon=m_appicon, motif=m_motif, firstrunview=m_firstrunview, lanesrest=m_lanesrest, platesizes=m_platesizes, meterprobe=m_meterprobe, cardgeom=m_cardgeom, titlelines=m_titlelines, statusjumps=m_statusjumps, queuefocus=m_queuefocus, queuenow=m_queuenow, queuegeom=m_queuegeom, unreadable=m_unreadable, fsitem=m_fsitem, screenfacts=m_screenfacts, itunesfail=m_itunesfail, notefact=m_notefact, platefact=m_platefact, fillfact=m_fillfact, lastfmaccount=m_lastfmaccount, tagsavefail=m_tagsavefail, renamebatch=m_renamebatch, quitwait=m_quitwait, dlgtab=m_dlgtab, dlgfacts=m_dlgfacts, itunesapply=m_itunesapply, infoedge=m_infoedge, markprobe=m_markprobe, restprobe=m_restprobe, switchstates=m_switchstates, rowdetail=m_rowdetail, dialogwalk=m_dialogwalk, checkann=m_checkann, launch_focus=m_launch_focus, closeups=m_closeups, ring_progression=m_ring_progression, opendialog=m_opendialog, openprefs=m_openprefs, icons_ab=m_icons_ab, measure=m_measure, measure_na=m_measure_na, fakebridge=m_fakebridge, lastfm=m_lastfm, itunes=m_itunes, open_child=m_open_child, open_named=m_open_named,
              tagedit=m_tagedit, tab=m_tab, icons=m_icons)

# The input-latency baseline: its rectangle's name (the strips' crop) and its scripts.
LATENCY_PROBE_RECT = "latencyBaseline"  # the strip crop: the item, by name
LATENCY_PROBE_ADD = ("(function(){var o=Qt.createQmlObject('import QtQuick; Rectangle { objectName: \\'latencyBaseline\\';"
                     " x: 1560; y: 34; z: 100000; width: 32; height: 32; color: \\'#000000\\'; MouseArea { anchors.fill: parent;"
                     " onPressed: parent.color = (parent.color == \\'#000000\\' ? \\'#ffffff\\' : \\'#000000\\') } }',"
                     " root.contentItem, 'latencyBaseline');return !!o})()")
LATENCY_PROBE_FLIP = ("(function(){var it=" + FIND + "('latencyBaseline');it.color=(it.color=='#000000'?'#ffffff':'#000000');"
                      "return true})()")
LATENCY_PROBE_REMOVE = "(function(){var it=" + FIND + "('latencyBaseline');if(it)it.destroy();return true})()"

# Broadband fixtures (make_broadband_fixtures.py), outside the library
BB_DIR = os.path.join(os.path.dirname(LIB), "broadband")
BB_STEREO = os.path.join(BB_DIR, "01 - White Noise.flac")
BB_51 = os.path.join(BB_DIR, "02 - Pink Noise 5.1.flac")
BB_TST = os.path.join(BB_DIR, "03 - Tone Silence Tone.flac")
BB_LEVELS = os.path.join(BB_DIR, "04 - Moving Levels.flac")  # music-like levels for the meter's stills
# a tagged track with a title long enough to elide (the fixture library's)
_LONG_TITLE_TRACK = sorted(glob.glob(os.path.join(LIB, "The Quiet Meridian", "1998 - Glass Orchard", "02 - A Remarkably Long*.flac"))
                           or ["02 - missing long title.flac"])[0]
_NORMAL_TRACK = sorted(glob.glob(os.path.join(LIB, "The Quiet Meridian", "2005 - Small Hours", "01 - *.flac")) or ["01 - missing.flac"])[0]
# stand-in cover cases (make_standin_fixtures.py): an album that wraps, no album, album = title; and the
# library's untagged file
SI_DIR = os.path.join(os.path.dirname(LIB), "standin")
SI_FILES = [("long-album", os.path.join(SI_DIR, "01 - Long Album.flac"), "an album title long enough to wrap"),
            ("no-album", os.path.join(SI_DIR, "02 - No Album Here.flac"), "no album tag (the stand-in has no text)"),
            ("same-name", os.path.join(SI_DIR, "03 - Same Name.flac"), "an album named as the track (no text)"),
            ("untagged", os.path.join(LIB, "Unsorted", "track07.flac"), "an untagged file (title from its name)")]


# a 192 kHz track (its analysis takes a moment)
_DUST_MOTES = sorted(glob.glob(os.path.join(LIB, "Lumen Drift", "2013 - Dust Motes", "*.flac")) or ["dust motes missing.flac"])[0]
# a short tagged track with art (the queue-end recording)
_SHORE_TRACK = os.path.join(LIB, "The Quiet Meridian", "2005 - Small Hours", "06 - Shore.flac")
# the title cases of the large-font card (title beside the cover on up to two lines): a short title, a medium
# one, a long one, an untagged file; each card shot at rest and its title's lines measured (macro titlelines)
_TITLE_CASES = [("short", os.path.join(LIB, "The Quiet Meridian", "2005 - Small Hours", "06 - Shore.flac"), "a short title"),
                ("medium", os.path.join(LIB, "The Quiet Meridian", "2005 - Small Hours", "02 - Morning Tide Field.flac"),
                 "a medium title"),
                ("long", _LONG_TITLE_TRACK, "a long title"),
                ("untagged", os.path.join(LIB, "Unsorted", "track07.flac"), "an untagged file (the title from its name)")]


def _title_lines(sess, what, restore):
    out = [(sess, "_", "_titles-load", "js root.uiBridge.replaceWithPaths(" + json.dumps([p_ for _, p_, _ in _TITLE_CASES])
            + f") | until root.uiBridge.queueLength === {len(_TITLE_CASES)} | js viewerController.setVisualizationMode(0)", "")]
    for i, (key, path, desc) in enumerate(_TITLE_CASES):
        frag = os.path.basename(path)
        out.append((sess, "nowplaying", f"title-{key}",
                    f"js root.uiBridge.playAt({i}) | until root.uiBridge.currentTrackPath.indexOf({json.dumps(frag)}) >= 0 | "
                    f"wait 1.0 | away | macro titlelines {key}", f"{what}: {desc}"))
    out.append((sess, "_", "_titles-close", "js root.uiBridge.stop() | " + restore, ""))
    return out


def _standin_lines(sess, what, restore):
    """The stand-in cover for each SI_FILES case , as now playing shots of the session `sess`."""
    out = [(sess, "_", "_standin-load", "js root.uiBridge.replaceWithPaths(" + json.dumps([p for _, p, _ in SI_FILES]) + ") | "
            f"until root.uiBridge.queueLength === {len(SI_FILES)} | js viewerController.setVisualizationMode(0)", "")]
    for i, (key, path, desc) in enumerate(SI_FILES):
        frag = os.path.basename(path)
        out.append((sess, "nowplaying", f"standin-{key}",
                    f"js root.uiBridge.playAt({i}) | until root.uiBridge.currentTrackPath.indexOf({json.dumps(frag)}) >= 0 | "
                    f"until root.uiBridge.currentTrackCoverPath === '' | wait 0.8 | away | expect track-no-cover:{frag} | "
                    f"macro cardgeom {key}",
                    f"{what}: no album art, {desc}"))
    out.append((sess, "_", "_standin-close", "js root.uiBridge.stop() | " + restore, ""))
    return out  # 4 s tone, 4 s digital silence, 4 s tone

def _pref_pages():
    """Configure's pages (title, state slug), read from the source's PreferencesDialog.qml, so the states follow a
    reorganised dialog (Library, Visualization, General, Last.fm). Without a source (pack.py), a fixed list."""
    src = os.environ.get("SHOT_SRC", "")
    if not src:
        titles = ["Library", "Visualization", "General", "Last.fm"]
    else:
        q = os.path.join(src, "ui/qml/dialogs/PreferencesDialog.qml")
        txt = open(q, encoding="utf-8").read()
        titles = re.findall(r'title:\s*I18n\.i18nc\("@title:tab preferences page",\s*"([^"]+)"\)', txt)
        if not titles:  # loudly: never states for pages the dialog does not have
            raise RuntimeError(f"no Configure pages found in {q}")
    return [(t, re.sub(r"[^a-z0-9]+", "-", t.lower().replace(".", "")).strip("-")) for t in titles]


PREF_PAGES = _pref_pages()

Q = "queue"
LRow = "libraryRow"
QRow = "queueRow"

STATES = [
    # ------------------------------------------------------------ first run
    ("empty", "main", "first-run-empty-library", "wait 0.5", "First run: no library folder, empty queue, stopped"),
    # a new install: no library, and the app's own default settings (settings.txt holds only the window's size)
    ("firstrun-defaults", "main", "first-run-defaults", "wait 0.5 | macro firstrunview",
     "First run of a new install: the app's own default settings (only the window size given), no library folder"),
    ("empty", "main", "first-run-add-folder-hover", "hover firstRunAddFolderButton 0.8",
     "First run: pointer over the Add Library Folder button (its hover; it has no tooltip)"),
    ("empty", "motion", "first-scan",
     f"away | strip 2.5 0.1 - :: js root.uiBridge.addLibraryRoot('{LIB}', 'Fixtures')",
     "First scan of the fixture library: the status bar scan indicator and the tree filling in"),
    ("empty", "main", "first-run-after-scan", "until root.uiBridge.libraryTrackCount > 100 && !root.uiBridge.libraryScanInProgress | wait 0.6 | away", "First run after the first scan (library filled, queue empty)"),
    ("empty", "statusbar", "scan-finished", "away", "Status bar after the first scan finished"),

    # ------------------------------------------------------------ library + queue + main states
    ("main", "main", "library-idle", "away | shown nowPlayingIdleRow", "Library loaded (one root, collapsed), nothing queued, stopped: now playing is its compact \"Nothing playing\" row; the plot at rest (its rest lines, and per channel the seam and L/R plates, without the scale)"),
    ("main", "statusbar", "idle", "away", "Status bar idle: library loaded, nothing playing"),
    # the idle row itself (the panel's crop), 10 pt wide
    ("main", "nowplaying", "idle-row", "away | shown nowPlayingIdleRow", "Now playing idle: the \"Nothing playing\" row, 10 pt"),
    ("main", "plot", "floor-empty", "away",
     "Pane before any audio (nothing played yet), the frequency scale off: the pane at rest (its rest lines show "
     "only with the scale on)"),
    ("main", "_", "_floor-unfilled", "away | macro floorprobe unfilled:whole", ""),  # the never-played floor, sampled
    # the never-played pane in the mixed view (the default: one lane, rest lines) and per channel (a stereo
    # pair: seam, L/R plates), its lines and plates sampled; the view put back after
    ("main", "_", "_rest-save", "macro floorprobe save", ""),
    ("main", "plot", "floor-empty-mixed", "js root.uiBridge.setSpectrogramViewMode(0) | wait 0.5 | away | macro restprobe mixed",
     "The pane before any audio, the mixed view (one lane): rest lines only"),
    ("main", "plot", "floor-empty-lanes", "js root.uiBridge.setSpectrogramViewMode(1) | wait 0.5 | away | macro restprobe lanes",
     "The pane before any audio, per channel (a stereo pair): the seam, the L/R plates, rest lines"),
    ("main", "plot", "floor-empty-waveform", "js viewerController.setVisualizationMode(1) | wait 0.6 | away",
     "The pane before any audio in the waveform, per channel"),
    ("main", "_", "_rest-restore", "macro floorprobe restore | wait 0.3", ""),
    ("main", "plot", "floor-empty-scale", "js root.uiBridge.setShowSpectrogramScale(true) | wait 0.4 | away",
     "Pane before any audio with the frequency scale on: the rest lines (kHz grid lines, intended) with their labels"),
    ("main", "_", "_scale-off", "js root.uiBridge.setShowSpectrogramScale(false) | wait 0.2", ""),
    ("main", "library", "hover", f"hover {LRow}@0.3,3.5", "Library: pointer over the 4th artist row, nothing selected"),
    ("main", "library", "selected", f"click {LRow}@0.3,2.5 | wait 0.4 | away", "Library: 3rd artist row selected by click"),
    ("main", "library", "expanded-keyboard", f"click {LRow}@0.3,2.5 | key Right | wait 0.3 | key Down | key Right | wait 0.3 | away",
     "Library: artist and its first album expanded with Right; album row selected by keyboard"),
    ("main", "library", "expanded-hover", f"hover {LRow}@0.3,6.5", "Library: expanded tree, pointer over a track row"),
    # Context menus are opened from the keyboard where the view takes the Menu key (or Shift+F10): the menu then
    # opens on the selected row, as for a keyboard user, not wherever the pointer is.
    ("main", "menu", "context-library-track", f"click {LRow}@0.3,6.5 | away | key Menu | settle 0.8", "Library context menu on a track row (Menu key)"),
    ("main", "menu", "context-library-album", f"esc | click {LRow}@0.3,3.5 | away | key Menu | settle 0.8", "Library context menu on an album row (Menu key)"),
    ("main", "menu", "context-library-artist", f"esc | click {LRow}@0.3,2.5 | away | key Menu | settle 0.8", "Library context menu on an artist row (Menu key)"),
    ("main", "library", "collapsed-again", f"esc | click {LRow}@0.3,2.5 | key Left | wait 0.3 | away",
     "Library: the artist collapsed again with Left (selection stays)"),
    # (refilling the queue while playing goes on playing: fill it, then stop, and wait until the bridge says Stopped)
    ("main", "queue", "loaded-stopped", "queue | wait 0.3 | js root.uiBridge.stop() | until root.uiBridge.playbackState === 'Stopped' | wait 0.6 | away",
     "Queue filled (9 albums incl. 5.1/7.1/192 kHz/MP3/untagged), stopped; header with totals"),
    ("main", "main", "playing-spectrogram-centered", "play Small_Hours 70 6 | away",
     "Playing a stereo 44.1 kHz track at 1:10, centered spectrogram, per-channel lanes"),
    ("main", "plot", "floor-spectrogram", "away", "Pane floor: spectrogram playing"),
    ("main", "transport", "at-rest", "away", "Transport at rest while playing"),
    ("main", "nowplaying", "at-rest", "away | macro cardgeom normal", "Now playing at rest"),
    # paused, and stopped with the track still loaded (Stop alone, not Clear); then playing again
    ("main", "nowplaying", "paused", "js root.uiBridge.pause() | wait 0.8 | away", "Now playing, paused"),
    ("main", "nowplaying", "stopped-with-track", "js root.uiBridge.stop() | wait 0.8 | away",
     "Now playing, stopped with the track still loaded (Stop alone)"),
    ("main", "_", "_np-resume", "play Small_Hours 70 6 | away", ""),
    ("main", "queue", "at-rest", "away", "Queue at rest (playing row)"),
    ("main", "library", "at-rest", "away", "Library at rest"),
    ("main", "plot", "at-rest", "away", "Spectrogram pane at rest (playing)"),
    ("main", "statusbar", "at-rest", "away", "Status bar at rest (playing)"),
    ("main", "queue", "hover", f"hover {QRow}@0.3,4.5", "Queue: pointer over row 5 while row 1 plays"),
    ("main", "queue", "selected", f"click {QRow}@0.3,2.5 | wait 0.3 | away", "Queue: row 3 selected (single)"),
    ("main", "queue", "multi-selection", f"click {QRow}@0.3,2.5 | key Down shift | key Down shift | wait 0.3 | away",
     "Queue: rows 3-5 selected with Shift+Down (list focused)"),
    ("main", "menu", "context-queue-multi", "key Menu | settle 0.8", "Queue context menu on a three-row selection (Menu key)"),
    ("main", "menu", "context-queue-playing-row", f"esc | click {QRow}@0.3,0.5 | away | key Menu | settle 0.8",
     "Queue context menu on the playing row (Menu key)"),
    ("main", "library", "follows-playing", "esc | away | macro treekeys", "Library while row 1 plays: the tree marks the playing track"),
    ("main", "library", "expanded-playing",
     "js libraryController.showTrackInLibrary(root.uiBridge.queuePathAt(0)) | wait 0.6 | away | macro treekeys",
     "Library expanded to the playing track (Show in Library): its artist and album open, the playing row in the tree"),
    ("main", "library", "collapsed-holding-playing", "js libraryController.collapseAll() | wait 0.3 | away",
     "Library: all collapsed while a track inside 'The Quiet Meridian' plays"),
    ("main", "queue", "scrolled", f"js {FIND}('playlistView').positionViewAtIndex(28, ListView.Center) | wait 0.4",
     "Queue: long queue scrolled to the middle (scroll bar shown)"),
    ("main", "main", "playing-spectrogram-rolling",
     f"js {FIND}('playlistView').positionViewAtBeginning() | js root.uiBridge.setSpectrogramDisplayMode(0) | wait 3 | "
     "macro rollprobe main-rolling-3s",
     "Playing, rolling spectrogram, 3 s after entering rolling mode"),
    ("main", "main", "playing-spectrogram-rolling-9s", "wait 6 | macro rollprobe main-rolling-9s",
     "Playing, rolling spectrogram, 9 s after entering rolling mode"),
    ("main", "main", "playing-frequency-scale",
     "js root.uiBridge.setSpectrogramDisplayMode(1) | js root.uiBridge.setShowSpectrogramScale(true) | wait 0.5",
     "Playing, centered spectrogram with the frequency scale on"),
    ("main", "main", "playing-scale-levelmeter", "js root.uiBridge.setShowLevelMeter(true) | wait 0.5",
     "Playing, frequency scale and level meter on"),
    ("main", "_", "_edge", "macro edge4x main", ""),
    ("main", "main", "playing-levelmeter", "js root.uiBridge.setShowSpectrogramScale(false) | wait 0.5",
     "Playing, level meter on (scale off)"),
    # the pane alone, spectrogram with the meter; the mode switch's states and the lane heads (Mute/Solo)
    ("main", "plot", "spectrogram-levelmeter", "away | expect spectrogram-meter", "The plot pane, spectrogram (2 ch), level meter on"),
    ("main", "_", "_switchstates", "unfocus | macro switchstates | away", ""),
    ("main", "_", "_switchstates-wave", "unfocus | macro switchstates waveform | away", ""),  # on the waveform's plate
    # Mute and Solo checked with the pointer away (per channel), spectrogram and waveform
    ("main", "_", "_lanes-rest", "macro floorprobe save | js root.uiBridge.setSpectrogramViewMode(1) | wait 0.5 | "
     "macro lanesrest | macro floorprobe restore | away", ""),
    ("main", "_", "_infoedge", "unfocus | macro infoedge | away", ""),  # the art viewer's info toggle edge
    ("main", "main", "playing-waveform-levelmeter", "js viewerController.setVisualizationMode(1) | ready 3",
     "Playing, waveform view with the level meter on"),
    ("main", "_", "_platehover", "unfocus | away | macro platehover", ""),
    ("main", "plot", "waveform-levelmeter", "away | expect waveform-meter", "The plot pane in waveform mode with the level meter on"),
    ("main", "main", "playing-waveform", "js root.uiBridge.setShowLevelMeter(false) | wait 0.5", "Playing, waveform view"),
    ("main", "plot", "floor-waveform", "away", "Pane floor: waveform playing"),
    ("main", "plot", "waveform-hover", "hover mainWaveformSurface@0.3,0.4 0.8", "Pointer over the waveform pane"),
    ("main", "plot", "hover-crosshair",
     "away | js viewerController.setVisualizationMode(0) | ready 2 | hover spectrogramPaneItem@0.3,0.4 0.8",
     "Pointer over the spectrogram: crosshair readout, lane heads, mode switch"),
    ("main", "plot", "zoomed-hover", "wheel spectrogramPaneItem@0.3,0.4 120 4 | wait 1.2",
     "Main pane zoomed in 4 wheel notches (x2.44), pointer over the plot: the zoom readout by the mode switch"),
    ("main", "_", "_plates-zoom", "macro platesizes visualizationModeSwitch,plotZoomPlate", ""),  # plate sizes, padding
    # (the windowed spectrogram viewer's corner plate: the viewer opened windowed in the main window, then closed again;
    # the fullscreen viewer has a strip instead)
    ("main", "_", "_plates-viewer", "macro platesizes @viewer:viewerCornerPlate", ""),
    ("main", "plot", "zoomed", "away | wait 0.3", "Main pane zoomed in, pointer away"),
    ("main", "menu", "view-zoomed", "menu 2", "View menu while zoomed in"),
    ("main", "plot", "mode-switch-hover", "esc | js resetZoomAction.trigger() | hover visualizationModeButton1 1.3",
     "Visualization mode switch: Waveform button hovered (tooltip)"),
    ("main", "main", "paused", "away | js root.uiBridge.pause() | wait 0.4", "Paused, centered spectrogram"),
    ("main", "menu", "playback-paused", "menu 3", "Playback menu while paused"),
    ("main", "main", "stopped", "esc | js root.uiBridge.stop() | wait 0.4 | away", "Stopped after playing (queue kept)"),
    ("main", "menu", "playback-stopped", "menu 3", "Playback menu while stopped (disabled items)"),
    ("main", "_", "_close", "esc", ""),

    # ------------------------------------------------------------ playing variants
    ("playing", "_", "_queue", "queue | away", ""),
    ("playing", "main", "analysing-192k", "start Dust_Motes | wait 0.25",
     "Just after Play on a 192 kHz track (analysis may already be done; see the analysing strip)"),
    ("playing", "main", "playing-192k", "wait 4", "Playing a 192 kHz/24-bit stereo track"),
    ("playing", "main", "playing-5.1-spectrogram", "play Afterimage - 5", "Playing 5.1 24-bit/48 kHz: six lanes with channel heads"),
    ("playing", "plot", "5.1-levelmeter-lanes", "js root.uiBridge.setShowLevelMeter(true) | wait 0.5 | hover spectrogramPaneItem@0.5,0.5 0.8",
     "Playing 5.1 with the level meter on, pointer over the pane: six lanes with their M/S buttons"),
    ("playing", "_", "_meter-51", "macro meterprobe 5.1", ""),  # the ladder meter on six lanes
    ("playing", "plot", "5.1-meter-rest", "away | wait 1.2",
     "Playing 5.1, the level meter on, the pointer away (the switch and the lane buttons hidden)"),
    ("playing", "viewer", "5.1-fullscreen",
     "js root.uiBridge.setViewerFullscreenMode(1) | js viewerController.openSpectrogramViewer() | settle 1 | ready 3 | "
     "window spectrogramFullscreenWindow | move xy:1000,700 | wait 0.6",
     "Playing 5.1 in the fullscreen viewer (2000x1400), meter on, pointer over the plot: six lanes in a tall window"),
    ("playing", "_", "_5.1-fullscreen-close", "window spectrogramFullscreenWindow | esc | window  | "
     "js viewerController.closeSpectrogramViewer() | js root.uiBridge.setViewerFullscreenMode(0) | settle 1 | away", ""),
    ("playing", "_", "_meter-off", "js root.uiBridge.setShowLevelMeter(false) | wait 0.3 | away", ""),
    ("playing", "plot", "5.1-hover", "hover spectrogramPaneItem@0.5,0.5 0.8",
     "Pointer over the 5.1 spectrogram: crosshair and lane mute/solo buttons"),
    ("playing", "plot", "5.1-lane-mute-hover", "hover plotLaneMuteButton#1 1.3", "Lane head: Mute hovered (tooltip)"),
    ("playing", "plot", "5.1-lane-muted", "click plotLaneMuteButton#1 | wait 0.8", "Lane 2 muted"),
    # spectrogram to waveform per channel (six lanes) and mixed (one lane): no empty frame (plot content)
    ("playing", "_", "_unmute", "click plotLaneMuteButton#1 | away | ready 3", ""),
    ("playing", "motion", "mode-switch-5.1", "strip 0.9 0.1 spectrogramPane :: js viewerController.setVisualizationMode(1)",
     "Mode switch, spectrogram to waveform, 5.1 per channel (six lanes): the plot's drawn content through the switch"),
    ("playing", "main", "playing-5.1-waveform", "away | ready 3", "Playing 5.1: waveform lanes"),
    ("playing", "main", "playing-5.1-downmix",
     "js viewerController.setVisualizationMode(0) | js root.uiBridge.setSpectrogramViewMode(0) | ready 3",
     "Playing 5.1, downmix (single lane) spectrogram"),
    # (the first switch in the mixed view on this track, then back, then a later one)
    ("playing", "motion", "mode-switch-mixed-first", "strip 0.9 0.1 spectrogramPane :: js viewerController.setVisualizationMode(1)",
     "FIRST mode switch to the waveform in the mixed view (the 5.1 downmix, one lane) on this track"),
    ("playing", "_", "_mixed-back-warm", "js viewerController.setVisualizationMode(0) | ready 3 | wait 0.5", ""),
    ("playing", "motion", "mode-switch-mixed", "strip 0.9 0.1 spectrogramPane :: js viewerController.setVisualizationMode(1)",
     "Mode switch, spectrogram to waveform, mixed (the 5.1 downmix, one lane), a LATER switch (the first is "
     "mode-switch-mixed-first): the plot's drawn content through the switch"),
    # (the mixed view's switch back, waveform to spectrogram, recorded)
    ("playing", "motion", "mode-switch-mixed-back", "strip 0.9 0.1 spectrogramPane :: js viewerController.setVisualizationMode(0)",
     "Mode switch back, waveform to spectrogram, in the mixed view (the 5.1 downmix, one lane)"),
    ("playing", "_", "_spectrogram-again", "js viewerController.setVisualizationMode(0) | ready 3", ""),
    ("playing", "main", "playing-7.1-spectrogram", "js root.uiBridge.setSpectrogramViewMode(1) | play Winter_Suite - 5",
     "Playing 7.1 (eight lanes)"),
    ("playing", "main", "playing-ends-early", "play Cut_Tape - 7 | away",
     "Playing a FLAC cut off at 55 % of its header's length (ends-early state; an issue is reported)"),
    ("playing", "main", "playing-ends-early-waveform", "js viewerController.setVisualizationMode(1) | ready 2.5",
     "Ends-early track in the waveform view: 'The file ends at …' note"),
    # the plot after the cut (the file's audio ends at 32.7 s of its header's 60 s), paused 1.2 s before it and
    # zoomed out 4 notches (the view then reaches past the cut; at the default zoom it ends there), from the cut
    # marker (the spectrogram's markerX) to the pane's end: centered one lane, per channel, rolling
    ("playing", "_", "_cut-floor", "js viewerController.setVisualizationMode(0) | macro floorprobe save | "
     "js root.uiBridge.setSpectrogramDisplayMode(1) | js root.uiBridge.setSpectrogramViewMode(0) | "
     "js root.uiBridge.pause() | js root.uiBridge.seek(31.5) | wait 0.5 | wheel spectrogramPaneItem@0.5,0.4 -120 4 | wait 1 | "
     "ready 3 | away | "
     "macro floorprobe ends-early-centered:marker | "
     "js root.uiBridge.setSpectrogramViewMode(1) | wait 0.8 | away | macro floorprobe ends-early-lanes:marker | "
     "js root.uiBridge.setSpectrogramDisplayMode(0) | wait 0.8 | away | macro floorprobe ends-early-rolling:marker | "
     "js resetZoomAction.trigger() | macro floorprobe restore", ""),
    ("playing", "main", "short-track-stretched",
     "js viewerController.setVisualizationMode(0) | play Brief_Signal - 2.5 | js root.uiBridge.pause() | wait 0.4",
     "A 4 s track, paused: stretched default zoom"),
    ("playing", "main", "short-track-waveform", "js viewerController.setVisualizationMode(1) | ready 2",
     "The 4 s track in the waveform view"),
    ("playing", "nowplaying", "missing-tags", "js viewerController.setVisualizationMode(0) | play track07 - 3",
     "Now playing an untagged file without cover (title from the file name; the cover's placeholder)"),
    ("playing", "main", "playing-missing-tags", "until root.uiBridge.currentTrackCoverPath === '' | away | wait 0.5",
     "Main window, playing a file without tags (its title from the file name; no artist, album or cover)"),
    ("playing", "nowplaying", "no-cover", "play Static_Bloom - 3 | wait 0.8 | macro cardgeom no-cover",
     "Now playing a tagged track from an album without cover art"),
    ("playing", "main", "playing-no-cover", "until root.uiBridge.currentTrackCoverPath === '' | away | wait 0.5",
     "Main window, playing a track without album art (Static Bloom): the now playing without a cover"),
    ("playing", "_", "_markprobe", "away | macro markprobe nowPlayingCover", ""),  # the no-art mark, sampled
    # the library's no-art thumbnail (a 16 px glyph) on the playing album's row, 1:1 and 4x
    ("playing", "_", "_noart-glyph", f"js libraryController.showTrackInLibrary({json.dumps(_NA_TRACK)}) | wait 0.6 | away | "
     "macro rowdetail glyph | js libraryController.collapseAll() | wait 0.3", ""),
    ("playing", "nowplaying", "embedded-cover", "play Harbour_Lights - 3", "Now playing a track whose cover is embedded"),
    ("playing", "nowplaying", "cover-hover", "hover nowPlayingCover 0.8", "Pointer over the now-playing cover (no tooltip)"),
    ("playing", "menu", "context-cover", "rclick nowPlayingCover", "Now-playing cover context menu"),
    ("playing", "nowplaying", "long-title", "esc | away | play Remarkably - 3",
     "Now playing a track with a very long title (elision)"),
    ("playing", "nowplaying", "long-title-hover", "hover nowPlayingWideText 1.5", "Long title hovered: full-text tooltip"),
    ("playing", "nowplaying", "unicode", "away | play Hanamura - 3", "Now playing a Japanese title"),
    ("playing", "nowplaying", "mp3", "play Kites - 3", "Now playing an MP3 (format in the status bar)"),
    # Broadband (fixtures/broadband, outside the library): bright data under the plot's labels, plates and meter
    ("playing", "_", "_broadband-stereo", "js root.uiBridge.replaceWithPaths(" + json.dumps([BB_STEREO]) + ") | "
     "until root.uiBridge.queueLength === 1 | js root.uiBridge.playAt(0) | until root.uiBridge.playbackState === 'Playing' | "
     "js root.uiBridge.setShowLevelMeter(true) | js root.uiBridge.setShowSpectrogramScale(true) | wait 2.5 | away", ""),
    ("playing", "main", "broadband-spectrogram", "hover spectrogramPaneItem@0.45,0.3 0.8",
     "Playing broadband white noise (bright data): the frequency scale, lane heads, plates, crosshair and level meter over it"),
    ("playing", "_", "_broadband-51", "js root.uiBridge.replaceWithPaths(" + json.dumps([BB_51]) + ") | "
     "until root.uiBridge.queueLength === 1 | js root.uiBridge.playAt(0) | until root.uiBridge.playbackState === 'Playing' | "
     "wait 2.5 | away", ""),
    ("playing", "main", "broadband-5.1", "hover spectrogramPaneItem@0.45,0.3 0.8",
     "Playing broadband pink noise in 5.1 (bright data): six lanes, their heads, plates and the level meter"),
    ("playing", "viewer", "broadband-5.1-fullscreen",
     "js root.uiBridge.setViewerFullscreenMode(1) | js viewerController.openSpectrogramViewer() | settle 1 | ready 3 | "
     "window spectrogramFullscreenWindow | move xy:1000,500 | wait 0.6",
     "Broadband 5.1 in the fullscreen viewer (2000x1400), meter and scale on, pointer over the plot"),
    ("playing", "_", "_broadband-close", "window spectrogramFullscreenWindow | esc | window  | "
     "js viewerController.closeSpectrogramViewer() | js root.uiBridge.setViewerFullscreenMode(0) | settle 1 | "
     "js root.uiBridge.setShowLevelMeter(false) | js root.uiBridge.setShowSpectrogramScale(false) | queue | away", ""),

    # the plot's empty regions, sampled (m_floorprobe, floors.jsonl): the 12 s tone-silence-tone fixture paused
    # in the spectrogram (one lane) at the default zoom without the short-track stretch (stretched, the track fills
    # the pane and has no outside). Centered: its silence (6 s: left of the playhead), after its end (the pane right
    # of the track). Centered never shows before the start (its view starts at the track's start): rolling, 0.4 s in
    ("playing", "_", "_floor-tone", "macro floorprobe save | js root.uiBridge.replaceWithPaths(" + json.dumps([BB_TST]) + ") | "
     "until root.uiBridge.queueLength === 1 | js viewerController.setVisualizationMode(0) | "
     "js root.uiBridge.setSpectrogramViewMode(0) | js root.uiBridge.setSpectrogramDisplayMode(1) | "
     "js root.uiBridge.setStretchShortTracks(false) | "
     "js root.uiBridge.playAt(0) | until root.uiBridge.playbackState === 'Playing' | wait 1 | js root.uiBridge.pause() | "
     "until root.uiBridge.playbackState === 'Paused' | ready 4", ""),
    ("playing", "_", "_floor-silence", "js root.uiBridge.seek(6) | wait 0.8 | away | macro floorprobe silence:playhead", ""),
    ("playing", "_", "_floor-post", "js root.uiBridge.seek(11.6) | wait 0.8 | away | macro floorprobe post-end:right", ""),
    ("playing", "_", "_floor-pre", "js root.uiBridge.setSpectrogramDisplayMode(0) | js root.uiBridge.seek(0.4) | wait 0.8 | "
     "away | macro floorprobe pre-start:left", ""),
    ("playing", "_", "_floor-close", "js root.uiBridge.stop() | macro floorprobe restore | queue | away", ""),
] + _standin_lines("playing", "Now playing (wide)", "queue | away") + [
    # a track change across cards of different metadata: untagged -> a normal tagged track with art -> the
    # long album without art; the library header's travel measured (expected 0 px)
    ("playing", "_", "_meta-queue", "js root.uiBridge.replaceWithPaths(" + json.dumps([SI_FILES[3][1], _NORMAL_TRACK, SI_FILES[0][1]])
     + ") | until root.uiBridge.queueLength === 3 | js viewerController.setVisualizationMode(0) | js root.uiBridge.playAt(0) | "
     "until root.uiBridge.currentTrackPath.indexOf('track07') >= 0 | wait 1.5 | away", ""),
    ("playing", "motion", "track-change-metadata",
     "strip 2.8 0.1 sidebarPane:0,0,1,0.62 :: js root.uiBridge.next() ; wait 1.4 ; js root.uiBridge.next()",
     "Track changes across metadata cards: an untagged file, then a tagged track with album art, then a long album "
     "without art (Next twice, 1.4 s apart): the now-playing card and the library header"),
    # the level meter on music-like levels (-18 dB RMS with dynamics), spectrogram and waveform, per channel
    ("playing", "_", "_levels-load", "js root.uiBridge.replaceWithPaths(" + json.dumps([BB_LEVELS]) + ") | "
     "until root.uiBridge.queueLength === 1 | js viewerController.setVisualizationMode(0) | js root.uiBridge.setSpectrogramViewMode(1) | "
     "js root.uiBridge.setShowLevelMeter(true) | js root.uiBridge.playAt(0) | until root.uiBridge.playbackState === 'Playing' | "
     "wait 3 | ready 3 | away", ""),
    ("playing", "plot", "meter-music-spectrogram", "away | wait 0.3 | expect spectrogram-meter | macro meterprobe music-spectrogram | "
     "hover visualizationModeButton1 0.3",
     "The pane on music-like levels (about -18 dB RMS, peaks near -6 dB): spectrogram per channel, level meter on, the "
     "mode switch hovered (its waveform segment)"),
    # (both from the same playback position, 20 s into the fixture: one meter, the same audio in both modes)
    ("playing", "motion", "meter-motion-spectrogram", "js root.uiBridge.seek(20) | wait 0.6 | away | strip 0.9 0.0 spectrogramPane :: wait 0",
     "The level meter moving on music-like levels, spectrogram, from 20.6 s into the track (every frame for 0.9 s)"),
    ("playing", "plot", "meter-music-waveform", "js viewerController.setVisualizationMode(1) | ready 3 | away | wait 0.3 | "
     "expect waveform-meter | macro meterprobe music-waveform | hover visualizationModeButton0 0.3",
     "The pane on music-like levels: waveform per channel, level meter on, the mode switch hovered (its spectrogram segment)"),
    ("playing", "motion", "meter-motion-waveform", "js root.uiBridge.seek(20) | wait 0.6 | away | strip 0.9 0.0 spectrogramPane :: wait 0",
     "The level meter moving on music-like levels, waveform, from 20.6 s into the track: the same audio as the spectrogram's "
     "(every frame for 0.9 s)"),
    ("playing", "_", "_levels-close", "js viewerController.setVisualizationMode(0) | js root.uiBridge.setShowLevelMeter(false) | "
     "js root.uiBridge.stop() | queue | away", ""),

    # ------------------------------------------------------------ transport
    ("transport", "_", "_play", "queue | play Small_Hours 40 4 | away", ""),
    ("transport", "_", "_closeups", "js root.uiBridge.setShowLevelMeter(true) | wait 0.6 | macro closeups | away | wait 0.5", ""),
    # the transport playing and paused, and a button with keyboard focus
    ("transport", "transport", "playing", "away", "Transport playing"),
    ("transport", "transport", "paused", "js root.uiBridge.pause() | wait 0.6 | away", "Transport paused"),
    ("transport", "transport", "stopped", "js root.uiBridge.stop() | wait 0.6 | away", "Transport stopped (the track loaded)"),
    ("transport", "_", "_resume", "js root.uiBridge.play() | wait 0.6 | away", ""),
    ("transport", "transport", "button-focus", "away | focus transportButton_playPause | wait 0.3",
     "Transport: Play/Pause with keyboard focus"),
] + [
    line for name in ("previous", "playPause", "stop", "next") for line in (
        ("transport", "transport", f"{name}-hover-tooltip", f"unfocus | hover transportButton_{name} 1.4",
         f"Transport: pointer over {name} (hover + tooltip)"),
        ("transport", "transport", f"{name}-pressed",
         f"press transportButton_{name} | wait 0.25", f"Transport: {name} held down (released off the button after)"),
        ("transport", "transport", f"{name}-focus",
         f"dmove xy:900,500 | release xy:900,500 | away | focus transportButton_{name}",
         f"Transport: {name} with keyboard focus (tooltip shows on focus)"),
    )
] + [
    ("transport", "seek", "hover", "unfocus | js root.uiBridge.play() | hover seekSlider@0.62,0.5 0.8",
     "Seek bar: hover line and the time box at 62 %"),
    ("transport", "seek", "dragging",
     "press seekSlider@0.3,0.5 | wait 0.2 | dmove seekSlider@0.4,0.5 | dmove seekSlider@0.45,0.5 | wait 0.4",
     "Seek bar: dragging the playhead (drag time shown)"),
    ("transport", "seek", "focus", "release seekSlider@0.45,0.5 | wait 0.4 | away | focus seekSlider", "Seek bar with keyboard focus"),
    # the seek time box near the bar's start (about 0:05 of 3:49), where it meets the bar's left end
    ("transport", "seek", "hover-start", "away | unfocus | hover seekSlider@0.022,0.5 0.8",
     "Seek bar hovered near its start (about 0:05): the time box"),
    ("transport", "volume", "mute-hover", "unfocus | hover muteButton 1.4", "Volume: pointer over the mute button (tooltip)"),
    ("transport", "volume", "mute-focus", "away | focus muteButton", "Volume: mute button with keyboard focus"),
    ("transport", "volume", "slider-hover", "unfocus | hover volumeSlider 1.4", "Volume: pointer over the slider (tooltip)"),
    ("transport", "volume", "slider-focus", "away | focus volumeSlider", "Volume: slider with keyboard focus"),
    ("transport", "volume", "muted", "unfocus | js muteAction.trigger() | wait 0.6", "Muted"),
    ("transport", "volume", "dragging",
     "js muteAction.trigger() | press volumeSlider@0.9,0.5 | dmove volumeSlider@0.4,0.5 | wait 0.4", "Volume slider being dragged"),
    ("transport", "_", "_release", "release volumeSlider@0.4,0.5 | away", ""),
    # the seek time box during a drag (above the slider: the crop holds the menu bar's row too)
    ("transport", "motion", "seek-drag-box", "away | strip 1.6 0.1 0,0,1600,110 :: press seekSlider@0.3,0.5 ; wait 0.25 ; "
     "dmove seekSlider@0.38,0.5 ; wait 0.15 ; dmove seekSlider@0.46,0.5 ; wait 0.35 ; release seekSlider@0.46,0.5",
     "Seek bar dragged: the time box appearing above the slider while the playhead follows, then leaving on release"),
    ("transport", "motion", "mute-toggle", "away | strip 1.4 0.1 transportBar:0.68,0,0.32,1 :: js muteAction.trigger() ; wait 0.7 ; "
     "js muteAction.trigger()",
     "Mute, then unmute (0.7 s later): the mute glyph and the volume groove's colour"),
    # the queue's playing row selected; keyboard focus in the list; the drop indicator; then emptied
    # keyboard focus reached with real keys: Tab from an unfocused window until the queue's list has focus, then
    # Down (keyboard modality on: the row's ring draws only then); its focus and modality recorded (fact queue-focus-*)
    ("transport", "queue", "focus", "unfocus | macro queuefocus | wait 0.4", "Queue with keyboard focus (real Tab, then Down)"),

    # ------------------------------------------------------------ queue
    # the long queue (50 rows, a scrollbar), row 1 playing; each state measured (macro queuegeom: FOCUS-FACTS' table)
    ("queue", "_", "_play", "queue | play Small_Hours 40 4 | away | macro queuegeom rest", ""),
    # a row pressed and held (the pointer down on row 4), then released and the selection cleared
    ("queue", "queue", "pressed", f"unfocus | press {QRow}@0.3,3.5 | wait 0.35 | macro queuegeom pressed",
     "Queue: the pointer pressed and held on row 4"),
    ("queue", "_", "_release", f"release {QRow}@0.3,3.5 | wait 0.3 | js queueController.clearSelection() | away", ""),
    ("queue", "queue", "hover-selected", f"unfocus | click {QRow}@0.3,2.5 | wait 0.3 | hover {QRow}@0.3,2.5 0.6 | "
     "macro queuegeom hover-selected", "Queue: row 3 selected (list focused) and hovered"),
    ("queue", "queue", "inactive-hover", f"js {FIND}('libraryView').forceActiveFocus() | wait 0.3 | hover {QRow}@0.3,4.5 0.6 | "
     f"until !{FIND_ANY}('playlistView').activeFocus | macro queuegeom inactive-hover",
     "Queue: row 3 selected while the list has no focus (the library has it: the inactive selection), row 5 hovered"),
    ("queue", "queue", "hover-now", f"js queueController.clearSelection() | unfocus | hover {QRow}@0.3,0.5 0.6 | "
     "macro queuegeom hover-now", "Queue: the playing row (1) hovered, nothing selected"),
    # (moved from the transport session, which the queue pack no longer runs)
    ("queue", "queue", "now-selected", f"unfocus | click {QRow}@0.3,0.5 | wait 0.3 | away",
     "Queue: the playing row selected"),
    ("queue", "queue", "hover-now-selected", f"hover {QRow}@0.3,0.5 0.6 | "
     "macro queuegeom hover-now-selected", "Queue: the playing row selected (list focused) and hovered"),
    # selected, focused (ring) and hovered at once: real Tab, then Down (row 2), the pointer on row 2
    ("queue", "queue", "focus-selected-hover", f"js queueController.clearSelection() | away | unfocus | "
     f"macro queuefocus combo Down | hover {QRow}@0.3,1.5 0.6 | macro queuegeom focus-selected-hover",
     "Queue: row 2 selected with the keyboard (real Tab, then Down: focus ring) and hovered"),
    ("queue", "queue", "paused-row", "js queueController.clearSelection() | unfocus | js root.uiBridge.pause() | "
     "until root.uiBridge.playbackState === 'Paused' | wait 0.6 | away | macro queuegeom paused",
     "Queue: the playing row while paused"),
    ("queue", "queue", "stopped-row", "js root.uiBridge.stop() | until root.uiBridge.playbackState === 'Stopped' | wait 0.6 | "
     "away | macro queuegeom stopped", "Queue: the playing row while stopped (the track still loaded)"),
    ("queue", "_", "_resume", "play Small_Hours 40 4 | away", ""),
    # the ring on an unselected row (Tab alone: the first row shown becomes current, nothing selected), the list
    # scrolled to its middle, so the scrollbar's thumb is lower down, away from the ringed row
    ("queue", "queue", "ring-unselected", f"js queueController.clearSelection() | "
     f"js {FIND}('playlistView').positionViewAtIndex(25, ListView.Beginning) | wait 0.3 | away | unfocus | "
     "macro queuefocus unselected - | wait 0.3 | macro queuegeom ring-unselected",
     "Queue scrolled to row 26: keyboard focus by a real Tab alone, the ring on row 26, nothing selected; the thumb lower down"),
    # a column resize handle hovered, then focused (from the list, one more real Tab: the handles follow the rows)
    ("queue", "queue", "handle-hover", f"js {FIND}('playlistView').positionViewAtBeginning() | unfocus | "
     "hover queueColumnHandle_album 0.6", "Queue header: the Album column's resize handle hovered"),
    ("queue", "queue", "handle-focus", "away | unfocus | macro queuefocus handle - | key Tab | wait 0.3 | "
     f"until ['artist','album','length'].some(function(c){{return {FIND_ANY}('queueColumnHandle_'+c).activeFocus}})",
     "Queue header: a column resize handle with keyboard focus (real Tab from the list)"),
    ("queue", "queue", "drop-indicator",
     f"away | unfocus | js (function(){{var a={FIND}('playlistDropArea');var i={FIND_ANY}('queueDropIndicator');"
     f"var v={FIND}('playlistView');var w={FIND}('{QRow}');"
     f"a.queueDropIndicatorY=a.mapFromItem(v,0,0).y+3*w.height-1;i.visible=true;return true}})() | "
     f"until {FIND_ANY}('queueDropIndicator').visible === true | wait 0.3 | away",
     "Queue: the drop indicator between rows 3 and 4 (made visible directly: a drag cannot be driven headless)"),
    ("queue", "_", "_drop-off", f"js {FIND_ANY}('queueDropIndicator').visible = false", ""),
    # keyboard focus in the long list scrolled to its end, beside the scrollbar's thumb: real Tab, then End and Up
    ("queue", "queue", "focus-scrolled", f"js queueController.clearSelection() | js {FIND}('playlistView').positionViewAtEnd() | "
     "wait 0.3 | away | unfocus | macro queuefocus scrolled End,Up | wait 0.3 | macro queuegeom focus-scrolled",
     "Queue: keyboard focus on row 49 of 50 (real Tab, then End and Up), the list scrolled to its end, its scrollbar shown"),
    # long artist, album and title elided (the fixture's long-named ensemble, 9 tracks)
    ("queue", "queue", "elision", f"unfocus | js root.uiBridge.replaceWithPaths({json.dumps(album_paths('The Extraordinarily Long-Named Chamber Ensemble of Upper Westfield/*'))}) | "
     "until root.uiBridge.queueLength === 9 | wait 0.5 | js queueController.clearSelection() | away | macro queuegeom elision",
     "Queue of 9 rows by a long-named ensemble: a long artist, album and track title elided"),
    # a short queue (4 rows, no scrollbar): a real unreadable row, then keyboard focus there
    ("queue", "queue", "unreadable", "js queueController.clearSelection() | unfocus | macro unreadable | "
     "js queueController.clearSelection() | wait 0.3 | away | macro queuegeom unreadable",
     "Queue of 4 rows: row 4 is not audio, just played: the playback error marks it unreadable; it stays the current row"),
    ("queue", "queue", "unreadable-later", "js root.uiBridge.playAt(2) | until root.uiBridge.playbackState === 'Playing' | "
     "wait 0.6 | away | macro queuegeom unreadable-later",
     "Queue of 4 rows: row 4 unreadable after another row (3) plays"),
    ("queue", "queue", "focus-short", "unfocus | macro queuefocus short Down | wait 0.3 | macro queuegeom focus-short",
     "Queue of 4 rows (no scrollbar), row 3 playing: keyboard focus by a real Tab, then Down (the ring on unreadable row 4)"),
    ("queue", "queue", "empty", "js root.uiBridge.stop() | js root.uiBridge.clearQueue() | wait 0.8 | away",
     "Queue emptied (library loaded): the empty queue's label"),

    # ------------------------------------------------------------ status bar
    ("statusbar", "_", "_play", "queue | play Small_Hours 30 3 | away", ""),
    ("statusbar", "statusbar", "playing", "away", "Status bar while playing (state, format)"),
    ("statusbar", "_", "_onetoone", "away | macro onetoone", ""),
    ("statusbar", "statusbar", "format-hover", "hover statusBarFormat@0.05,0.5 0.8", "Status bar: pointer over the format text (no tooltip)"),
    ("statusbar", "statusbar", "repeat-one-shuffle",
     "away | js repeatOneAction.trigger() | js root.uiBridge.setShuffleEnabled(true) | wait 0.2",
     "Status bar with Repeat One and Shuffle on"),
    ("statusbar", "statusbar", "repeat-shuffle-hover", "hover statusBarRepeatShuffle 0.8", "Repeat/shuffle indicator hovered (labelled in words; no tooltip)"),
    ("statusbar", "menu", "playback-repeat-one-shuffle", "away | menu 3", "Playback menu: Repeat One and Shuffle checked"),
    ("statusbar", "statusbar", "repeat-all",
     "esc | js repeatAllAction.trigger() | js root.uiBridge.setShuffleEnabled(false) | wait 0.2", "Status bar with Repeat All"),
    ("statusbar", "_", "_repeat-off", "js repeatOffAction.trigger()", ""),
] + [
    line for look, over in (
        ("listening-0", '{"lastFmScrobbleListenedMs":0}'),
        ("listening-half", '{"lastFmScrobbleListenedMs":60000}'),
        ("listening-almost", '{"lastFmScrobbleListenedMs":110000}'),
        ("sending", '{"lastFmScrobbleState":2}'),
        ("scrobbled", '{"lastFmScrobbleState":3}'),
        ("not-scrobbled", '{"lastFmScrobbleState":6,"lastFmScrobbleIneligibility":3,"lastFmScrobbleListenedMs":45000}'),
        ("queued-offline", '{"lastFmPendingScrobbleCount":3,"lastFmScrobbleMessage":"network unreachable"}'),
        ("failed", '{"lastFmScrobbleState":4,"lastFmScrobbleMessage":"Invalid session key"}'),
        ("paused", '{"lastFmScrobblingPaused":true}'),
    ) for line in (
        ("statusbar", "statusbar", f"lastfm-{look}", f"away | macro lastfm {over}",
         f"Last.fm indicator: {look} (state injected: no network)"),
        ("statusbar", "statusbar", f"lastfm-{look}-tooltip", "hover scrobbleIndicator 1.4", f"Last.fm indicator: {look}, hovered"),
    )
] + [
    ("statusbar", "_", "_glyphline", 'away | macro lastfm {"lastFmScrobbleListenedMs":60000} | wait 0.3 | macro glyphline', ""),
    ("statusbar", "_", "_ring-progression", "away | macro ring_progression", ""),
    ("statusbar", "menu", "context-scrobble-indicator", '''macro lastfm {"lastFmScrobbleListenedMs":60000} | away | focus scrobbleIndicator | key F10 shift | settle 0.8''',
     "Last.fm indicator menu (Shift+F10)"),
    ("statusbar", "_", "_lastfm-off", '''esc | macro lastfm {"lastFmShowScrobbleProgress":false}''', ""),
    ("statusbar", "statusbar", "issues-new",
     "away | js errorController.report(1, 1, '/music/Some Artist/Some Album/01 - Missing.flac', 'The file could not be opened: No such file or directory') | js errorController.report(2, 0, 'Fixtures', 'Could not read 2 files during the scan') | wait 0.8",
     "Status bar: two new (unseen) issues"),
    ("statusbar", "statusbar", "issues-hover", "hover statusBarErrorsButton 1.4", "Issues button hovered (tooltip)"),
    # opened as a user does: a pointer click on the status bar's issues button (its focus as opened in dlg-*)
    ("statusbar", "dialog", "recent-errors", "away | click statusBarErrorsButton | settle 1 | away | "
     "macro notefact recent-issues-opened Recent Issues opened by a pointer click on the status bar's issues button "
     "(statusBarErrorsButton), not by the keyboard", "Recent Issues: one error, one warning (opened by a pointer click)"),
    ("statusbar", "dialog", "recent-errors-focus", "focusdefault", "Recent Issues with keyboard focus on its default control"),
    ("statusbar", "statusbar", "issues-seen", "esc | wait 0.5", "Status bar after the issues were seen"),
    ("statusbar", "statusbar", "scanning-known",
     'away | macro fakebridge statusBar {"libraryScanInProgress":true,"libraryScanProcessed":57,"libraryScanTotal":180}',
     "Library scan in progress, total known: label, count and progress bar (scan state injected)"),
    ("statusbar", "statusbar", "scanning-known-hover", "hover statusBarScan 1.4", "Scan indicator hovered (tooltip)"),
    ("statusbar", "statusbar", "scanning-unknown",
     'away | macro fakebridge statusBar {"libraryScanInProgress":true,"libraryScanProcessed":57,"libraryScanTotal":0}',
     "First scan of a folder, total unknown: the count only, no bar (by design without a total; scan state injected)"),
    ("statusbar", "statusbar", "backend-disconnected", 'macro fakebridge statusBar {"connected":false}',
     "Status bar while the backend is not connected (injected; status bar only)"),
    ("statusbar", "statusbar", "backend-stopped",
     'macro fakebridge statusBar {"backendFailure":"The playback engine stopped unexpectedly (exit code 101)."}',
     "Status bar after the backend stopped (injected; status bar only, the main-window message is not shown)"),
    ("statusbar", "_", "_bridge-reset", "macro fakebridge statusBar reset", ""),
    # a scan's label arriving, counting and leaving in the status bar: the scan state injected step by step (a real
    # scan of the fixture libraries ends before a frame shows the label), the status bar's own response recorded
    ("statusbar", "motion", "scan-injected", 'away | strip 2.6 0.1 statusBar :: macro fakebridge statusBar {"libraryScanInProgress":true,"libraryScanProcessed":0,"libraryScanTotal":180} ; wait 0.5 ; macro fakebridge statusBar {"libraryScanInProgress":true,"libraryScanProcessed":60,"libraryScanTotal":180} ; wait 0.5 ; macro fakebridge statusBar {"libraryScanInProgress":true,"libraryScanProcessed":150,"libraryScanTotal":180} ; wait 0.5 ; macro fakebridge statusBar reset',
     "A library scan in the status bar, its state injected: the label arriving at 0 %, counting to 33 % and 83 %, then leaving"),
    # the status bar's items while the scan count grows (layout jumps), and while a new track analyses
    ("statusbar", "_", "_status-jumps", "away | macro statusjumps", ""),
    ("statusbar", "statusbar", "analysing", "js root.uiBridge.replaceWithPaths(" + json.dumps([_DUST_MOTES]) + ") | "
     "until root.uiBridge.queueLength === 1 | js root.uiBridge.playAt(0) | wait 0.4 | away",
     "Status bar while a new 192 kHz track starts and is analysed (the status bar has no analysis indicator of its own)"),
    ("statusbar", "_", "_analysing-back", "queue | play Small_Hours 30 3 | away", ""),
    ("statusbar", "motion", "rescan", "away | strip 3.0 0.1 statusBar :: js root.uiBridge.rescanAllLibraryRoots()",
     "Library rescan of an unchanged library: it reports no progress, so nothing appears (see scanning-* states)"),

    # ------------------------------------------------------------ menus + search
] + [
    ("menus", "menu", f"{n}-empty-queue", f"esc | away | menu {i}", f"{n.title()} menu, empty queue, stopped (disabled items)")
    for i, n in enumerate(["file", "edit", "view", "playback", "settings", "help"])
] + [
    ("menus", "menu", "file-keyboard", "esc | menu 0 | key Down | key Down | wait 0.3", "File menu, keyboard highlight on item 2"),
    ("menus", "_", "_play", "esc | queue | play Small_Hours 30 2", ""),
] + [
    ("menus", "menu", f"{n}-playing", f"esc | away | menu {i}", f"{n.title()} menu while playing")
    for i, n in enumerate(["file", "edit", "view", "playback", "settings", "help"])
] + [
    ("menus", "menu", "playback-repeat-hover", "esc | menu 3 | hover menuitem:3:-6 0.5", "Playback menu, pointer over a Repeat item"),
    ("menus", "menu", "menubar-hover", "esc | hover menubar:2 0.5", "Menu bar: pointer over View (no menu open)"),
    ("menus", "menubar", "hovered", "esc | away | hover menubar:2 0.5", "Menu bar, pointer over View, no menu open"),
    ("menus", "menubar", "open", "esc | away | menu 0 | hover menuitem:0:1 0.4", "Menu bar with File open, pointer on its second item"),
    ("menus", "_", "_menubar-close", "esc | away", ""),
    ("menus", "search", "empty", "away | js globalSearchController.openDialog() | settle 0.9", "Search palette open, empty query"),
    ("menus", "search", "help-hover", "hover globalSearchHelpButton 1.3", "Search palette: help button hovered"),
    ("menus", "search", "results", "away | type mer | wait 0.8", "Search palette: 'mer' with results"),
    ("menus", "search", "results-keyboard", "key Down | key Down | key Down | wait 0.4", "Search palette: keyboard selection down three rows"),
    # (a row whose tooltip adds to it: the first result's may say nothing more than the row, and then shows none)
    ("menus", "search", "result-hover", "hover @tooltipResultRow@0.15,0.5 1.6", "Search palette: pointer over a result, its tooltip shown"),
    ("menus", "menu", "context-search-result", "rclick globalSearchResultTitle#2",
     "Search result context menu (right click; it opens at the click point; from the keyboard: context-search-result-keyboard)"),
    ("menus", "search", "results-many", "esc 1 | away | key Backspace | key Backspace | key Backspace | type l | wait 0.8",
     "Search palette: 'l' (many results, truncated groups)"),
    ("menus", "search", "no-results", "key Backspace | type zzqx | wait 0.8", "Search palette: no results"),
    ("menus", "search", "results-unicode", "key Backspace | key Backspace | key Backspace | key Backspace | type ørn | wait 0.8",
     "Search palette: non-ASCII query 'ørn'"),
    ("menus", "_", "_close", "esc", ""),

    # ------------------------------------------------------------ dialogs
    ("dialogs", "_", "_play", "queue | play Small_Hours 30 2 | js root.uiBridge.pause()", ""),
    ("dialogs", "dialog", "about", "macro opendialog aboutDialog | settle 0.9 | away", "About Ferrous"),
    ("dialogs", "dialog", "about-default-focus", "focusdefault", "About, keyboard focus on the default button"),
    ("dialogs", "dialog", "about-components", "macro dlgtab aboutTabs 1", "About Ferrous: its Components tab"),
    ("dialogs", "dialog", "about-kde", "esc | macro opendialog aboutKdeDialog | settle 0.9", "About KDE"),
] + [
    ("dialogs", "dialog", f"preferences-{slug}", f"esc | macro openprefs {title} | settle 1.1 | away", f"Preferences: {title} page")
    for title, slug in PREF_PAGES
] + [
    ("dialogs", "dialog", "preferences-default-focus", "focusdefault",
     f"Preferences ({PREF_PAGES[-1][0]} page) with keyboard focus on its default button"),
    ("dialogs", "dialog", f"preferences-{PREF_PAGES[1][1]}-tabbed",
     f"esc | macro openprefs {PREF_PAGES[1][0]} | settle 1 | window preferencesDialog | key Tab | key Tab | wait 0.3 | window",
     f"Preferences: {PREF_PAGES[1][0]} page after two Tab presses"),
    ("dialogs", "dialog", "confirm-remove-library-root",
     "esc | macro openprefs Library | settle 1 | click libraryRootRow@0.5,0.5 | click libraryRootRemoveButton | "
     "waitpopup ConfirmDialog 1.5",
     "Confirmation: Remove Library Folder, opened with the page's Remove button (removeCurrentRoot)"),
    ("dialogs", "dialog", "confirm-remove-library-root-default-focus", "focusdefault", "Remove-folder confirmation, default focus"),
    ("dialogs", "dialog", "library-root-name-rename", f"esc 2 | js root.openLibraryRootNameDialog('rename', '{LIB}', 'Fixtures') | settle 0.9",
     "Library folder name dialog (rename)"),
    ("dialogs", "dialog", "library-root-name-rename-default-focus", "focusdefault", "Rename library folder, default focus"),
    ("dialogs", "dialog", "library-root-name-add", f"esc | js root.openLibraryRootNameDialog('add', '{LIB2}', '') | settle 0.9",
     "Library folder name dialog (add)"),
    ("dialogs", "dialog", "diagnostics", "esc | macro opendialog diagnosticsDialog | settle 1.2 | away", "Help > Diagnostics"),
    ("dialogs", "dialog", "diagnostics-default-focus", "focusdefault", "Diagnostics, default focus"),
    ("dialogs", "dialog", "confirm-clear-diagnostics", "macro open_child diagnosticsDialog ConfirmDialog",
     "Confirmation: clear the diagnostics log (over Diagnostics)"),
    ("dialogs", "dialog", "confirm-clear-diagnostics-default-focus", "focusdefault", "Clear-log confirmation, default focus"),
    ("dialogs", "dialog", "keyboard-shortcuts", "esc 2 | macro opendialog keyboardShortcutsDialog | settle 1.1 | away",
     "Settings > Configure Keyboard Shortcuts"),
    ("dialogs", "dialog", "keyboard-shortcuts-default-focus", "focusdefault", "Configure Keyboard Shortcuts, default focus"),
    ("dialogs", "dialog", "keyboard-shortcuts-custom",
     "js (function(){var d=keyboardShortcutsDialog;d.selectAction('stop');d.setBindings(stopAction,['Ctrl+Alt+S','Media Stop']);"
     "return true})() | settle 0.6", "Configure Keyboard Shortcuts: Stop with keys of its own, not yet applied"),
    ("dialogs", "dialog", "keyboard-shortcuts-refused",
     "js (function(){var d=keyboardShortcutsDialog;d.selectAction('next');d.refusal=d.refusalFor(nextAction,'Del');"
     "return true})() | settle 0.6", "Configure Keyboard Shortcuts: a key the queue keeps, refused"),
    ("dialogs", "dialog", "keyboard-shortcuts-typed-key",
     "js (function(){var d=keyboardShortcutsDialog;d.refusal='';d.selectAction('follow_playhead');return true})() | settle 0.6",
     "Configure Keyboard Shortcuts: Follow Playhead's F, with its note"),
    ("dialogs", "dialog", "keyboard-shortcuts-reassign",
     "js (function(){var d=keyboardShortcutsDialog;d.selectAction('stop');"
     "d.giveBindings(stopAction,d.withKey(d.bindingsOf(stopAction),0,'Space'));return true})() | settle 1 | wait 0.6",
     "Shortcut Conflict: a key Play/Pause has, for Stop (over Configure Keyboard Shortcuts)"),
    ("dialogs", "dialog", "quit-save-failed",
     "esc 2 | js root.showQuitSaveFailedDialog('Settings could not be saved: Permission denied (os error 13)') | settle 0.9",
     "Quit: saving the state failed (opened directly with a sample message)"),
    ("dialogs", "dialog", "quit-save-failed-default-focus", "focusdefault", "Quit save failed, default focus"),
    ("dialogs", "dialog", "tag-editor", "esc | macro tagedit 4 | away", "Edit Tags: four tracks of one album"),
    ("dialogs", "dialog", "tag-editor-all-selected", "js root.tagEditorApi.selectAllRows() | wait 0.5", "Edit Tags: all rows selected (bulk fields)"),
    ("dialogs", "dialog", "tag-editor-one-selected", "js root.tagEditorApi.setSelectedRows([1]) | wait 0.5", "Edit Tags: one row selected"),
    # the same one edit as tag-editor-editing, committed with a real Tab (no second, scripted edit)
    ("dialogs", "dialog", "tag-editor-modified", "window tagEditorDialog | key Tab | wait 0.6 | window ",
     "Edit Tags: the typed title committed with a real Tab (its row's unsaved-change mark)"),
    ("dialogs", "dialog", "tag-editor-auto-number", "macro open_child tagEditorDialog AutoNumberDialog", "Edit Tags > Auto Number"),
    ("dialogs", "dialog", "tag-editor-auto-number-default-focus", "focusdefault", "Auto Number, default focus"),
    ("dialogs", "dialog", "tag-editor-rename-preview",
     "esc | macro renamebatch | js tagEditorDialog.triggerRename() | waitpopup RenamePreviewDialog 2",
     "Edit Tags > Rename Files preview: twelve files (the rename batch), seven renamed, five keeping their names"),
    ("dialogs", "dialog", "tag-editor-rename-preview-default-focus", "focusdefault", "Rename preview, default focus"),
    ("dialogs", "dialog", "tag-editor-status-details", "esc | macro tagsavefail | macro open_child tagEditorDialog TagEditorStatusDetailsDialog",
     "Edit Tags > Details after a real save with one folder not writable (one file saved, one failed)"),
    ("dialogs", "dialog", "tag-editor-quit-wait", "esc | macro quitwait",
     "Edit Tags: 'finishing tag changes' quit wait, two files (the list injected), with Keep Waiting and Quit Anyway"),
    ("dialogs", "dialog", "tag-editor-close-confirm", "esc | js tagEditorDialog.requestClose() | waitpopup TagEditorCloseConfirmDialog 0.8",
     "Edit Tags: close with unsaved changes -> confirmation"),
    ("dialogs", "dialog", "tag-editor-close-confirm-default-focus", "focusdefault", "Close confirmation, default focus"),
    ("dialogs", "_", "_tag-close", "esc | js root.tagEditorApi.revertField('title') | js tagEditorDialog.closeEditor() | wait 0.6 | js root.uiBridge.play() | wait 0.4", ""),
    ("dialogs", "dialog", "itunes-empty", "macro itunes empty | js root.openItunesArtworkDialog() | settle 1 | away",
     "Replace from iTunes: no results (stand-in bridge; no network)"),
    ("dialogs", "dialog", "itunes-searching", "macro itunes searching",
     "Replace from iTunes: searching (injected); the cog is Breeze's running busy indicator, rotating (a still shows "
     "it at rest: its rotation is the strip itunes-searching-spinner)"),
    ("dialogs", "motion", "itunes-searching-spinner", "strip 1.0 0.0 - dialog=Replace :: wait 0",
     "Replace from iTunes, searching: the busy indicator (Breeze's cog) rotating, 1 s of frames in the dialog's own window"),
    ("dialogs", "dialog", "itunes-results", "macro itunes results | wait 0.6",
     "Replace from iTunes: five results (fixture covers as stand-ins; the last one loading)"),
    ("dialogs", "dialog", "itunes-results-default-focus", "focusdefault", "iTunes results, default focus"),
    ("dialogs", "dialog", "confirm-replace-cover", "macro itunesapply 1",
     "Confirmation: replace the album's cover, as Apply on the second result opens it (applySuggestion)"),
    ("dialogs", "viewer", "itunes-preview", "esc | js itunesArtworkDialog.showPreview(1) | wait 1.5 | window albumArtFullscreenWindow",
     "iTunes candidate preview in the fullscreen album art viewer"),
    # (A preview always shows its details, which hold Apply and Back to Results; the info toggle is off during
    # a preview by design, ViewerController.toggleAlbumArtInfoVisible. Its own control is the comparison
    # switch, Left/Right: the current cover against the candidate.)
    # (The probe's key events bypass Qt's shortcut map, so the switch runs what the Left/Right shortcuts run.)
    ("dialogs", "viewer", "itunes-preview-current-cover", "js viewerController.switchComparisonImage() | wait 0.6 | window albumArtFullscreenWindow",
     "iTunes candidate preview switched to the current cover for comparison (Left/Right)"),
    ("dialogs", "_", "_close", "window albumArtFullscreenWindow | esc | wait 1 | window  | esc", ""),

    # ------------------------------------------------------------ viewers
    ("viewers", "_", "_play", "queue | play Small_Hours 60 5 | away", ""),
] + [
    line for mode, mname in ((0, "windowed"), (1, "fullscreen")) for line in (
        [("viewers", "viewer", f"spectrogram-{mname}" + ("-title-strip" if mode else ""),
          f"js root.uiBridge.setViewerFullscreenMode({mode}) | js root.uiBridge.setShowFullscreenTitleStrip(true) | js viewerController.openSpectrogramViewer() | settle 1 | ready 3"
          + (" | window spectrogramFullscreenWindow" if mode else ""),
          f"Spectrogram viewer, {mname}, title strip on" if mode else
          "Spectrogram viewer, windowed (no title strip by design: the window title names it; controls on the corner plate)")]
        + [("viewers", "viewer", f"spectrogram-{mname}-hover",
            ("window spectrogramFullscreenWindow | " if mode else "") + "move xy:700,450 | wait 0.8",
            f"Spectrogram viewer, {mname}, pointer over the plot (controls shown)"),
           ("viewers", "viewer", f"spectrogram-{mname}-zoomed",
            ("window spectrogramFullscreenWindow | " if mode else "") + "wheel xy:700,450 120 4 | wait 1.2 | move xy:700,450 | wait 0.6",
            f"Spectrogram viewer, {mname}, zoomed 4 notches: zoom readout + Reset Zoom")]
        + ([("viewers", "viewer", "spectrogram-windowed-reset-hover", "move xy:700,450 | wait 0.5 | hover viewerResetButton 1.3",
             "Windowed viewer: Reset Zoom hovered (tooltip)"),
            ("viewers", "viewer", "spectrogram-windowed-meter",
             "js root.uiBridge.setShowLevelMeter(true) | wait 0.5 | move xy:700,450 | wait 0.6",
             "Windowed spectrogram viewer with the level meter on, pointer over the plot"),
            ("viewers", "_", "_edge", "away | wait 0.4 | macro edge4x viewer", ""),
            ("viewers", "_", "_windowed-meter-off", "js root.uiBridge.setShowLevelMeter(false) | wait 0.3", "")] if not mode else [])
        + [("viewers", "viewer", f"waveform-{mname}",
            "js resetZoomAction.trigger() | js viewerController.setVisualizationMode(1) | ready 2"
            + (" | window spectrogramFullscreenWindow" if mode else ""), f"Waveform viewer, {mname}"),
           ("viewers", "viewer" if mode else "_", f"spectrogram-{mname}-no-strip" if mode else "_spectrogram-windowed-reopen",
            "js viewerController.setVisualizationMode(0) | js viewerController.closeSpectrogramViewer() | settle 1 | js root.uiBridge.setShowFullscreenTitleStrip(false) | js viewerController.openSpectrogramViewer() | settle 1 | ready 3"
            + (" | window spectrogramFullscreenWindow" if mode else ""), f"Spectrogram viewer, {mname}, title strip off" if mode else "")]
        + ([("viewers", "viewer", "spectrogram-fullscreen-auto-hidden",
             "window spectrogramFullscreenWindow | js root.uiBridge.setShowFullscreenTitleStrip(true) | js viewerController.closeSpectrogramViewer() | settle 1 | js viewerController.openSpectrogramViewer() | settle 1 | ready 1 | move xy:700,450 | wait 6.5",
             "Fullscreen spectrogram viewer 6.5 s after the last pointer move (auto-hidden controls)")] if mode else [])
        + [("viewers", "viewer", f"albumart-{mname}",
            "js viewerController.closeSpectrogramViewer() | settle 1 | js viewerController.openAlbumArtViewer() | settle 2 | wait 0.4"
            + (" | window albumArtFullscreenWindow" if mode else ""), f"Album art viewer, {mname}"),
           ("viewers", "viewer", f"albumart-{mname}-info",
            ("" if mode else "move xy:800,500 | wait 0.3 | ")  # (the plate shown again; its 5 s hide restarted)
            + "js viewerController.toggleAlbumArtInfoVisible(function(){}) | wait 0.4" + (" | window albumArtFullscreenWindow" if mode else ""),
            f"Album art viewer, {mname}, file info pane"),
           ("viewers", "menu", f"context-albumart-viewer-{mname}",
            "js viewerController.toggleAlbumArtInfoVisible(function(){}) | " + ("window albumArtFullscreenWindow | " if mode else "") + ("key Menu | settle 0.8" if mode else "rclick xy:500,400"),
            f"Album art viewer context menu ({mname}" + (", Menu key)" if mode else ", right click; the Menu key and Shift+F10 open it too, at the art's centre)")),
           ("viewers", "_", f"_close-{mname}", ("window albumArtFullscreenWindow | " if mode else "") + "esc | js viewerController.closeAlbumArtViewer() | settle 1", "")]
    )
] + [
    ("viewers", "_", "_reset", "js root.uiBridge.setViewerFullscreenMode(1) | js root.uiBridge.setShowFullscreenTitleStrip(true)", ""),

    # ------------------------------------------------------------ focus walk
    # Launch default: the session's first state, before anything is clicked or typed
    ("focus", "focus", "launch-default", "wait 0.4 | macro launch_focus", "Right after launch: the default focus"),
    # the walk with the status bar's indicators shown (Last.fm listening, one new issue), so they are in it
    ("focus", "_", "_setup", "queue | play Small_Hours 30 2 | js root.uiBridge.pause() | "
     '''macro lastfm {"lastFmScrobbleListenedMs":60000} | '''
     "js errorController.report(1, 1, '/music/A/B/01 - Missing.flac', 'The file could not be opened') | away | unfocus", ""),
] + [
    ("focus", "focus", f"tab-{i:02d}", f"macro tab {i}", "Focus walk") for i in range(1, 25)
] + [
    # dialogs' focus stops and checked-state announcements (FOCUS-A11Y.md): Configure and About walked with
    # Tab; the mode switch's radio buttons and a Configure check box toggled with Space (events recorded)
    ("focus", "_", "_a11y-dialogs", f"unfocus | away | macro openprefs {PREF_PAGES[0][0]} | macro dialogwalk Configure 14 | "
     "macro opendialog aboutDialog | macro dialogwalk About 8", ""),
    ("focus", "_", "_a11y-checked", "unfocus | macro checkann mode-switch main visualizationModeButton1 visualizationModeButton0 | unfocus | "
     f"macro openprefs {PREF_PAGES[1][0]} | "  # (the Spectrogram page: VisualizationPage.qml, with the stretch check box)
     "macro checkann stretch-short-tracks dialog stretchShortTracksCheckBox | js preferencesDialog.close() | wait 0.5 | unfocus | away", ""),
] + [
    # from idle (its own session: a stopped track keeps its metadata, so idle comes only at a start), the first Play of a track without art: the stand-in grows into the cover slot
    ("firstplay-noart", "_", "_idle", "away | wait 0.5 | expect idle", ""),
    ("firstplay-noart", "motion", "first-play-no-art",
     "strip 1.8 0.1 - :: js root.uiBridge.replaceWithPaths([" + json.dumps(os.path.join(LIB, "Orchid Static", "2017 - Static Bloom",
                                                                                        os.path.basename(_NA_TRACK))) + "]) ; "
     "wait 0.3 ; js root.uiBridge.playAt(0)",
     "First Play from idle of a track without album art (Static Bloom): the stand-in cover comes into the slot as a cover does"),
    # the plates revealed by the pointer entering the pane; then Stop and Clear: back to idle
    ("firstplay-noart", "_", "_motif", "wait 1.2 | away | macro motif", ""),  # the icon's motif in the app
    ("firstplay-noart", "_", "_settle", "wait 1.5 | js root.uiBridge.setSpectrogramViewMode(1) | away | wait 1.6", ""),
    ("firstplay-noart", "motion", "plates-reveal", "strip 1.0 0.1 spectrogramPane :: move xy:900,700",
     "The pointer enters the pane: the mode switch and the lane heads' plates reveal"),
    ("firstplay-noart", "motion", "stop-to-idle",
     "away | wait 0.6 | strip 1.6 0.1 sidebarPane:0,0,1,0.62 :: js root.uiBridge.stop() ; wait 0.6 ; js root.uiBridge.clearQueue()",
     "Stop, then Clear the queue (0.6 s later): the now-playing card (what it ends on is measured from the recording)"),
    # the queue runs out on its own: one track, played into its last 2 s, then the card at the end of playback
    ("firstplay-noart", "_", "_end-queue", "js root.uiBridge.replaceWithPaths(" + json.dumps([_SHORE_TRACK]) + ") | "
     "until root.uiBridge.queueLength === 1 | js root.uiBridge.playAt(0) | "
     "until root.uiBridge.durationSeconds > 3 && root.uiBridge.currentTrackPath.indexOf('Shore') >= 0 | wait 0.8 | "
     "js root.uiBridge.seek(root.uiBridge.durationSeconds - 2.2) | wait 0.5 | away", ""),
    ("firstplay-noart", "motion", "queue-end-to-idle", "strip 3.4 0.1 sidebarPane:0,0,1,0.62 :: wait 0",
     "The queue runs out on its own: the last track plays to its end (about 1.7 s in), then the now-playing card"),
    # ------------------------------------------------------------ library density: 320 invented artists
    # the large library's scan recorded: the status bar's scan label arriving and leaving (the 180-track fixture
    # scans too fast for the label to show); then waiting for the scan as before
    ("density", "motion", "scan-large", f"away | strip 6.0 0.1 statusBar :: js root.uiBridge.addLibraryRoot({json.dumps(LIB_LARGE)}, 'Large Shelf')",
     "The large library's scan (1,628 tracks): the status bar's scan label arriving, counting and leaving"),
    ("density", "_", "_scan", "macro largelib wait | away", ""),
    ("density", "library", "density-scrolled",
     f"js {FIND}('libraryView').contentY = 2600 | wait 0.4 | click libraryRow#3@0.3,0.5 | key Right | wait 0.4 | "
     "key Down | key Down | key Right | wait 0.4 | key Down | key Down | key Down | key Down | key Down | key Right | wait 0.5 | away",
     "Library density: 320 invented artists (1,628 tracks), scrolled about a hundred rows down, two artists and an album expanded"),
    # the focused row's ring beside the library's scrollbar lane (1:1 and 4x; the ring must stop at the lane)
    ("density", "_", "_ring-lane", "macro rowdetail ring", ""),
    ("density", "main", "library-density", "away | wait 0.3",
     "Main window with the large library (320 artists), scrolled and partly expanded"),
    # ------------------------------------------------------------ library roots
    ("roots", "library", "two-roots", f"js root.uiBridge.addLibraryRoot('{LIB2}', 'Second Shelf') | until (root.uiBridge.libraryRootEntries || []).length === 2 && !root.uiBridge.libraryScanInProgress | wait 0.6 | away",
     "Library with two folders: root header rows"),
    ("roots", "library", "two-roots-hover", f"hover {LRow}@0.3,0.5", "Library: pointer over a root row"),
    ("roots", "menu", "context-library-root", f"click {LRow}@0.3,0.5 | away | key Menu | settle 0.8", "Library context menu on a root row (Menu key)"),
    ("roots", "dialog", "preferences-library-two-roots", "esc | macro openprefs Library | settle 1.2",
     "Preferences: Library page with two folders"),
    ("roots", "dialog", "preferences-library-root-selected", "click libraryRootRow#1@0.5,0.5 | wait 0.4",
     "Preferences: a library folder selected (Rename/Rescan/Remove enabled)"),
    # a third folder, and the pointer over its row
    ("roots", "_", "_third-root", "esc | js root.uiBridge.addLibraryRoot(" + json.dumps(os.path.join(os.path.dirname(LIB), "standin"))
     + ", 'Stand-ins') | until (root.uiBridge.libraryRootEntries || []).length === 3 && !root.uiBridge.libraryScanInProgress | "
     "wait 0.6 | macro openprefs Library | settle 1.2", ""),
    ("roots", "dialog", "preferences-library-three-roots-hover", "hover libraryRootRow#2@0.5,0.5 0.6",
     "Configure, Library: three folders, the pointer over the third"),
    ("roots", "_", "_close", "esc", ""),

    # ------------------------------------------------------------ motion strips
    # From idle (an empty queue, nothing loaded: the session's start), the first Play: File > Play Whole Library.
    ("motion", "_", "_idle", "away | wait 0.5 | shown nowPlayingIdleRow", ""),
    ("motion", "motion", "first-play-from-idle", "strip 1.8 0.1 - :: js playAllLibraryTracksAction.trigger()",
     "First Play from idle: the compact \"Nothing playing\" row, the queue empty, then File > Play Whole Library: the cover comes in "
     "(before it, the plot pane is at rest: its rest lines show without the scale)"),
    ("motion", "_", "_play", "queue | play Small_Hours 60 4 | away", ""),
    ("motion", "motion", "track-change-same-album", "strip 0.8 0.1 nowPlayingPanel+playlistView:0,0,0.45,0.2 :: js root.uiBridge.next()",
     "Track change within an album: now-playing fade of changed fields, the playing-row move in the queue"),
    ("motion", "motion", "track-change-new-album", "play Moth_Copper - 2 | strip 1.0 0.1 nowPlayingPanel+playlistView:0,0,0.45,0.2 :: js root.uiBridge.previous()",
     "Track change to another album (cover changes): cover and text cross-fade, playing-row move"),
    # a track without album art (the compact now playing) and back, in the now-playing column
    ("motion", "_", "_artqueue", "macro artqueue | away", ""),
    ("motion", "motion", "art-to-no-art", "strip 1.2 0.1 sidebarPane:0,0,1,0.72 :: js root.uiBridge.next()",
     "Track change from a track with album art to one without (Static Bloom): the now-playing column"),
    ("motion", "_", "_art-check", "until root.uiBridge.currentTrackCoverPath === '' | wait 0.8", ""),
    ("motion", "motion", "no-art-to-art", "strip 1.2 0.1 sidebarPane:0,0,1,0.72 :: js root.uiBridge.previous()",
     "Track change from a track without album art back to one with it: the now-playing column"),
    ("motion", "_", "_art-restore", "until root.uiBridge.currentTrackCoverPath !== '' | queue | play Moth_Copper - 2 | "
     "js root.uiBridge.previous() | wait 1.2 | away", ""),
    ("motion", "motion", "play-pause-glyph", "strip 0.5 0.1 0,26,420,50 :: js root.uiBridge.pause()", "Play/Pause glyph cross-fade on pause"),
    ("motion", "motion", "play-pause-glyph-resume", "strip 0.5 0.1 0,26,420,50 :: js root.uiBridge.play()", "Play/Pause glyph on resume"),
    ("motion", "motion", "seek-tooltip", "strip 1.6 0.1 0,0,1600,110 :: hover seekSlider@0.62,0.5 0.7 ; move xy:900,500",
     "Seek bar hover: hover line + time box appearing, then fading on leave"),
    ("motion", "motion", "spectrogram-zoom", "strip 2.0 0.1 spectrogramPane :: wheel spectrogramPaneItem@0.5,0.4 120 3 ; wait 0.8 ; js resetZoomAction.trigger()",
     "Spectrogram zoom in (3 notches) and Reset Zoom: rescale and backdrop layers"),
    ("motion", "motion", "visualization-mode-switch", "away | strip 1.2 0.1 spectrogramPane :: js viewerController.setVisualizationMode(1)",
     "Visualization mode switch, spectrogram to waveform"),
    ("motion", "motion", "visualization-mode-switch-back", "strip 1.2 0.1 spectrogramPane :: js viewerController.setVisualizationMode(0)",
     "Visualization mode switch, waveform to spectrogram"),
    ("motion", "motion", "rolling-scroll", "js root.uiBridge.setSpectrogramDisplayMode(0) | wait 1.2 | strip 0.5 0.0 spectrogramPane :: wait 0",
     "Rolling spectrogram scrolling (0.5 s of frames)"),
    ("motion", "motion", "centered-scroll", "js root.uiBridge.setSpectrogramDisplayMode(1) | wait 1.2 | strip 0.5 0.0 spectrogramPane :: wait 0",
     "Centered spectrogram scrolling (0.5 s of frames)"),
    ("motion", "motion", "analysing", "strip 1.5 0.1 spectrogramPane :: start Dust_Motes",
     "Play a new 192 kHz track: the pane while analysis starts"),
    # Every frame of short windows (see STRIP_MODE), tight crops.
    ("motion", "_", "_play2", "js root.uiBridge.play() | wait 0.4 | away", ""),
    # the queue's "now" marker moving on Next; then back
    ("motion", "motion", "queue-now-move", "macro queuenow top | wait 0.3 | strip 1.0 0.1 mainQueuePane:0,0,1,0.45 :: js root.uiBridge.next()",
     "Next: the queue's playing (\"now\") row moving down one row"),
    ("motion", "_", "_now-back", "macro queuenow after | js root.uiBridge.previous() | wait 0.8 | away", ""),
    # the first switch to the waveform after a track starts waits for its detail; later ones do not.
    # Recorded both: the first (reference, its facts in the focus pack), then back, then the later one (the focus image)
    ("motion", "motion", "mode-switch-lane-first",
     "hover spectrogramPaneItem@0.6,0.25 0.4 | hover visualizationModeButton1 0.4 | "
     "strip 0.6 0.1 spectrogramPane:0.5,0,0.5,0.5 :: click visualizationModeButton1",
     "FIRST switch to the waveform since the track started (Dust Motes), clicked: the switch beside one lane"),
    ("motion", "_", "_lane-back-warm", "click visualizationModeButton0 | ready 2 | wait 0.5", ""),
    # (the switch clicked for real, the pointer on it, and in the crop beside the lane: the control's response apart
    # from the data's readiness)
    ("motion", "motion", "mode-switch-lane",
     "hover spectrogramPaneItem@0.6,0.25 0.4 | hover visualizationModeButton1 0.4 | "
     "strip 0.6 0.1 spectrogramPane:0.5,0,0.5,0.5 :: click visualizationModeButton1",
     "Visualization mode switch to waveform, clicked, a LATER switch on this track (the first is mode-switch-lane-first): "
     "the switch (top right) beside one lane, every frame 0-400 ms"),
    ("motion", "motion", "mode-switch-lane-back",
     "hover visualizationModeButton0 0.4 | strip 0.6 0.1 spectrogramPane:0.5,0,0.5,0.5 :: click visualizationModeButton0",
     "Visualization mode switch back to spectrogram, clicked: the switch (top right) beside one lane, every frame 0-400 ms"),
    # rolling mode, waveform to spectrogram: the pane's left edge after the switch (~30 px of black was seen there once)
    # Rolling for 4 s (its history drawn), then the waveform for 2 s, then the spectrogram clicked: the probe measures
    # the black columns at the pane's left before the switch and after it (rolling_<variant>.txt), with the
    # playback position (black before the track's start is expected; black over played time is not).
    ("motion", "_", "_rolling-wave", "js root.uiBridge.setSpectrogramDisplayMode(0) | wait 4 | "
     "macro rollprobe rolling-4s | js viewerController.setVisualizationMode(1) | wait 2 | hover visualizationModeButton0 0.4", ""),
    ("motion", "motion", "rolling-after-mode-switch",
     "macro rollprobe before-switch | strip 1.5 0.1 spectrogramPane :: click visualizationModeButton0",
     "Rolling spectrogram: waveform to spectrogram (clicked), the whole pane for 1.5 s, its left edge after the switch "
     "(black left of the plot at a position shorter than the pane's span is the time before the track's start)"),
    ("motion", "_", "_rolling-after", "macro rollprobe after-switch-1.5s | wait 3 | macro rollprobe after-switch-4.5s", ""),
    ("motion", "_", "_rolling-restore", "js root.uiBridge.setSpectrogramDisplayMode(1) | away | wait 0.5", ""),
    ("motion", "motion", "zoom-notch-lane", "ready 2 | strip 0.6 0.1 spectrogramPane:0,0,1,0.5 :: wheel spectrogramPaneItem@0.5,0.25 120 1",
     "Spectrogram zoom, one wheel notch, one lane, every frame 0-400 ms"),
    ("motion", "motion", "zoom-reset-lane", "wait 0.6 | strip 0.6 0.1 spectrogramPane:0,0,1,0.5 :: js resetZoomAction.trigger()",
     "Reset Zoom, one lane, every frame 0-400 ms"),
    ("motion", "_", "_pause2", "js root.uiBridge.pause() | wait 0.4 | away", ""),
    ("motion", "motion", "play-pause-glyph-2x", "strip 0.5 0.1 transportButton_previous+transportButton_next :: js root.uiBridge.play()",
     "Play/Pause glyph cross-fade (paused to playing), 2x, every frame"),
    ("motion", "motion", "play-pause-glyph-2x-pause", "strip 0.5 0.1 transportButton_previous+transportButton_next :: js root.uiBridge.pause()",
     "Play/Pause glyph cross-fade (playing to paused), 2x, every frame"),
    ("motion", "motion", "hover-fade-queue-row", "away | strip 0.9 0.1 @plainQueueRow:0,-1,0.6,3 :: hover @plainQueueRow@0.3,0.5 0.35 ; move xy:1598,450",
     "Queue row hover tint fading in, then out, on a row that is neither selected nor playing, every frame"),
    ("motion", "motion", "hover-fade-transport-button",
     "strip 0.9 0.1 transportButton_previous+transportButton_next :: hover transportButton_stop 0.35 ; move xy:1598,450",
     "Transport button hover fading in, then out, 2x, every frame (before the 700 ms tooltip delay)"),
    ("motion", "motion", "hover-fade-plot-tool-button",
     "hover spectrogramPaneItem@0.5,0.5 0.5 | strip 0.9 0.1 visualizationModeSwitch :: hover visualizationModeButton1 0.35 ; move spectrogramPaneItem@0.5,0.5",
     "Plot tool button (Waveform) hover fading in, then out, 3x, every frame"),
    ("motion", "motion", "viewer-autohide-fullscreen",
     "move xy:1598,450 | js root.uiBridge.setViewerFullscreenMode(1) | js root.uiBridge.setShowFullscreenTitleStrip(true) | "
     "js viewerController.openSpectrogramViewer() | wait 1.5 | window spectrogramFullscreenWindow | "
     "strip 6.6 0.1 - win=spectrogramFullscreenWindow :: move xy:1000,700 ; wait 0.1 ; move xy:1010,705",
     "Fullscreen viewer (paused): pointer moves, then after 5 s the title strip fades and the corner controls, "
     "cursor and crosshair hide (only the viewer window recorded)"),
    # the strip showing again, the pointer moved after it hid (its show; the hide is viewer-autohide-fullscreen)
    ("motion", "motion", "viewer-strip-show-fullscreen",
     "window spectrogramFullscreenWindow | strip 1.2 0.1 - win=spectrogramFullscreenWindow :: move xy:1000,700 ; wait 0.05 ; move xy:1010,705",
     "Fullscreen viewer: the pointer moves after the strip hid; the strip and corner controls show again"),
    ("motion", "_", "_close-viewer", "window spectrogramFullscreenWindow | esc | window  | js viewerController.closeSpectrogramViewer() | settle 1", ""),
    ("motion", "motion", "viewer-open-windowed",
     "js root.uiBridge.setViewerFullscreenMode(0) | strip 1.0 0.1 - :: js viewerController.openSpectrogramViewer()",
     "Windowed spectrogram viewer opening (a modal popup inside the main window, not a separate window)"),
    ("motion", "_", "_viewer-inwindow", "settle 1 | macro inwindow", ""),
    ("motion", "motion", "viewer-close-windowed", "strip 0.8 0.1 - :: js viewerController.closeSpectrogramViewer()",
     "Windowed spectrogram viewer closing (a modal popup inside the main window, not a separate window)"),
    ("motion", "motion", "albumart-open-windowed", "settle 1 | away | strip 1.0 0.1 - :: js viewerController.openAlbumArtViewer()",
     "Windowed album art viewer opening (a modal popup inside the main window, not a separate window)"),
    ("motion", "_", "_albumart-inwindow", "settle 1 | macro inwindow", ""),
    ("motion", "motion", "albumart-close-windowed", "strip 0.8 0.1 - :: js viewerController.closeAlbumArtViewer()",
     "Windowed album art viewer closing (a modal popup inside the main window, not a separate window)"),
    ("motion", "motion", "albumart-autohide-fullscreen",
     "settle 1 | move xy:1598,450 | js root.uiBridge.setViewerFullscreenMode(1) | js root.uiBridge.setShowFullscreenTitleStrip(true) | "
     "js viewerController.openAlbumArtViewer() | wait 1.5 | window albumArtFullscreenWindow | "
     "strip 6.6 0.1 - win=albumArtFullscreenWindow :: move xy:1000,700 ; wait 0.1 ; move xy:1010,705",
     "Fullscreen album art viewer: pointer moves, then after 5 s its strip hides over the art (only the viewer window recorded)"),
    ("motion", "_", "_close-albumart", "window albumArtFullscreenWindow | esc | window  | js viewerController.closeAlbumArtViewer() | "
     "js root.uiBridge.setViewerFullscreenMode(0) | settle 1", ""),

    ("motion-ui", "_", "_play", "queue | play Small_Hours 60 4 | js root.uiBridge.pause() | wait 0.4 | away", ""),
    # Input-latency baseline: a plain rectangle the harness adds (not the app's), recoloured on press with no
    # animation, at the window's top-right corner. Its latency is the harness's own (input to rendered frame),
    # to subtract from the app's. Removed afterwards.
    ("motion-ui", "_", "_latency-probe-add", "js " + LATENCY_PROBE_ADD + " | wait 0.3", ""),
] + [
    ("motion-ui", "motion", f"latency-baseline-click-{i}", f"strip 0.5 0.1 {LATENCY_PROBE_RECT} :: click latencyBaseline",
     f"Input-latency baseline {i}/3: a harness rectangle recoloured on press, no animation (a real click)")
    for i in (1, 2, 3)
] + [
    ("motion-ui", "motion", "latency-baseline-script", f"strip 0.5 0.1 {LATENCY_PROBE_RECT} :: js " + LATENCY_PROBE_FLIP,
     "Input-latency baseline: the same rectangle recoloured by script (the probe's eval)"),
    ("motion-ui", "_", "_latency-probe-remove", "js " + LATENCY_PROBE_REMOVE, ""),
    ("motion-ui", "motion", "transport-press",
     "away | strip 0.9 0.1 transportButton_previous+transportButton_next :: press transportButton_stop ; wait 0.45 ; "
     "dmove xy:900,500 ; release xy:900,500",
     "Transport button pressed and held (released off it): every frame of the press, 2x"),
    ("motion-ui", "motion", "transport-tooltip", "strip 1.6 0.1 0,26,420,70 :: hover transportButton_next 1.2 ; move xy:900,500",
     "Transport button: hover, tooltip after the delay, fade on leave"),
    ("motion-ui", "motion", "queue-row-hover", f"strip 0.6 0.1 playlistView:0,0,1,0.3 :: hover {QRow}@0.3,2.5 0.3 ; move xy:1598,450",
     "Queue row hover tint in and out"),
    ("motion-ui", "motion", "plot-plates-hover", "strip 1.0 0.1 spectrogramPane :: hover spectrogramPaneItem@0.6,0.4 0.5 ; move xy:1598,450",
     "Plot plates (mode switch, lane heads) on pane hover in and out"),
    ("motion-ui", "motion", "library-expand", f"js libraryController.collapseAll() | wait 0.3 | click {LRow}@0.3,1.5 | strip 0.6 0.1 libraryPane:0,0,1,0.45 :: key Right",
     "Library tree expand (Right on an artist)"),
    ("motion-ui", "motion", "library-collapse", "strip 0.6 0.1 libraryPane:0,0,1,0.45 :: key Left", "Library tree collapse"),
    ("motion-ui", "motion", "menu-open-close", "away | strip 0.8 0.1 0,0,520,320 :: menu 0 ; wait 0.3 ; esc",
     "File menu open and close"),
    ("motion-ui", "motion", "dialog-open-close", "esc | away | strip 1.2 0.1 - dialog=aboutDialog :: macro opendialog aboutDialog ; wait 0.6 ; esc",
     "About opening and closing: a window of its own (its first frame is drawn whole; KWin's window animation is not recorded)"),
    ("motion-ui", "motion", "preferences-open", "strip 0.7 0.1 - dialog=Configure :: js preferencesDialog.openPage(0)", "Preferences open"),
    ("motion-ui", "motion", "search-open", "esc | away | strip 0.8 0.1 - :: js globalSearchController.openDialog()",
     "Search palette opening"),
    ("motion-ui", "motion", "search-close", "settle 0.6 | strip 0.8 0.1 - :: esc", "Search palette closing"),
    ("motion-ui", "motion", "search-typing", "js globalSearchController.openDialog() | settle 0.6 | strip 1.2 0.1 - :: type mer",
     "Search palette results appearing while typing"),
    ("motion-ui", "motion", "issue-indicator-appears",
     "esc | strip 0.6 0.1 statusBar :: js errorController.report(1, 1, '/music/A/B/01 - X.flac', 'The file could not be opened')",
     "Status bar issues indicator appearing"),
    ("motion-ui", "motion", "scrobble-ring-fill",
     'strip 3.4 0.1 statusBar:0.8,0,0.2,1 :: macro lastfm {"lastFmScrobbleListenedMs":0} ; wait 0.4 ; '
     'macro lastfm {"lastFmScrobbleListenedMs":60000} ; wait 0.4 ; macro lastfm {"lastFmScrobbleListenedMs":110000} ; '
     'wait 0.4 ; macro lastfm {"lastFmScrobbleListenedMs":119000} ; wait 0.4 ; macro lastfm {"lastFmScrobbleState":3} ; '
     'wait 1.2 ; repaint',
     "Scrobble indicator: 0 %, mid-way, nearly done, then scrobbled, held 1.2 s (a frame is forced at +1.2 s; states injected)"),

    # ------------------------------------------------------------ window size and scaling
    ("narrow-empty", "main", "first-run-empty-library", "wait 0.5", "Narrow window: first run"),
    ("narrow", "main", "library-idle", "away", "Narrow window: library idle"),
    ("narrow", "nowplaying", "idle-row", "away | shown nowPlayingIdleRow", "Now playing idle, narrow window, 10 pt"),
    ("narrow", "main", "playing-spectrogram-centered", "queue | play Small_Hours 70 6 | away", "Narrow window: playing, centered spectrogram"),
    ("narrow", "transport", "at-rest", "away", "Narrow window: transport playing"),
    ("narrow", "seek", "hover-start", "unfocus | hover seekSlider@0.022,0.5 0.8",
     "Narrow window: the seek bar hovered near its start (about 0:05): the time box"),
    ("narrow", "statusbar", "playing", "away", "Narrow window: status bar playing"),
    ("narrow", "queue", "at-rest", "away | macro queuegeom rest-narrow", "Narrow window: queue at rest (playing row)"),
    ("narrow", "nowplaying", "at-rest", "away", "Narrow window (1000x720): now playing"),
    # the narrow card without album art, playing, then the session's track again
    ("narrow", "nowplaying", "no-cover", "play Static_Bloom - 3 | until root.uiBridge.currentTrackCoverPath === '' | away | wait 0.5",
     "Narrow window (1000x720), default font: a track without album art (Static Bloom)"),
    ("narrow", "main", "playing-no-cover", "away", "Narrow window (1000x720), default font: playing a track without art"),
    ("narrow", "_", "_no-cover-back", "play Small_Hours 70 6 | away", ""),
    ("narrow", "main", "playing-waveform", "js viewerController.setVisualizationMode(1) | ready 3", "Narrow window: waveform"),
    ("narrow", "main", "paused", "js viewerController.setVisualizationMode(0) | js root.uiBridge.pause() | wait 0.4", "Narrow window: paused"),
    ("narrow", "search", "results", "js globalSearchController.openDialog() | settle 0.8 | type mer | wait 0.8", "Narrow window: search palette"),
    ("narrow", "dialog", f"preferences-{PREF_PAGES[1][1]}", f"esc | macro openprefs {PREF_PAGES[1][0]} | settle 1.1",
     f"Narrow window: Preferences, {PREF_PAGES[1][0]} page"),
    ("narrow", "dialog", "tag-editor", "esc | macro tagedit 4", "Narrow window: Edit Tags"),
    ("narrow", "menu", "view", "js tagEditorDialog.closeEditor() | wait 0.5 | menu 2", "Narrow window: View menu"),
    ("narrow", "_", "_close", "esc", ""),
    # art to none and back in the narrow window (1000x720): the slot cross-fades, nothing else moves
    ("narrow", "_", "_artqueue", "js libraryController.collapseAll() | macro artqueue | away", ""),
    ("narrow", "motion", "art-to-no-art-narrow", "strip 1.2 0.1 sidebarPane:0,0,1,0.9 :: js root.uiBridge.next()",
     "Narrow window (1000x720): track change from a track with album art to one without: the left column"),
    ("narrow", "_", "_art-check", "until root.uiBridge.currentTrackCoverPath === '' | wait 0.8", ""),
    ("narrow", "motion", "no-art-to-art-narrow", "strip 1.2 0.1 sidebarPane:0,0,1,0.9 :: js root.uiBridge.previous()",
     "Narrow window (1000x720): track change from a track without album art back to one with it: the left column"),
    ("hidpi", "main", "playing-spectrogram-centered", "queue | play Small_Hours 70 7 | away", "Scale 2: playing, centered spectrogram"),
    ("hidpi", "dialog", f"preferences-{PREF_PAGES[1][1]}", f"macro openprefs {PREF_PAGES[1][0]} | settle 1.3",
     f"Scale 2: Preferences, {PREF_PAGES[1][0]} page"),
    ("hidpi", "search", "results", "esc | js globalSearchController.openDialog() | settle 0.8 | type mer | wait 0.8", "Scale 2: search palette"),
    ("hidpi", "menu", "playback", "esc | menu 3", "Scale 2: Playback menu"),
    ("hidpi", "_", "_close", "esc", ""),
    ("bigfont", "main", "library-idle", "away", "Large font (Noto Sans 24 pt): library idle"),
    ("bigfont", "nowplaying", "idle-row", "away | shown nowPlayingIdleRow", "Now playing idle at 24 pt, 1600x1000"),
    ("bigfont", "main", "playing-spectrogram-centered", "queue | play Small_Hours 70 6 | away",
     "Large font: playing (1600x1000: the compact now playing, where the library would be short)"),
    ("bigfont", "transport", "at-rest", "away", "Large font: transport playing"),
    ("bigfont", "seek", "hover-start", "unfocus | hover seekSlider@0.022,0.5 0.8",
     "Large font: the seek bar hovered near its start (about 0:05): the time box"),
    ("bigfont", "queue", "at-rest", "away | macro queuegeom rest-24pt", "Large font: queue at rest (playing row)"),
    ("bigfont", "_", "_queue24", "macro crop11 queue24 font24 mainQueuePane", ""),
    # the lane heads at 24 pt (per channel), 2x, with the letters' chroma
    ("bigfont", "_", "_lanes24", "macro floorprobe save | js root.uiBridge.setSpectrogramViewMode(1) | wait 0.6 | unfocus | "
     "macro switchstates lanes | macro floorprobe restore | away", ""),
    ("bigfont", "nowplaying", "at-rest", "away", "Large font, 1600x1000: the compact now playing with album art"),
    # the compact now playing without album art (its stand-in), then the session's track again
    ("bigfont", "nowplaying", "no-cover", "play Static_Bloom - 3 | until root.uiBridge.currentTrackCoverPath === '' | away | wait 0.5 | "
     "macro cardgeom no-cover",
     "Large font, 1600x1000 (the compact now playing): a track from an album without cover art (Static Bloom)"),
    ("bigfont", "_", "_compact-mark", "macro markprobe nowPlayingCover", ""),  # the mark at the compact size
    ("bigfont", "_", "_no-cover-back", "play Small_Hours 70 6 | away", ""),
] + _standin_lines("bigfont", "Compact now playing at 24 pt (1600x1000)", "queue | play Small_Hours 70 6 | away") + \
    _title_lines("bigfont", "Now playing at 24 pt, 1600x1000", "queue | play Small_Hours 70 6 | away") + [
    # the metadata track change in the 24 pt compact card (the same three tracks as track-change-metadata)
    ("bigfont", "_", "_meta-queue24", "js root.uiBridge.replaceWithPaths(" + json.dumps([SI_FILES[3][1], _NORMAL_TRACK, SI_FILES[0][1]])
     + ") | until root.uiBridge.queueLength === 3 | js root.uiBridge.playAt(0) | "
     "until root.uiBridge.currentTrackPath.indexOf('track07') >= 0 | wait 1.5 | away", ""),
    ("bigfont", "motion", "track-change-metadata-24pt",
     "strip 2.8 0.1 sidebarPane:0,0,1,0.62 :: js root.uiBridge.next() ; wait 1.4 ; js root.uiBridge.next()",
     "Track changes across metadata cards at 24 pt (the compact card): untagged, then a tagged track with art, then a "
     "long album without art: the card and the library header"),
    ("bigfont", "_", "_meta-close24", "js root.uiBridge.stop() | queue | play Small_Hours 70 6 | away", ""),
    ("bigfont", "menu", "playback", "menu 3", "Large font: Playback menu"),
    ("bigfont", "dialog", f"preferences-{PREF_PAGES[1][1]}-first", f"esc | macro openprefs {PREF_PAGES[1][0]} | settle 1.2",
     f"Large font: Preferences, {PREF_PAGES[1][0]} (opened first)"),
    ("bigfont", "dialog", "about", "esc | macro opendialog aboutDialog | settle 1", "Large font: About"),
    ("bigfont", "dialog", "tag-editor", "esc | macro tagedit 4", "Large font: Edit Tags"),
    ("bigfont", "search", "results", "js tagEditorDialog.closeEditor() | wait 0.5 | js globalSearchController.openDialog() | settle 0.8 | type mer | wait 0.8",
     "Large font: search palette"),
    ("bigfont", "_", "_searchfield", "macro searchfield 24pt", ""),
    ("bigfont", "_", "_close", "esc", ""),

    # ------------------------------------------------------------ animations off (AnimationDurationFactor=0)
    ("animoff", "_", "_play", "queue | play Small_Hours 60 4 | away", ""),
    ("animoff", "motion", "animoff-track-change", "strip 0.6 0.1 nowPlayingPanel+playlistView:0,0,0.45,0.2 :: js root.uiBridge.next()",
     "Animations off: track change (now playing, playing row)"),
    ("animoff", "animoff", "track-change-after", "wait 0.2", "Animations off: right after a track change"),
    ("animoff", "motion", "animoff-play-pause-glyph", "strip 0.4 0.1 transportButton_previous+transportButton_next :: js root.uiBridge.pause()",
     "Animations off: Play/Pause glyph, 2x"),
    ("animoff", "motion", "animoff-dialog-open", "away | strip 0.6 0.1 - dialog=aboutDialog :: macro opendialog aboutDialog",
     "Animations off: About opening (dim + dialog)"),
    ("animoff", "animoff", "dialog-open", "wait 0.1", "Animations off: About open"),
    ("animoff", "motion", "animoff-search-open", "esc | wait 0.3 | strip 0.6 0.1 - :: js globalSearchController.openDialog()",
     "Animations off: search palette opening"),
    ("animoff", "animoff", "search-open", "wait 0.1", "Animations off: search palette open"),
    ("animoff", "animoff", "scan-count-only",
     'esc | wait 0.3 | macro fakebridge statusBar {"libraryScanInProgress":true,"libraryScanProcessed":57,"libraryScanTotal":0} | wait 0.2',
     "Animations off: library scan with an unknown total (the count only, no bar; injected)"),

    # ------------------------------------------------------------ large font per area (Noto Sans 24 pt)
    ("bigfont", "transport", "next-hover-tooltip", "esc | away | hover transportButton_next 1.4", "Large font: transport, Next hovered (tooltip)"),
    ("bigfont", "seek", "hover", "hover seekSlider@0.62,0.5 0.8", "Large font: seek bar hover time box"),
    ("bigfont", "volume", "slider-hover", "hover volumeSlider 1.4", "Large font: volume slider hovered (tooltip)"),
    ("bigfont", "statusbar", "playing", "away", "Large font: status bar while playing"),
    ("bigfont", "statusbar", "lastfm-listening-half", 'macro lastfm {"lastFmScrobbleListenedMs":60000} | hover scrobbleIndicator 1.4',
     "Large font: Last.fm indicator hovered (injected)"),
    ("bigfont", "statusbar", "issues-new",
     "away | macro lastfm {\"lastFmShowScrobbleProgress\":false} | js errorController.report(1, 1, '/music/A/B/01 - Missing.flac', 'The file could not be opened') | wait 0.3",
     "Large font: status bar with a new issue"),
    ("bigfont", "statusbar", "scanning-known",
     'macro fakebridge statusBar {"libraryScanInProgress":true,"libraryScanProcessed":57,"libraryScanTotal":180}',
     "Large font: library scan in progress (injected)"),
    ("bigfont", "_", "_bridge-reset", "macro fakebridge statusBar reset", ""),
    # (the palette reopens with its last query: cleared, so the state is empty as its caption says)
    ("bigfont", "search", "empty", "js globalSearchController.openDialog() | settle 0.8 | "
     "js globalSearchController.queryField.text = '' | wait 0.6 | until globalSearchController.queryField.text === ''",
     "Large font: search palette, empty"),
] + [
    ("bigfont", "dialog", f"preferences-{slug}", f"esc | macro openprefs {title} | settle 1.1 | away", f"Large font: Configure, {title} page")
    for title, slug in PREF_PAGES
] + [
    ("bigfont", "dialog", "itunes-empty", "esc | macro itunes empty | js root.openItunesArtworkDialog() | settle 1 | away",
     "Large font: Replace from iTunes, no results (stand-in)"),
    ("bigfont", "dialog", "itunes-results", "macro itunes results | wait 0.3", "Large font: Replace from iTunes, results (stand-ins)"),
    # the first row's high-resolution artwork loading: its details line reads "Loading high-resolution artwork…"
    ("bigfont", "dialog", "itunes-results-loading", "macro itunes loading | wait 0.4",
     "Large font: Replace from iTunes, results, the first row loading its high-resolution artwork (stand-ins)"),
    ("bigfont", "_", "_itunes-close", "esc | js itunesArtworkDialog.uiBridge = root.uiBridge", ""),
    # every dialog at 24 pt for LAYOUT.md's label-vs-action font check
    ("bigfont", "dialog", "library-root-name-rename", f"esc 2 | js root.openLibraryRootNameDialog('rename', '{LIB}', 'Fixtures') | settle 0.9",
     "Large font: library folder name dialog (rename)"),
    ("bigfont", "dialog", "diagnostics", "esc | macro opendialog diagnosticsDialog | settle 1.2 | away", "Large font: Diagnostics"),
    ("bigfont", "dialog", "confirm-clear-diagnostics", "macro open_child diagnosticsDialog ConfirmDialog",
     "Large font: confirmation to clear the diagnostics log"),
    ("bigfont", "dialog", "quit-save-failed",
     "esc 2 | js root.showQuitSaveFailedDialog('Settings could not be saved: Permission denied (os error 13)') | settle 0.9",
     "Large font: quit, saving the state failed"),
    ("bigfont", "dialog", "recent-errors", "esc | macro opendialog recentErrorsDialog | settle 1", "Large font: Recent Issues"),
    ("bigfont", "dialog", "tag-editor-auto-number", "esc | macro tagedit 4 | macro open_child tagEditorDialog AutoNumberDialog",
     "Large font: Edit Tags > Auto Number"),
    # the three-button confirmation (Unsaved Changes) at 24 pt
    ("bigfont", "dialog", "tag-editor-close-confirm", "esc | js root.tagEditorApi.setCell(1, 'title', 'Morning Tide Field (Live)') | "
     "wait 0.4 | js tagEditorDialog.requestClose() | waitpopup TagEditorCloseConfirmDialog 0.8",
     "Large font: Edit Tags closed with an unsaved change: Unsaved Changes (three buttons)"),
    ("bigfont", "_", "_dialogs24-close", "esc | js root.tagEditorApi.revertField('title') | js tagEditorDialog.closeEditor() | wait 0.5", ""),
    # every dialog at 24 pt (the ones not shot above)
    ("bigfont", "dialog", "about-kde", "esc | macro opendialog aboutKdeDialog | settle 1", "Large font: About KDE"),
    ("bigfont", "dialog", "library-root-name-add", f"esc | js root.openLibraryRootNameDialog('add', '{LIB2}', '') | settle 0.9",
     "Large font: Add Library Folder"),
    ("bigfont", "dialog", "confirm-remove-library-root",
     "esc 2 | macro openprefs Library | settle 1.2 | click libraryRootRow@0.5,0.5 | click libraryRootRemoveButton | "
     "waitpopup ConfirmDialog 1.5", "Large font: Remove Library Folder over Configure"),
    # the batch's Edit Tags first: twelve rows selected, Track and Disc mixed (their "Multiple values" placeholders)
    ("bigfont", "dialog", "tag-editor-batch", "esc 2 | macro renamebatch | wait 0.4 | away",
     "Large font: Edit Tags, the twelve-file batch, all selected (Track and Disc mixed: their placeholders)"),
    ("bigfont", "dialog", "tag-editor-rename-preview",
     "js tagEditorDialog.triggerRename() | waitpopup RenamePreviewDialog 2",
     "Large font: Edit Tags > Rename Files preview, the twelve-file batch"),
    # the four album tracks again: after the batch, quitwait would name two of its long file names;
    # one esc (the preview): a second would ask about the batch's unsaved edits
    ("bigfont", "dialog", "tag-editor-quit-wait", "esc | macro tagedit 4 | macro quitwait", "Large font: Finishing Tag Changes, two files"),
    ("bigfont", "dialog", "tag-editor-status-details", "esc | macro tagsavefail | macro open_child tagEditorDialog TagEditorStatusDetailsDialog",
     "Large font: Tag Editor Details after a real save with one folder not writable"),
    ("bigfont", "_", "_dialogs24b-close", "esc | js tagEditorDialog.closeEditor() | wait 0.5", ""),
    ("bigfont", "dialog", "itunes-error", "js root.openItunesArtworkDialog() | settle 1 | macro itunesfail",
     "Large font: Replace from iTunes, a real failed search"),
    ("bigfont", "dialog", "confirm-replace-cover", "macro itunes results | wait 0.4 | macro itunesapply 1",
     "Large font: Replace Album Art (from Apply)"),
    ("bigfont", "_", "_dialogs24c-close", "esc 2 | js itunesArtworkDialog.uiBridge = root.uiBridge", ""),
    ("bigfont", "search", "no-results", "js globalSearchController.openDialog() | settle 0.8 | type zzqx | wait 0.8",
     "Large font: search palette, no results"),
    ("bigfont", "_", "_search24-close", "esc", ""),
] + [
    ("bigfont", "menu", n, f"esc | away | menu {i}", f"Large font: {n.title()} menu")
    for i, n in enumerate(["file", "edit", "view", "playback", "settings", "help"])
] + [
    # 24 pt in a large window (1920x1200): the library and the queue at a large font with room
    # 24 pt in a taller window (1600x1400): the now playing wide again (compact at 1600x1000)
    ("tall-bigfont", "main", "playing-spectrogram-centered", "queue | play Small_Hours 70 6 | away",
     "Large font (24 pt) in a 1600x1400 window: playing, the now playing wide again"),
    ("tall-bigfont", "nowplaying", "at-rest", "away", "Large font, 1600x1400: now playing"),
    ("wide-bigfont", "main", "playing-spectrogram-centered", "queue | play Small_Hours 70 6 | away",
     "Large font (24 pt) in a 1920x1200 window: playing"),
    ("wide-bigfont", "library", "at-rest", "away", "Large font, 1920x1200: the library"),
    ("wide-bigfont", "queue", "at-rest", "away", "Large font, 1920x1200: the queue (playing row)"),
    ("wide-bigfont", "library", "expanded-playing",
     "js libraryController.showTrackInLibrary(root.uiBridge.queuePathAt(0)) | wait 0.6 | away | macro treekeys | "
     "expect now-row:centred", "Large font, 1920x1200: the library expanded to the playing track, its row centred and amber"),
    # the now playing's compact layout at 24 pt, art to none and back, in the wide window
    ("wide-bigfont", "_", "_artqueue", "js libraryController.collapseAll() | macro artqueue | away", ""),
    ("wide-bigfont", "motion", "art-to-no-art-24pt", "strip 1.2 0.1 sidebarPane:0,0,1,0.72 :: js root.uiBridge.next()",
     "Large font (24 pt), 1920x1200: track change from a track with album art to one without: the now-playing column"),
    ("wide-bigfont", "_", "_art-check", "until root.uiBridge.currentTrackCoverPath === '' | wait 0.8", ""),
    ("wide-bigfont", "motion", "no-art-to-art-24pt", "strip 1.2 0.1 sidebarPane:0,0,1,0.72 :: js root.uiBridge.previous()",
     "Large font (24 pt), 1920x1200: track change from a track without album art back to one with it: the now-playing column"),
] + [
    ("bigfont", "viewer", "spectrogram-windowed",
     "esc | away | js root.uiBridge.play() | js root.uiBridge.setViewerFullscreenMode(0) | js root.uiBridge.setShowFullscreenTitleStrip(true) | "
     "js viewerController.openSpectrogramViewer() | settle 1 | ready 3 | move xy:700,450 | wait 0.5",
     "Large font: spectrogram viewer, windowed, pointer over the plot"),
    ("bigfont", "viewer", "spectrogram-fullscreen",
     "js viewerController.closeSpectrogramViewer() | settle 1 | js root.uiBridge.setViewerFullscreenMode(1) | "
     "js viewerController.openSpectrogramViewer() | settle 1 | ready 3 | window spectrogramFullscreenWindow | move xy:700,450 | wait 0.5",
     "Large font: spectrogram viewer, fullscreen, title strip on, pointer over the plot"),
    ("bigfont", "viewer", "albumart-windowed",
     "window spectrogramFullscreenWindow | esc | window  | js viewerController.closeSpectrogramViewer() | settle 1 | "
     "js root.uiBridge.setViewerFullscreenMode(0) | js viewerController.openAlbumArtViewer() | settle 2 | wait 0.4 | "
     "move xy:800,500 | wait 0.3 | js viewerController.toggleAlbumArtInfoVisible(function(){}) | wait 0.4",
     "Large font: album art viewer, windowed, with the file info pane"),
    ("bigfont", "viewer", "albumart-fullscreen",
     "js viewerController.closeAlbumArtViewer() | settle 1 | js root.uiBridge.setViewerFullscreenMode(1) | "
     "js viewerController.openAlbumArtViewer() | settle 2 | wait 0.4 | window albumArtFullscreenWindow",
     "Large font: album art viewer, fullscreen, with the file info pane"),
    ("bigfont", "_", "_viewers-close", "window albumArtFullscreenWindow | esc | window  | js viewerController.closeAlbumArtViewer() | settle 1", ""),
    # (Last.fm on, as in the 10 pt walk's setup: its indicator is a Tab stop)
    ("bigfont", "_", "_focus-start", 'esc | away | js root.uiBridge.pause() | macro lastfm {"lastFmScrobbleListenedMs":60000} | '
     "away | unfocus", ""),
] + [
    # as many Tabs as the 10 pt walk (12 here made the 24 pt walk look shorter; it was the harness)
    ("bigfont", "focus", f"tab-{i:02d}", f"macro tab {i}", "Focus walk") for i in range(1, 25)
] + [
    ("narrow-bigfont", "main", "library-idle", "away", "Narrow window 1000x720 at a large font: library idle"),
    ("narrow-bigfont", "nowplaying", "idle-row", "away | shown nowPlayingIdleRow", "Now playing idle, narrow window, 24 pt"),
    ("narrow-bigfont", "main", "playing-spectrogram-centered", "queue | play Small_Hours 70 6 | away",
     "Narrow window at a large font: playing"),
    ("narrow-bigfont", "queue", "at-rest", "away | macro queuegeom rest-narrow-24pt",
     "Narrow window (1000x720) at 24 pt: queue at rest (playing row)"),
    ("narrow-bigfont", "_", "_queue24", "macro crop11 queue24 font24 mainQueuePane", ""),
    # the narrow (non-compact) card at 24 pt, and its track change across metadata cards: untagged, a tagged
    # track, a long title, a long album without art (Next three times, 1.4 s apart); the library header's travel
    ("narrow-bigfont", "nowplaying", "at-rest", "away", "Narrow window (1000x720) at 24 pt: now playing"),
] + _title_lines("narrow-bigfont", "Now playing at 24 pt, narrow window (1000x720)",
                 "queue | play Small_Hours 70 6 | away") + [
    ("narrow-bigfont", "_", "_meta-queue-n24", "js root.uiBridge.replaceWithPaths("
     + json.dumps([SI_FILES[3][1], _NORMAL_TRACK, _LONG_TITLE_TRACK, SI_FILES[0][1]])
     + ") | until root.uiBridge.queueLength === 4 | js viewerController.setVisualizationMode(0) | js root.uiBridge.playAt(0) | "
     "until root.uiBridge.currentTrackPath.indexOf('track07') >= 0 | wait 1.5 | away", ""),
    ("narrow-bigfont", "motion", "track-change-metadata-narrow24",
     "strip 4.4 0.1 sidebarPane:0,0,1,0.9 :: js root.uiBridge.next() ; wait 1.4 ; js root.uiBridge.next() ; wait 1.4 ; "
     "js root.uiBridge.next()",
     "Track changes across metadata cards in the narrow window at 24 pt (the narrow, not the compact, card): untagged, "
     "a tagged track with art, a long title, a long album without art: the card and the library header"),
    ("narrow-bigfont", "_", "_meta-close-n24", "js root.uiBridge.stop() | queue | play Small_Hours 70 6 | away", ""),
    # Stop, then Clear, in the narrow window at 24 pt (the large-font card); then playing again
    ("narrow-bigfont", "motion", "stop-to-idle-narrow24",
     "away | wait 0.6 | strip 1.6 0.1 sidebarPane:0,0,1,0.9 :: js root.uiBridge.stop() ; wait 0.6 ; js root.uiBridge.clearQueue()",
     "Stop, then Clear the queue (0.6 s later), narrow window at 24 pt: the now-playing card"),
    # medium -> long title in the narrow window at 24 pt (does the title's size change in use?)
    ("narrow-bigfont", "_", "_title-change-q", "js root.uiBridge.replaceWithPaths("
     + json.dumps([_TITLE_CASES[1][1], _TITLE_CASES[2][1]]) + ") | until root.uiBridge.queueLength === 2 | "
     "js viewerController.setVisualizationMode(0) | js root.uiBridge.playAt(0) | "
     "until root.uiBridge.currentTrackPath.indexOf('Morning Tide') >= 0 | wait 1.5 | away", ""),
    ("narrow-bigfont", "motion", "title-change-narrow24", "strip 1.2 0.1 sidebarPane:0,0,1,0.9 :: js root.uiBridge.next()",
     "Narrow window at 24 pt: from a medium title (two lines) to a long one (Next): the card and its title's size"),
    ("narrow-bigfont", "_", "_n24-replay", "queue | play Small_Hours 70 6 | away", ""),
    ("narrow-bigfont", "search", "results", "js globalSearchController.openDialog() | settle 0.8 | type mer | wait 0.8",
     "Narrow window at a large font: search palette"),
] + [
    ("narrow-bigfont", "dialog", f"preferences-{slug}", f"esc | macro openprefs {title} | away", f"Narrow window at a large font: Configure, {title} page")
    for title, slug in PREF_PAGES
] + [
    ("narrow-bigfont", "dialog", "about", "esc | macro opendialog aboutDialog | settle 1", "Narrow window at a large font: About"),
    ("narrow-bigfont", "menu", "playback", "esc | away | menu 3", "Narrow window at a large font: Playback menu"),
    ("narrow-bigfont", "_", "_close", "esc", ""),

    # ------------------------------------------------------------ contrast measurements (CONTRAST.md)
    # Each line records items (rect, declared colour, palette role) with `measure`, then grabs; contrast.py
    # samples the grab's pixels in those rects. Shots of area "contrast" are not put on sheets.
    ("contrast", "contrast", "menu-disabled",
     "menu 1 | macro measure menu-disabled-text text (function(){var m=root.menuBar.menus[1];for(var i=0;i<m.count;i++){var it=m.itemAt(i);if(it&&it.text&&!it.enabled)return it.contentItem}})() | "
     "macro measure menu-enabled-text text (function(){var m=root.menuBar.menus[1];for(var i=0;i<m.count;i++){var it=m.itemAt(i);if(it&&it.text&&it.enabled)return it.contentItem}})()",
     "Contrast: Edit menu, empty queue (disabled and enabled items)"),
    ("contrast", "_", "_play", "esc | queue | play Small_Hours 40 4 | away", ""),
    ("contrast", "contrast", "rows",
     f"click {QRow}@0.3,2.5 | hover {QRow}@0.3,4.5 0.3 | " + " | ".join(
         f"macro measure {label} {kind} " + FIND_VIS.format(name=name, k=k) for label, kind, name, k in (
             ("queue-row-primary", "text", "queueCell_title", 3), ("queue-row-secondary", "text", "queueCell_artist", 3),
             ("queue-row-number", "text", "queueCell_order", 3), ("queue-row-selected", "text", "queueCell_title", 2),
             ("queue-row-hover", "text", "queueCell_title", 4), ("queue-header-totals", "text", "regionHeaderTotals", 0),
             ("library-row-primary", "text", "libraryRowTitle", 2), ("library-header-totals", "text", "regionHeaderTotals", 1),
             ("now-playing-text", "text", "nowPlayingWideText", 0),
             ("status-bar-state", "text", "statusBarPlaybackStateLabel", 0), ("status-bar-format", "text", "statusBarFormat", 0),
             ("seek-played", "wave-played", "seekWaveform", 0), ("seek-unplayed", "wave-unplayed", "seekWaveform", 0))),
     "Contrast: queue rows (plain, selected, hovered), headers, library, now playing, status bar, seek waveform"),
    ("contrast", "contrast", "playing-selected",
     f"click {QRow}@0.3,0.5 | away | macro measure queue-playing-selected text " + FIND_VIS.format(name="queueCell_title", k=0),
     "Contrast: the playing row selected (amber on the selection tint)"),
    ("contrast", "contrast", "plot-plates",
     "hover spectrogramPaneItem@0.6,0.4 0.5 | macro measure plot-lane-label text " + FIND_VIS.format(name="plotLaneLabel", k=0) + " | "
     "macro measure plot-lane-mute text " + FIND_VIS.format(name="plotLaneMuteButton", k=0),
     "Contrast: plot plate text on the spectrogram"),
    ("contrast", "contrast", "focus-seek",
     "away | focus seekSlider | macro measure focus-ring-seek ring " + FIND_VIS.format(name="seekFocusRing", k=0),
     "Contrast: the seek bar's focus ring"),
    ("contrast", "contrast", "focus-volume",
     "focus volumeSlider | macro measure focus-ring-volume ring " + FIND_VIS.format(name="volumeFocusRing", k=0),
     "Contrast: the volume slider's focus ring"),
    ("contrast", "contrast", "focus-button",
     "focus transportButton_next | macro measure focus-ring-transport-button ring " + FIND_VIS.format(name="transportButton_next", k=0),
     "Contrast: a transport button's focus frame (style)"),
    ("contrast", "_", "_unfocus", "unfocus", ""),
    ("contrast", "contrast", "queue-hover-tint",
     "away | hover @plainQueueRow@0.3,0.5 0.4 | macro measure queue-hover-tint fill (function(){var o=[];var s=[root.Overlay.overlay.parent];"
     "while(s.length){var it=s.shift();if(!it.visible)continue;if(it.objectName==='queueRow'&&!it.rowSelected&&!it.isCurrentQueueRow)"
     "o.push([it.mapToItem(null,0,0).y,it]);var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}"
     "o.sort(function(a,b){return a[0]-b[0]});var h=(" + TARGETS_PLAIN + ");var ref=null;"
     "for(var k=0;k<o.length;k++)if(o[k][1]!==h){ref=o[k][1];break}return {_m:true,item:h,ref:ref}})()",
     "Contrast: the queue's hover tint against the view (a plain row)"),
    # The tree keyboard-focused (Tab focus, then Down and Up): the current row draws its 1 px frame. Measured on
    # the row that draws it (its tint's border on), not on the view's currentItem.
    ("contrast", "contrast", "library-focus-frame",
     "away | focus libraryView | key Down | key Up | wait 0.3 | macro measure library-focus-frame ring (function(){var v=" + FIND + "('libraryView');"
     "var s=[v];while(s.length){var it=s.shift();if(!it||!it.visible)continue;if(it.objectName==='rowBackgroundTint'&&it.border&&it.border.width>0)"
     "return {_m:true,item:it,color:root.uiPalette.focusRing};for(var i=0;i<it.children.length;i++)s.push(it.children[i]);}return null})()",
     "Contrast: the library's current-row focus frame (tree keyboard-focused)"),
    # The scrobble indicator's bar (N2: a 16 px glyph with a 2 px bar beneath it), against the status bar
    # The scrobble indicator's bar while listening, against the status bar: a bar of at least 3 px shows from 0 %
    ("contrast", "contrast", "scrobble-bar-listening-0",
     'unfocus | away | macro lastfm {"lastFmScrobbleListenedMs":0} | macro measure scrobble-bar-listening-0 bar '
     "(function(){var ind=" + FIND + "('scrobbleIndicator');return {_m:true,item:" + FIND + "('scrobbleIndicatorBar'),"
     "color:ind.barColor,ref:" + FIND + "('statusBar'),extra:{min_w:3,progress:ind.barProgress}}})()",
     "Contrast: the scrobble indicator's bar while listening, at 0 % (at least 3 px) against the status bar"),
    ("contrast", "contrast", "scrobble-bar-listening-25",
     'macro lastfm {"lastFmScrobbleListenedMs":30000} | macro measure scrobble-trough bar '
     "(function(){return {_m:true,item:" + FIND + "('scrobbleIndicatorTrough'),ref:" + FIND + "('statusBar')}})() | "
     'macro measure scrobble-bar-listening-25 bar '
     "(function(){var ind=" + FIND + "('scrobbleIndicator');return {_m:true,item:" + FIND + "('scrobbleIndicatorBar'),"
     "color:ind.barColor,ref:" + FIND + "('statusBar'),extra:{min_w:3,progress:ind.barProgress}}})()",
     "Contrast: the scrobble indicator's bar while listening, at 25 % of its threshold, against the status bar"),
    ("contrast", "contrast", "scrobble-bar-listening",
     'macro lastfm {"lastFmScrobbleListenedMs":60000} | macro measure scrobble-bar-listening bar '
     "(function(){var ind=" + FIND + "('scrobbleIndicator');return {_m:true,item:" + FIND + "('scrobbleIndicatorBar'),"
     "color:ind.barColor,ref:" + FIND + "('statusBar'),extra:{min_w:3,progress:ind.barProgress}}})()",
     "Contrast: the scrobble indicator's bar while listening (50 %) against the status bar"),
    ("contrast", "contrast", "scrobble-bar-done",
     'macro lastfm {"lastFmScrobbleState":3} | macro measure_na scrobble-bar-done an accepted scrobble shows no bar by '
     "design (R5: its state is the glyph's shape; the glyph is measured below) | macro measure scrobble-glyph-done glyph "
     "(function(){var ind=" + FIND + "('scrobbleIndicator');return {_m:true,item:" + FIND + "('scrobbleIndicatorGlyph'),"
     "color:ind.glyphColor,ref:" + FIND + "('statusBar')}})()",
     "Contrast: scrobbled: no bar by design (n/a); the Last.fm glyph (its own colour) against the status bar"),
    ("contrast", "_", "_lastfm-off", 'macro lastfm {"lastFmShowScrobbleProgress":false}', ""),

    # ------------------------------------------------------------ app icon
    ("icon-noart24", "_", "_noart24", "queue | play Static_Bloom - 1 | until root.uiBridge.currentTrackCoverPath === '' | "
     "wait 1 | away | macro motif", ""),  # the no-art mark at 24 pt (the compact cover) and the glyph
    ("icons", "_", "_icons", "macro icons", ""),
    ("icons", "_", "_icons-ab", "macro icons_ab", ""),
    # The app icon: sizes, context, variants (when rendered); the About window (its own 128 px icon)
    ("icons", "_", "_appicon", "macro appicon", ""),
    ("icons", "_", "_appicon2", "macro appicon2", ""),
    ("icons", "_", "_appicon3", "macro appicon3", ""),
    ("icons", "_", "_appicon4", "macro appicon4", ""),
    ("icons", "dialog", "about-window", "macro opendialog aboutDialog | settle 1", "The About window (the app's own, its icon)"),
    ("icons", "_", "_about-close", "esc | settle 0.5", ""),
    ("icons", "_", "_appicon5", "macro appicon5", ""),  # (after the About window: its title bar)
    ("icons", "_", "_appicon6", "macro appicon6", ""),
    ("icons", "_", "_appicon7", "macro appicon7", ""),
    ("icons", "_", "_appicon8", "macro appicon8", ""),
    ("icons", "_", "_appicon9", "macro appicon9", ""),
    ("icons", "_", "_appicon10", "macro appicon10", ""),
    ("icons", "_", "_appicon11", "macro appicon11", ""),  # (only with SHOT_ICON_VARIANTS=1)
]

# States the pack does not drive, with the reason (INDEX.md lists them).
NOT_CAPTURED = [
    ("Last.fm real states", "No network and no Secret Service in the isolated session; the scrobble indicator is fed a "
                            "stand-in bridge with the Last.fm fields set (the real component draws them). Sign-in flow "
                            "(Preferences > Last.fm authorizing) not shown."),
    ("iTunes real search", "Would send fixture names to Apple; results are injected stand-ins using fixture covers."),
    ("Offline / backend stopped (real)", "Needs the Rust backend to die. The status bar's disconnected and stopped "
                                         "looks are shown with injected bridge fields; the main window's "
                                         "backend-stopped message above the transport is not shown."),
    ("Library scan progress (real)", "The fixture library scans in well under a second and a rescan of unchanged "
                                     "files reports no progress; the scan indicator states are injected."),
    ("Queue drag and drop", "Needs a platform drag (QDrag) the probe cannot synthesize; the drop indicator and "
                            "displaced-row slide are not shown."),
    ("Column resize drag in the queue header", "Not driven (pointer drag on the handle); handles only seen at rest."),
    ("Native file/folder dialogs", "Platform (KDE portal) dialogs, not Ferrous UI."),
    ("Tag editor saving / quit wait (real)", "Saving writes files; the quit-wait dialog is opened directly instead."),
    ("Tag editor status details with content", "Only reachable after a save error; opened directly (empty)."),
    ("Quit save failed (real)", "Needs a failing save at quit; opened directly with a sample message."),
    ("Menus in strips", "Menus are windows of their own (#642), which the still shots draw in, but a strip's images "
                        "are of the main window: the File menu strip shows the menu bar, not the menu."),
    ("Context menus at the pointer", "Context menus are opened from the keyboard (the Menu key, or Shift+F10) wherever "
                                     "the view takes it: library, queue, the scrobble indicator and the fullscreen "
                                     "album art viewer; they open on the selected row or control. The windowed album "
                                     "art viewer and the search palette take those keys as shortcuts, which the "
                                     "probe's keys go past: the viewer's menu is shot from a right click (at the "
                                     "click point; from the keys it opens at the art's centre), the palette's from a "
                                     "right click and from a real Menu key."),
    ("Dialog windows' open and close animation", "Configure and About are real windows (DialogWindow): KWin animates "
                                                 "their opening and closing as a compositor effect, which the app's own "
                                                 "frame readback cannot see; their strips show the client window only "
                                                 "(it appears in one frame)."),
    ("Display-sleep inhibition failure, MPRIS-driven changes", "System integrations; not visible UI states."),
    ("Scale 2 for both themes on every area", "Only key states (brief: 2-3)."),
]


# ---- packaging (pack.py)
# Sheet tile crops in logical px of the 1600x1000 window (x, y, w, h), per area or "area/state".
# Menus, dialogs and the search palette are cropped around their popups from the geometry recorded at
# grab time; these are the fallbacks and the bands for areas without popups. Tooltips and other
# popups visible in a shot are added to the band.
SHEET_CROPS_PX = {
    "menubar": (0, 0, 820, 340),
    "statusbar": (0, 880, 1600, 120),
    "transport": (0, 0, 820, 150),
    "seek": (0, 0, 1600, 150),
    "volume": (800, 0, 800, 150),
    "nowplaying": (0, 26, 820, 520),
    "queue": (361, 75, 1239, 580),
    "library": (0, 75, 820, 700),
    "plot": (361, 646, 1239, 329),
    "menu": (0, 0, 800, 560),
    "dialog": (250, 100, 1100, 800),
    "search": (250, 60, 1100, 760),
}
# Strip view crops (x, y, w, h) in the recorded frame's pixels, where the automatic "region that
# changes" is not what the strip is about.
# (Full-window recordings: the dialog/palette rect from the geometry pass, padded.)
STRIP_VIEW = {
    # the now-playing column, the sidebar from the app (its whole-window image: EXTRA_VIEWS); from the sidebar's top,
    # not the transport's: the new track's playhead moves in the seek waveform from x 168
    "first-play-from-idle": "sidebarPane:0,0,1,0.567",
    "first-play-no-art": "sidebarPane:0,0,1,0.567",
    "dialog-open-close": (420, 240, 760, 540),
    "preferences-open": (329, 191, 942, 618),
    "search-open": (370, 77, 860, 646), "search-close": (370, 77, 860, 646),
    "search-typing": (370, 77, 860, 646),
    # the plot, both lanes and the time on the bottom one, not a thumbnail of the window (the pans' band: vi-moves-band)
    "vi-pan-waveform": "spectrogramPane", "vi-pan-spectrogram": "spectrogramPane",
    "first-scan": (0, 75, 820, 470),
    "animoff-dialog-open": (420, 240, 760, 540),
    "animoff-search-open": (370, 77, 860, 646),
}
# Strips exported in both themes (colour matters); the rest are dark only (raw frames keep both).
STRIP_BOTH_THEMES = {"albumart-info-toggle", "hover-fade-queue-row", "mute-toggle", "seek-drag-box", "queue-now-move", "first-scan", "stop-to-idle", "mode-switch-mixed", "first-play-from-idle", "first-play-no-art", "library-expand", "library-collapse", "track-change-same-album", "track-change-new-album", "art-to-no-art", "no-art-to-art", "scrobble-ring-fill", "spectrogram-zoom",
                     "dialog-open-close", "search-open", "search-close",  # (light: the dims)
                     "play-pause-glyph-2x-pause", "viewer-autohide-fullscreen"}  # light glyphs and overlays
# How a strip is packed: by default 8-12 key frames. window=(a, b): every rendered frame from the one
# before the action to b ms after it; zoom: integer pixel zoom (nearest neighbour); free: no 700 px
# minimum for its tiles (tiny crops shown many to a row).
STRIP_MODE = {
    # view=(x, y, w, h): a crop of the recording (here the dialog's own window): the busy indicator and its label
    "itunes-searching-spinner": dict(window=(0, 250), zoom=3, view=(400, 306, 120, 64)),
    "mode-switch-lane": dict(window=(0, 400)), "mode-switch-lane-back": dict(window=(0, 400)),
    "mode-switch-lane-first": dict(window=(0, 400)),
    "zoom-notch-lane": dict(window=(0, 400)), "zoom-reset-lane": dict(window=(0, 400)),
    "play-pause-glyph-2x": dict(window=(0, 300), zoom=2, free=True),
    "play-pause-glyph-2x-pause": dict(window=(0, 300), zoom=2, free=True),
    "hover-fade-queue-row": dict(window=(0, 700), zoom=2, free=True),  # 3 rows at 2x
    "transport-press": dict(window=(0, 500), zoom=2, free=True),
    "hover-fade-transport-button": dict(window=(0, 700), zoom=2, free=True),
    "hover-fade-plot-tool-button": dict(window=(0, 700), zoom=3, free=True),
    "animoff-play-pause-glyph": dict(window=(0, 300), zoom=2, free=True),
}
# The item each area is about: its rect is recorded at grab time, and the sheet tile is cropped to it
# (plus any popup or tooltip shown), at every window size and font.
AREA_SUBJECT = {"transport": "transportBar", "seek": "transportBar", "volume": "transportBar", "statusbar": "statusBar",
                "nowplaying": "nowPlayingPanel", "library": "libraryPane", "queue": "mainQueuePane",
                "plot": "spectrogramPane"}
# States of these areas must show an open popup (menu, dialog, palette) at grab time, or the capture fails.
VERIFY_OPEN_AREAS = {"menu", "dialog", "search"}
VERIFY_OPEN_EXEMPT = {"menubar-hover"}  # the menu bar itself, hovered: no menu by design
# States of other areas that show an open menu, which must be shown at grab time too.
VERIFY_MENU_STATES = {("menubar", "open"), ("contrast", "menu-disabled")}
# The overview sheet ("every area at rest"), one page per theme, first in the core set: (area, state).
OVERVIEW = [("main", "playing-spectrogram-centered"), ("transport", "at-rest"), ("nowplaying", "at-rest"),
            ("queue", "at-rest"), ("library", "at-rest"), ("plot", "at-rest"), ("statusbar", "at-rest"),
            ("menubar", "open"), ("dialog", "preferences-library"), ("search", "results"),
            ("viewer", "spectrogram-fullscreen-title-strip"), ("dialog", "about")]
# Comparison sheets: rows = states, columns = themes, tiles cropped as on their area's sheets.
COMPARE_SHEETS = [
    dict(name="plot-floors", title="Plot floor, dark vs light: empty before audio (at rest: rest lines), spectrogram playing, waveform playing",
         states=[("plot", "floor-empty"), ("plot", "floor-spectrogram"), ("plot", "floor-waveform")]),
    dict(name="menubar", title="Menu bar, dark vs light: File open with the pointer on its second item, then hovered",
         states=[("menubar", "open"), ("menubar", "hovered")]),
]
# Curated sheets (one per theme listed): tiles as on their area's sheets. A state is (area, state) or
# (area, state, variant) with variant "font24", a window size such as "1000x720", or both ("1920x1200+font24").
PICK_SHEETS = [
    dict(name="library-nowplaying", themes=("dark",),
         title="Library and now playing: the playing name collapsed and expanded, the text block, no cover",
         states=[("library", "collapsed-holding-playing"), ("library", "follows-playing"), ("library", "expanded-playing"),
                 ("nowplaying", "at-rest"), ("nowplaying", "long-title"), ("nowplaying", "no-cover")]),
    dict(name="dialogs", themes=("dark",),
         title="Dialogs: library folders, the Remove confirmation as a window over them, Recent Issues, Diagnostics",
         states=[("dialog", "preferences-library-two-roots"), ("dialog", "confirm-remove-library-root"),
                 ("dialog", "recent-errors"), ("dialog", "diagnostics")]),
    dict(name="dialogs-light", themes=("light",),
         title="Dialogs, light: library folders, the Remove confirmation as a window over them, Recent Issues, Diagnostics",
         states=[("dialog", "preferences-library-two-roots"), ("dialog", "confirm-remove-library-root"),
                 ("dialog", "recent-errors"), ("dialog", "diagnostics")]),
    dict(name="viewers", themes=("dark",),
         title="Viewers and pane: fullscreen strip, windowed with the meter, windowed zoomed, 5.1 in the pane and fullscreen",
         states=[("viewer", "spectrogram-fullscreen-title-strip"), ("viewer", "spectrogram-windowed-meter"),
                 ("viewer", "spectrogram-windowed-zoomed"), ("plot", "5.1-levelmeter-lanes"), ("viewer", "5.1-fullscreen")]),
    dict(name="light", themes=("light",),
         title="Light: the 5.1 pane (meter on), Replace from iTunes with no results, searching, and results",
         states=[("plot", "5.1-levelmeter-lanes"), ("dialog", "itunes-empty"),
                 ("dialog", "itunes-searching"), ("dialog", "itunes-results")]),
    dict(name="large-font", themes=("dark",),
         title="Large font (Noto Sans 24 pt): main window at 1600x1000 (compact now playing), 1600x1400 and 1920x1200, Preferences",
         states=[("main", "playing-spectrogram-centered", "font24"), ("main", "playing-spectrogram-centered", "1600x1400+font24"),
                 ("main", "playing-spectrogram-centered", "1920x1200+font24"),
                 ("dialog", "preferences-library", "font24")]),
]
# Dialog states whose shot must be of this window (its title starts with the first string) showing this body
# text (a fragment; None: the title alone). The runner checks right after the grab, pack.py again over the
# round: a confirmation grabbed as its parent window fails loudly instead of passing as the parent.
_DIALOG_EXPECT = {
    "confirm-remove-library-root": ("Remove Library Folder", "Remove the library folder"),
    "library-root-name-rename": ("Rename Library Folder", "Without a name, the library shows the folder"),
    "library-root-name-add": ("Add Library Folder", "Without a name, the library shows the folder"),
    "confirm-clear-diagnostics": ("Delete Diagnostics Log", "Delete the diagnostics log?"),
    "quit-save-failed": ("Couldn’t Save Before Quitting", "could not be saved"),
    "tag-editor-auto-number": ("Auto Number", "Number tracks in the table"),
    "tag-editor-rename-preview": ("Rename Files", "will be renamed"),
    "tag-editor-status-details": ("Tag Editor Details", None),
    "tag-editor-quit-wait": ("Finishing Tag Changes", "Ferrous quits once the tag editor"),
    "tag-editor-close-confirm": ("Unsaved Changes", "Save changes before closing the tag editor?"),
    "confirm-replace-cover": ("Replace Album Art", "Replace the album art of"),  # (its heading, set by applySuggestion)
    "tag-editor-editing": ("Edit Tags", None),
    "itunes-error": ("Replace", "search iTunes"),  # (the app's failed state, "Couldn’t search iTunes")
}
DIALOG_EXPECT = dict(_DIALOG_EXPECT, **{k + "-default-focus": v for k, v in _DIALOG_EXPECT.items()})
# The core set, in order: about 20 images every reviewer views (their context holds no more). ("sheet", group,
# theme) is the first page of that sheet; ("strip", name, theme) is the transition's one merged image. Every
# other sheet and strip is reference.
# about 15 (a reviewer's context lost 25 before its notes): the 1:1 sheets, the overview, the light first run,
# the details, the most telling motion; CORE-FACTS.md gives each one's measured facts as text. A larger core's other
# images (whole windows fitted, the picks, the tag editor, HiDPI, the reverse art change, art-perceived, search)
# are reference.
CORE = [
    ("sheet", "overview", "dark"),  # (the light overview is reference: light is in the light images)
    ("sheet", "window-light-firstrun", "light"),
    ("sheet", "onetoone-dark", "dark"), ("sheet", "onetoone-light", "light"), ("sheet", "details-1to1", "dark"),
    ("sheet", "light-details", "light"), ("sheet", "light-511-itunes", "light"), ("sheet", "waveform-meter", "dark"),
    ("sheet", "noart-dark", "dark"), ("sheet", "noart-rows-dark", "dark"),  # (the compact stand-ins; the no-art glyph and the ring lane apart)
    ("sheet", "closeups", "both"),
    ("strip", "first-play-from-idle", "dark"), ("strip", "first-play-metadata", "dark"),
    ("strip", "art-to-no-art-1to1", "dark"), ("strip", "pair:mode-switch", "dark"),
    ("strip", "pair:library-expand-collapse", "dark"), ("strip", "pair:track-change", "dark"),
] + ([("sheet", "iconab", "both")] if ICON_AB else [])  # the icon comparison, while there is one (SHOT_ICON_AB=1)
# any recording whose frames are neither its start nor its end nor a blend of them for longer than this (a
# plot recording's `neither_ms`) joins the core by itself, its worst theme, unless the core shows it already.
NEITHER_CORE_MS = 50
# (CORE-FACTS.md) the CONTRAST.md rows quoted under a core sheet, by element-name fragment (case ignored).
CORE_FACT_CONTRAST = {
    "onetoone-dark": ["queue", "library", "now playing", "transport", "seek", "volume", "art info", "header"],
    "onetoone-light": ["queue", "library", "now playing", "transport", "seek", "volume", "art info", "header"],
    "details-1to1": ["last.fm", "scrobble", "search", "mode switch", "plate"],
    "light-details": ["last.fm", "scrobble", "now playing", "stand-in", "status bar"],
    "light-511-itunes": ["lane", "meter", "5.1", "itunes", "dialog"],
    "waveform-meter": ["waveform", "meter"],
    "closeups": ["transport", "meter", "hover", "pressed"],
    "window-light-firstrun": ["first run", "empty", "placeholder", "library"],
    "overview": [],
    "noart-dark": ["library", "focus", "now playing"],
}
# Popup open/close recordings (and tooltips): any of them with a measured blink (a scope's or an element's)
# joins the core by itself, its worst theme, unless the core already shows it (a regression such as an
# album-art-viewer close blink is in front of the reviewers).
POPUP_STRIP_RE = r"(^|-)(open|close|tooltip)(-|$)"
# Sheets of images made at their final size, dark above light, never scaled (their 1x crops stay 1x):
# (sheet name, area, state, title).
STACK_SHEETS = [
    ("ring", "scrobblering", "progression",
     "Scrobble indicator (glyph and bar), dark above light: every state beside a running scan bar, at 1x and 6x"),
    ("iconab", "iconab", "a-vs-b", f"App icon: {ICON_A_LABEL} vs {ICON_B_LABEL}, at 1x and 4x" if ICON_AB
     else "App icon (installed), at 1x and 4x on Breeze Dark and Light"),
    ("onetoone-dark", "onetoone", "readable", "Dark at 1:1: transport, now playing, library, queue, art info, the queue/pane seam",
     dict(themes=("dark",))),
    ("onetoone-light", "onetoone", "readable", "Light at 1:1: transport, now playing, library, queue, art info, the queue/pane seam",
     dict(themes=("light",))),
    ("closeups", "closeup", "controls",
     "Close-ups, dark above light: a transport button at rest, hovered, pressed (3x); the level meter's top per plot "
     "state (2x) with each lane's dB; the plot's floor colours", dict(floors=True)),
    ("meteredge", "edge", ("meter-main", "meter-viewer"),
     "The plot's right edge with the level meter on, 4x: the main window's pane and the windowed spectrogram viewer, dark above light"),
    ("queue24", "queue24", "font24", "The queue at 24 pt (Noto Sans), 1:1: window 1600x1000 above 1000x720, dark",
     dict(themes=("dark",), sizes=("1600x1000", "1000x720"))),
]
# 1:1 sheets laid out by hand (pack.custom_sheet): (name, title, theme, rows); a cell is (area, state, crop, label)
# or a column of them; crop "subject" (the area's item at the grab), (fx, fy, fw, fh) of the shot, or None.
_MSG = (0.18, 0.36, 0.64, 0.36)  # the middle of the iTunes dialog: its glyph and message
CUSTOM_SHEETS = [
    # small things at 1:1, from three sessions: the status bar with a line through the Last.fm glyph's centre
    # (both themes), the search field at 24 pt, and the mode switch's plate during a switch (frames of the
    # clicked switch to the waveform, 3x)
    ("details-1to1", "Details at 1:1: the Last.fm glyph's centre line, the search field at 24 pt, the mode switch hovered "
     "without focus (3x, dark and light) and its plate during a click (3x)", "dark",
     [[("details", "statusbar-glyph", None, "Status bar, dark")],
      # (the field is as wide as the window at 24 pt, over the reviewers' 1568 px with the sheet's margins: its two
      # ends, the glyph and the query, then the buttons; only its plain middle is left out)
      [("details", "searchfield24pt", None, "Search field at 24 pt (1600x1000), dark: its left end",
        dict(variant="font24", part=(0, 0, 0.42, 1))),
       ("details", "searchfield24pt", None, "Its right end (the plain middle left out)",
        dict(variant="font24", part=(0.75, 0, 0.25, 1)))],
      [("details", "modeswitch-hover", None, "Mode switch hovered without focus, dark"),
       ("details", "modeswitch-hover", None, "light", dict(theme="light"))],
      [dict(strip="mode-switch-lane", item="visualizationModeSwitch", times=[-17, 17, 34, 67, 150], zoom=3,
            label="Mode switch, clicked to the waveform: its plate at 3x, frame by frame (ms after the click)")]]),
    # the core's two whole windows, scaled to fit ~1 MP (a reviewer's input would scale them anyway)
    ("window-dark-meter", "Dark: the main window playing, frequency scale and level meter on", "dark",
     [[("main", "playing-scale-levelmeter", None, "Main window, dark, meter and scale on", dict(fit=(1220, 770)))]]),
    # the light first run at 1:1 (its content is in the window's top 560 px: 1300x560 of 1600x1000)
    ("window-light-firstrun", "Light at 1:1: the main window at first run (no library folder yet), its top left 1300x560 px",
     "light", [[("main", "first-run-empty-library", (0, 0, 0.8125, 0.56), "Main window, light, first run, actual pixels")]]),
    # the dark idle window (library loaded, nothing queued), fitted; the waveform with the meter on at 1:1
    ("window-dark-idle", "Dark: the main window idle (library loaded, nothing queued, stopped)", "dark",
     [[("main", "library-idle", None, "Main window, dark, idle", dict(fit=(1220, 770)))]]),
    # (both themes, the pane's right half each: the waveform and the meter with its top seam)
    ("waveform-meter", "At 1:1: the plot pane in waveform mode with the level meter on, its right half, dark and light",
     "dark", [[("plot", "waveform-levelmeter", "subject", "Dark: the pane's right half (actual pixels)", dict(part=(0.5, 0, 0.5, 1))),
               ("plot", "waveform-levelmeter", "subject", "Light", dict(part=(0.5, 0, 0.5, 1), theme="light"))]]),
    # one HiDPI image in the core: scale 2 at 1:1 device pixels (nothing scaled), the main window's top left
    ("hidpi-2x", "Scale 2 (HiDPI), dark, at 1:1 device pixels: the main window's top left (512x480 logical px), playing",
     "dark", [[("main", "playing-spectrogram-centered", (0, 0, 0.32, 0.48), "Main window at scale 2, actual device pixels",
                dict(scale=2))]]),
    # (the tag editor opens 1600 px wide, 1624 with its decoration: at 1:1 only in halves, one theme)
    ("tageditor-dark", "The tag editor at 1:1 (its own window, 1624 px wide with its decoration): its left half above "
     "its right half, dark", "dark", [[("dialog", "tag-editor", (0, 0, 0.5, 1), "Left half")],
                                      [("dialog", "tag-editor", (0.5, 0, 0.5, 1), "Right half")]]),
    # light details at 1:1: the now playing without album art (wide and compact), and the status bar in every
    # Last.fm state at full width (1600 px), as its left half above its right half
    ("light-details", "Light at 1:1: now playing without album art (wide; compact at 24 pt), and the status bar in each "
     "Last.fm state, full width as its left half above its right half", "light",
     [[("nowplaying", "no-cover", "subject", "Now playing, no album art (wide)"),
       ("nowplaying", "no-cover", "subject", "Compact now playing at 24 pt, no album art", dict(variant="font24"))]] +
     [[[("statusbar", f"lastfm-{k}", "subject", f"Last.fm {k}: left half, then right half", dict(part=(0, 0, 0.5, 1), lh=17)),
        ("statusbar", f"lastfm-{k}", "subject", "", dict(part=(0.5, 0, 0.5, 1), lh=2, gap=5))]]
      for k in ("listening-0", "listening-half", "listening-almost", "sending", "scrobbled", "not-scrobbled",
                "queued-offline", "failed", "paused")]),
    # the stand-in cover's cases (SI_FILES), wide; and compact at 24 pt with the library's no-art glyph and
    # its focus ring beside the scrollbar, per theme
    *[(f"standin-wide-{th}", f"{th.capitalize()} at 1:1: the stand-in cover, wide: an album that wraps, no album, album = "
       "title, an untagged file", th,
       [[("nowplaying", f"standin-{k}", "subject", lb) for k, lb in
         (("long-album", "Album that wraps"), ("no-album", "No album tag"), ("same-name", "Album = title"), ("untagged", "Untagged"))]])
      for th in ("dark", "light")],
    # (split in two: G2's compact cards reserve their lines, so all four with the rows no longer fit 1.15 MP)
    *[(f"noart-{th}", f"{th.capitalize()} at 1:1: the compact stand-in at 24 pt (an album that wraps, no album, album = "
       "title, untagged)", th,
       [[("nowplaying", "standin-long-album", "subject", "Compact, album that wraps", dict(variant="font24")),
         ("nowplaying", "standin-no-album", "subject", "Compact, no album tag", dict(variant="font24"))],
        [("nowplaying", "standin-same-name", "subject", "Compact, album = title", dict(variant="font24")),
         ("nowplaying", "standin-untagged", "subject", "Compact, untagged", dict(variant="font24"))]])
      for th in ("dark", "light")],
    *[(f"noart-rows-{th}", f"{th.capitalize()} at 1:1 and 4x: the library's no-art glyph and the focused row's ring beside "
       "the scrollbar", th,
       [[("details", "library-noart-glyph", None, "Library no-art glyph"),
         ("details", "library-ring-lane", None, "Focus ring and scrollbar lane (large library)")]])
      for th in ("dark", "light")],
    ("light-511-itunes", "Light at 1:1: the 5.1 pane (meter on) and Replace from iTunes with results",
     "light", [[("plot", "5.1-levelmeter-lanes", "subject", "The 5.1 pane, meter on, pointer over it (its left 920 px)",
                 dict(max_w=920))],
               [("dialog", "itunes-results", None, "Replace from iTunes: results (its own window; its top 600 px)",
                 dict(max_h=600))]]),
]
# A transition's frames at 1:1 (pack.frames11_job): crops in window coordinates ("fixed", rect) or following
# an item of the trace ("follow", pattern, pad above, height, x, width); opacity series (optional) and geometry
# curves (pattern, "y" | "h" | "miny": the smallest y of the matching rows) beside them.
_ART11 = dict(
    # (x 0, the recording's width: the sidebar as the app lays it out)
    crops=[("Metadata (follows the artist line)", ("follow", "*wideArtistAlbum/nowPlayingArtistAlbum", 34, 84, 0, None)),
           ("The cover slot's middle (fixed: the slot stays)", ("fixed", (0, 215, None, 84))),
           ("Library rows (follows one of them)", ("follow", "*libraryRowContent/libraryRowTitle", 30, 84, 0, None))],
    opacity=[("outgoing title", "*Outgoing*Title*"), ("incoming title", "*Incoming*Title*"),
             ("outgoing cover", "*albumArtOutgoing"), ("stand-in cover", "*albumArtStandIn"),
             ("outgoing stand-in", "*albumArtOutgoingStandIn")],
    geometry=[("library first row y", "*libraryRowContent/libraryRowTitle*", "miny"),
              ("artist line y", "*wideArtistAlbum/nowPlayingArtistAlbum", "y")],
    note="Each row is one rendered frame; the crops are the same size in every row, so text doubling, a jump or a "
         "blank frame is seen as it was drawn.")
FRAMES11 = {
    "art-to-no-art": dict(_ART11, title="Album art to none at 1:1: the metadata spots and the library header, frame by frame"),
    "no-art-to-art": dict(_ART11, title="No album art to art at 1:1: the metadata spots and the library header, frame by frame"),
}
# a recording's whole region as perceived at real speed, a row per recording (pack.perceived_sheet):
# (name, title, [(strip, theme, row label)])
PERCEIVED_SHEETS = [
    ("art-perceived", "Art changes, the whole left column as perceived at real speed",
     [("art-to-no-art", "dark", "Album art to none"), ("no-art-to-art", "dark", "No album art to art")]),
    # First Play's metadata alone (the wide text block's items, wherever they are shown), 1:1, eight moments
    ("first-play-metadata", "First Play from idle: only where the now-playing metadata (title, artist line, details "
     "line) ends up, as perceived at real speed, 1:1",
     [("first-play-from-idle", "dark", "First Play from idle", dict(items=["*nowPlayingWideText/*"], at="end", pad=8,
                                                                   moments=8, per_line=4))]),
]
# an item's geometry sampled from a recording's trace at evenly spaced times after its first transition
# starts (pack.geometry_samples: CORE-FACTS.md and MOTION-METRICS.md): name -> (step ms, count, [(label, pattern,
# "rect" | "miny")]); "rect" follows the matching item shown most at the end (then with the most trace points),
# "miny" the topmost of them.
GEOMETRY_SAMPLES = {
    "first-play-from-idle": (50, 10, [
        ("cover slot", "*nowPlayingCoverArea/nowPlayingCover", "rect"),
        ("cover image", "*nowPlayingCover/Image", "rect"),
        # (square at every frame, centred in the band: w x h = w/h at x)
        ("cover tile, shown part", "*nowPlayingCoverArea/nowPlayingCover", "aspect"),
        ("cover tile", "*nowPlayingCoverArea/nowPlayingCover", "frames"),  # (360x360 throughout, the clip grows)
        ("cover band (the clip)", "*sidebarPane/nowPlayingPanel/nowPlayingCoverArea", "frames"),
        # (the idle row's and the cover's luminance per frame, beside the "other" blink)
        ("idle row luminance", "*nowPlayingPanel/nowPlayingIdleRow", "lum"),
        ("cover luminance", "*nowPlayingCoverArea/nowPlayingCover", "lum"),
        ("cover image aspect", "*nowPlayingCover/Image", "aspect"),
        ("stand-in cover", "*nowPlayingCover/albumArtStandIn", "rect"),
        ("lines (opacity)", "*nowPlayingLines", "rect"),
        ("text block", "*liveText/nowPlayingWideText", "rect"),
        ("title", "*nowPlayingWideText/wideTitle*", "rect"),
        ("artist line", "*nowPlayingWideText/wideArtistAlbum*", "rect"),
        ("details line", "*nowPlayingWideText/wideDetails*", "rect"),
        ("idle row", "*nowPlayingIdleRow*", "rect"),
        ("library first row", "*libraryRowContent/libraryRowTitle*", "miny")]),
}
# Two transitions side by side in one image (name, left strip, right strip, title, themes): e.g. both
# directions of the mode switch. Each half is a merged image of its own at 777 px.
STRIP_PAIRS = [
    # a queue row's hover fading in and out (2x) | the mute toggle
    ("hover-mute", "hover-fade-queue-row", "mute-toggle",
     "A queue row's hover fading in, then out (2x, left); Mute, then unmute (right)", ("dark",)),
    # the queue's "now" row moving on Next, dark | light; the play/pause glyph | the first scan
    ("queue-now-themes", "queue-now-move@dark", "queue-now-move@light",
     "Next: the queue's playing row moving down one row: dark (left), light (right)", ("dark",)),
    ("chrome-motion", "play-pause-glyph", "first-scan",
     "The transport's play/pause glyph changing (left); the first library scan, scanning to done (right)", ("dark",)),
    # Stop -> idle dark beside light; at 24 pt in the narrow window beside the queue running out on its own
    ("stop-to-idle-themes", "stop-to-idle@dark", "stop-to-idle@light",
     "Stop, then Clear the queue (0.6 s later), wide: dark (left), light (right)", ("dark",)),
    ("idle-paths", "stop-to-idle-narrow24", "queue-end-to-idle",
     "Stop, then Clear, in the narrow window at 24 pt (left); the queue running out on its own, wide (right)",
     ("dark",)),
    # album art to none, wide, dark beside light
    ("art-to-no-art-themes", "art-to-no-art@dark", "art-to-no-art@light",
     "Album art to none (Next), wide: dark (left), light (right)", ("dark",)),
    ("track-change", "track-change-new-album", "track-change-same-album",
     "Track change: to another album, the cover changing (left); within the album (right)", ("dark", "light")),
    ("art-switch-24pt", "art-to-no-art-24pt", "no-art-to-art-24pt",
     "Large font (24 pt), 1920x1200: album art to none (left) and back (right), the compact now playing", ("dark",)),
    ("art-switch", "art-to-no-art", "no-art-to-art",
     "Album art to none (left) and back (right): the now-playing column, per-element blink", ("dark", "light")),
    # the FIRST switch to the waveform since the track started beside a LATER one (per channel); the mixed view's
    # later switch in dark beside light ("name@theme": a half of another theme)
    # First Play dark beside light; the metadata track change wide beside 24 pt compact
    ("first-play-themes", "first-play-from-idle@dark", "first-play-from-idle@light",
     "First Play from idle: dark (left), light (right)", ("dark",)),
    ("track-change-cards", "track-change-metadata", "track-change-metadata-24pt",
     "Track changes across metadata cards (untagged, tagged with art, long album): wide (left), 24 pt compact (right)",
     ("dark",)),
    # the mixed view's first switch beside a later one; First Play without art dark | light; Stop -> idle beside the
    # plates' reveal
    ("mode-switch-mixed-first-later", "mode-switch-mixed-first", "mode-switch-mixed",
     "Mode switch to the waveform in the mixed view: the FIRST switch since the track started (left), a LATER one (right)",
     ("dark",)),
    ("first-play-no-art-themes", "first-play-no-art@dark", "first-play-no-art@light",
     "First Play from idle of a track without album art: dark (left), light (right)", ("dark",)),
    ("mixed-back-plates", "mode-switch-mixed-back", "plates-reveal",
     "Mode switch back, waveform to spectrogram, in the mixed view (left); the pointer entering the pane: the plates "
     "reveal (right)", ("dark",)),
    ("mode-switch-first-later", "mode-switch-lane-first", "mode-switch-lane",
     "Mode switch to the waveform, per channel: the FIRST switch since the track started (left), a LATER one (right)",
     ("dark",)),
    ("mode-switch-mixed-themes", "mode-switch-mixed@dark", "mode-switch-mixed@light",
     "Mode switch to the waveform in the mixed view (one lane), a later switch: dark (left), light (right)", ("dark",)),
    ("mode-switch", "mode-switch-lane-back", "mode-switch-lane",
     "Visualization mode switch, both directions: waveform to spectrogram (left), spectrogram to waveform (right)",
     ("dark",)),
    ("library-expand-collapse", "library-expand", "library-collapse",
     "Library tree: an artist expanded (left) and collapsed again (right), with the keyboard", ("dark", "light")),
    ("search", "search-open", "search-close", "Search palette: opening (left) and closing (right), separate recordings",
     ("light", "dark")),
]
# More images of one recording, for another region: name -> [(suffix, (x, y, w, h) or None for the whole recording)].
EXTRA_VIEWS = {"first-play-from-idle": [("window", None)]}
# Per-element ink in a strip's graph and caption (label, fnmatch pattern on the text item's name): each
# element's own rect, so a local dim is not averaged away by the rest of the region.
_FP_ITEMS = [("idle row", "any:*nowPlayingPanel/nowPlayingIdleRow"), ("cover", "any:*nowPlayingCoverArea/nowPlayingCover")]
NOW_PLAYING_ELEMENTS = [("title", "nowPlaying*Title*"), ("artist line", "nowPlaying*ArtistAlbum*"),
                        ("details line", "nowPlaying*Details*")]
# the large-font card (both sidebar widths) and the narrow one: the title beside the cover, then artist, album
# and details as lines of their own
NOW_PLAYING_LINES = [("title", "nowPlaying*Title*"), ("artist", "nowPlaying*Artist*"), ("album", "nowPlaying*Album*"),
                     ("details", "nowPlaying*Details*")]
ELEMENT_INK = {"art-to-no-art-24pt": NOW_PLAYING_LINES, "no-art-to-art-24pt": NOW_PLAYING_LINES,
               "art-to-no-art-narrow": NOW_PLAYING_LINES, "no-art-to-art-narrow": NOW_PLAYING_LINES,
               "track-change-new-album": NOW_PLAYING_ELEMENTS, "track-change-same-album": NOW_PLAYING_ELEMENTS, "art-to-no-art": NOW_PLAYING_ELEMENTS,
               "no-art-to-art": NOW_PLAYING_ELEMENTS, "track-change-metadata": NOW_PLAYING_ELEMENTS,
               # (the compact card's lines: title, artist, album, details)
               # (the large-font card's lines: the title beside the cover, then artist, album and details)
               "track-change-metadata-narrow24": [("title", "nowPlaying*Title*"), ("artist", "nowPlaying*Artist*"),
                                                  ("album", "nowPlaying*Album*"), ("details", "nowPlaying*Details*")],
               "track-change-metadata-24pt": [("title", "nowPlaying*Title*"), ("artist", "nowPlaying*Artist*"),
                                              ("album", "nowPlaying*Album*"), ("details", "nowPlaying*Details*")],
               "first-play-from-idle": NOW_PLAYING_ELEMENTS + _FP_ITEMS, "first-play-no-art": NOW_PLAYING_ELEMENTS + _FP_ITEMS,
               "stop-to-idle": NOW_PLAYING_ELEMENTS + _FP_ITEMS,

               "stop-to-idle-narrow24": NOW_PLAYING_LINES + _FP_ITEMS, "title-change-narrow24": NOW_PLAYING_LINES, "queue-end-to-idle": NOW_PLAYING_ELEMENTS + _FP_ITEMS}
# Strips whose crop holds the playing position (the playhead, its time text): their periodic single-frame
# updates are masked out of the transitions (motion_strip.analyse). Elsewhere a single-frame change is a step.
PERIODIC_UPDATES = {"art-to-no-art-24pt", "no-art-to-art-24pt", "art-to-no-art", "no-art-to-art", "seek-tooltip", "visualization-mode-switch", "visualization-mode-switch-back",
                    "vi-autoscroll"}
# The perceived-at-real-speed rows of a strip: `rect` (x, y, w, h in the recording's image) for small parts of
# a large recording, `segment` "last" to show the last transition rather than the longest.
# The top lane of the plot pane (below the mode switch's plate, left of the meter), and a band through its centre:
# for the right half of the pane (the clicked switch's recording) and for the whole pane
# (the band: a window about 300x80 px in the lane's middle, so its perceived tiles stay readable)
_LANE_R, _LANE_MID_R = "spectrogramPane:0.5,0.12,0.42,0.36", "spectrogramPane:0.6,0.16,0.24,0.18"
_LANE_FULL, _LANE_MID_FULL = "spectrogramPane:0.02,0.12,0.9,0.36", "spectrogramPane:0.38,0.16,0.24,0.18"
# an eye following the sliding library header, simulated in the perceived rows
# (the trace lists an item when it changes, or when it is text shown: a library row's title is always there)
_HEADER_PURSUIT = ("the library's first rows", "*libraryRowContent/libraryRowTitle")
_HEADER_TRAVEL = ("the library's rows", "*libraryRowContent/libraryRowTitle*")  # 0 px since the stand-in
MOTION_FOCUS = {
    # the perceived rows follow the art: a box over its right edge before (x 1299) and after (x 1127)
    "albumart-info-toggle": dict(rect=(1000, 380, 380, 240), rows="all", b_lines=1, pursuit=("the art", "*albumArtImage")),
    # visualization interaction: the perceived rows show the plot that moves (a 380 px stretch of its upper lane), not
    # the plates that fade beside it (recording coordinates: its crop starts at the transport's top)
    **{n: dict(rect=(741, 518, 380, 215), b_lines=1, band=360)
       for n in ("vi-follow-waveform", "vi-follow-spectrogram", "vi-autoscroll", "vi-seek-plot", "vi-seek-bar")},
    # (a pan's: also whether the crosshair is at one x in every lane of every frame)
    **{n: dict(rect=(741, 518, 380, 215), b_lines=1, band=360, lanes="spectrogramPane")
       for n in ("vi-pan-waveform", "vi-pan-spectrogram")},
    # the queue's first rows where the playing row moves (rows 1-3, the number and title columns); the first scan:
    # the status bar's right half (the scan label arriving and leaving)
    "queue-now-move": dict(rect="mainQueuePane:0,0.06,0.55,0.18", rows="all", b_lines=1),
    "first-scan": dict(rect="statusBar:0.4,0,0.6,1", rows="all", b_lines=1),
    "scan-large": dict(rect="statusBar:0.4,0,0.6,1", rows="all", b_lines=1),
    "scan-injected": dict(rect="statusBar:0.4,0,0.6,1", rows="all", b_lines=1),
    "mute-toggle": dict(rows="all", b_lines=1),
    "seek-drag-box": dict(rows="all", b_lines=1),
    # the field and the first rows under it, where each keystroke's result shows (the perceived rows); one 16.7 ms
    # line, so the key frames, which have the moving footer, are not thumbnails
    "search-typing": dict(rect=(402, 107, 300, 260), b_lines=1, band=400, max_frames=6),
    # the top-right corner; perceived rows for the reveal and the hide
    "viewer-strip-show-fullscreen": dict(rect=(1700, 0, 300, 110), rows="all", b_lines=1, max_frames=6),
    "viewer-autohide-fullscreen": dict(rect=(1700, 0, 300, 110), segment="last", rows="all", b_lines=1, zoom_graphs=True, band=360, max_frames=6),
    # the cover's foot and the text block; and the cover itself, mid-fade (recording coordinates)
    "track-change-new-album": dict(rect=(0, 300, 300, 140), extra=[("the cover", (20, 20, 320, 280))], b_lines=1, band=420),
    "track-change-same-album": dict(rect=(0, 300, 300, 140)),
    # the halves of the pairs (STRIP_PAIRS), 777 px wide: a smaller key-frame band and one 16.7 ms line
    # (the lane's drawn content as a blink when it empties; a perceived row through the lane's centre)
    "mode-switch-lane": dict(b_lines=1, band=360, plot=_LANE_R, extra=[("the lane's centre", _LANE_MID_R)]),
    "mode-switch-lane-first": dict(b_lines=1, band=360, plot=_LANE_R, extra=[("the lane's centre", _LANE_MID_R)]),
    "mode-switch-lane-back": dict(b_lines=1, band=360, plot=_LANE_R, extra=[("the lane's centre", _LANE_MID_R)]),
    "visualization-mode-switch": dict(plot=_LANE_FULL, extra=[("the lane's centre", _LANE_MID_FULL)]),
    "mode-switch-5.1": dict(plot="spectrogramPane:0.02,0.03,0.9,0.94", extra=[("the lanes' centre", _LANE_MID_FULL)]),
    "mode-switch-mixed": dict(plot="spectrogramPane:0.02,0.06,0.9,0.9", extra=[("the lane's centre", _LANE_MID_FULL)]),
    "mode-switch-mixed-back": dict(plot="spectrogramPane:0.02,0.06,0.9,0.9", extra=[("the lane's centre", _LANE_MID_FULL)]),
    "mode-switch-mixed-first": dict(plot="spectrogramPane:0.02,0.06,0.9,0.9", extra=[("the lane's centre", _LANE_MID_FULL)]),
    "visualization-mode-switch-back": dict(plot=_LANE_FULL, extra=[("the lane's centre", _LANE_MID_FULL)]),
    "library-expand": dict(b_lines=1, band=360), "library-collapse": dict(b_lines=1, band=360),
    "search-open": dict(b_lines=1, band=360), "search-close": dict(b_lines=1, band=360),
    # the album art viewer's strip and corner: its reveal and its hide, zoomed graphs
    "albumart-autohide-fullscreen": dict(rect=(1500, 0, 500, 110), segment="last", rows="all", b_lines=1, zoom_graphs=True,
                                         band=360, max_frames=6),
    "seek-tooltip": dict(rows="all", b_lines=1),  # the time box fading in on hover, and out on leave
    # the column: the sidebar as the app lays it out, from its top down 510 px
    "first-play-from-idle": dict(rect="sidebarPane:0,0,1,0.567", max_h=180, b_lines=1, band=400, pursuit=_HEADER_PURSUIT),
    "plates-reveal": dict(b_lines=1, band=360),
    "stop-to-idle": dict(rect=(0, 0, None, None), max_h=200, b_lines=1, band=360),
    "stop-to-idle-narrow24": dict(rect=(0, 0, None, None), max_h=200, b_lines=1, band=360),
    "title-change-narrow24": dict(rect=(0, 0, None, None), max_h=200, b_lines=1, band=360,
                                  travel=("the library header", "*libraryPane/libraryHeader/regionHeaderTitle")),
    "queue-end-to-idle": dict(rect=(0, 0, None, None), max_h=200, b_lines=1, band=360),
    "first-play-no-art": dict(rect="sidebarPane:0,0,1,0.567", max_h=180, b_lines=1, band=400, pursuit=_HEADER_PURSUIT,
                              travel=_HEADER_TRAVEL),
    # the sidebar (its own width, read from the app: a fixed 372 px crop took 12 px of the moving plot), from its
    # top: the cover's place and the text block
    "art-to-no-art": dict(rect=(0, 0, None, 560), max_h=200, b_lines=1, band=360, pursuit=_HEADER_PURSUIT, travel=_HEADER_TRAVEL),
    "track-change-metadata": dict(rect=(0, 0, None, None), max_h=200, b_lines=1, band=360,
                                  travel=("the library header", "*libraryPane/libraryHeader/regionHeaderTitle")),
    "track-change-metadata-24pt": dict(rect=(0, 0, None, None), max_h=200, b_lines=1, band=360,
                                       travel=("the library header", "*libraryPane/libraryHeader/regionHeaderTitle")),
    "track-change-metadata-narrow24": dict(rect=(0, 0, None, None), max_h=200, b_lines=1, band=360,
                                           travel=("the library header", "*libraryPane/libraryHeader/regionHeaderTitle")),
    "art-to-no-art-narrow": dict(rect=(0, 0, None, None), max_h=200, b_lines=1, band=360, travel=_HEADER_TRAVEL),
    "no-art-to-art-narrow": dict(rect=(0, 0, None, None), max_h=200, b_lines=1, band=360, travel=_HEADER_TRAVEL),
    "art-to-no-art-24pt": dict(rect=(0, 0, None, 700), max_h=200, b_lines=1, band=360, pursuit=_HEADER_PURSUIT, travel=_HEADER_TRAVEL),
    "no-art-to-art-24pt": dict(rect=(0, 0, None, 700), max_h=200, b_lines=1, band=360, pursuit=_HEADER_PURSUIT, travel=_HEADER_TRAVEL),
    "no-art-to-art": dict(rect=(0, 0, None, 560), max_h=200, b_lines=1, band=360, pursuit=_HEADER_PURSUIT, travel=_HEADER_TRAVEL),
}

# ---- layout checker (layout.py): allowlists, fnmatch patterns against an item's objectName/id, its
# "path/name", its QML file or its type.
# Text designed to elide (long titles in list cells); an elision anywhere else is reported.
ELIDE_OK = [
    "queueCell_*", "libraryRowTitle", "globalSearchResultTitle", "globalSearchResultContext",
    "nowPlayingTitle", "nowPlayingArtistAlbum", "nowPlayingDetails", "nowPlayingNarrow*",  # full text in a tooltip
    "libraryRootPath", "albumArtFolderPath",  # paths elide in the middle by design
    "libraryRootPathField", "confirmDialogPath",  # PathLabel: read-only paths, middle-elided, full path in a tooltip (M4)
    "diagnosticsLogPath",  # the same PathLabel in Diagnostics (seen at 24 pt)
    # The tag editor's table: fixed-width text columns elide long values; the number columns (#, Year,
    # Disc: trackNo, year, discNo) stay checked.
    *[f"*tagEditorRow/Label(col={c})" for c in ("title", "artist", "album", "albumArtist", "genre", "fileName",
                                                "formatKind", "comment", "status")],
    # (N5 names the cells tagEditorCell_<column> and fits the file columns to the table instead of scrolling)
    *[f"tagEditorCell_{c}" for c in ("title", "artist", "album", "albumArtist", "genre", "fileName", "directory",
                                     "formatKind", "comment", "status")],
]
# Items whose layout is not a defect by design (whole findings dropped).
LAYOUT_IGNORE = []
# Text whose contrast is not judged here (e.g. drawn over images or plots on purpose).
CONTRAST_SKIP = []


# ---- What a state must show, checked before its grab (step "expect", EXPECT above): (area, state) -> check,
# in every session that has the state. A state that does not show what it claims fails the run.
_LOOKS = {"listening-0": "listening", "listening-half": "listening", "listening-almost": "listening",
          "scrobbled": "scrobbled", "queued-offline": "queued", "failed": "failed", "paused": "paused"}
EXPECT_STATES = {
    ("library", "expanded-playing"): "now-row", ("library", "follows-playing"): "holds-now",
    ("library", "collapsed-holding-playing"): "collapsed-holds-now",
    ("main", "playing-waveform-levelmeter"): "waveform-meter", ("main", "playing-scale-levelmeter"): "spectrogram-meter",
    ("main", "playing-levelmeter"): "spectrogram-meter",
    ("nowplaying", "no-cover"): "no-cover", ("main", "playing-no-cover"): "no-cover",
    ("nowplaying", "missing-tags"): "no-cover", ("main", "playing-missing-tags"): "no-cover",
    ("search", "empty"): "search-empty", ("search", "results"): "search-results",
    ("main", "first-run-empty-library"): "first-run", ("main", "first-run-defaults"): "first-run", ("main", "library-idle"): "idle",
    ("plot", "5.1-levelmeter-lanes"): "lanes:6", ("plot", "5.1-hover"): "lanes:6",
    ("main", "playing-spectrogram-centered"): "playing",
    **{("statusbar", f"lastfm-{k}"): f"look:{v}" for k, v in _LOOKS.items()},
    **{("viewer", n): "viewer-spectrogram" for n in (
        "spectrogram-windowed", "spectrogram-windowed-hover", "spectrogram-windowed-zoomed", "spectrogram-windowed-meter",
        "spectrogram-fullscreen", "spectrogram-fullscreen-title-strip", "spectrogram-fullscreen-hover",
        "spectrogram-fullscreen-zoomed")},
    **{("viewer", f"albumart-{m}{sfx}"): "viewer-albumart" for m in ("windowed", "fullscreen") for sfx in ("", "-info")},
}
_seen = set()


def _with_expect(line):
    chk = EXPECT_STATES.get((line[1], line[2]))
    if not chk or f"expect {chk}" in line[3]:
        return line
    _seen.add((line[1], line[2]))
    return (line[0], line[1], line[2], (line[3] + " | " if line[3] else "") + f"expect {chk}", line[4])


STATES = [_with_expect(l) for l in STATES]


# (dialogs and viewers) states added after existing ones, then a dlgfacts line after each dialog state
def _insert_after(session, state, lines):
    i = next((k for k, l in enumerate(STATES) if l[0] == session and l[2] == state), None)
    if i is None:
        raise LookupError(f"_insert_after: no state {session}/{state}")
    STATES[i + 1:i + 1] = lines


_insert_after("dialogs", "about-default-focus", [
    ("dialogs", "dialog", "about-close-hover", "hover aboutCloseButton 0.6", "About: its Close button hovered"),
    ("dialogs", "dialog", "about-close-pressed", "press aboutCloseButton | wait 0.3", "About: its Close button pressed (held)"),
    ("dialogs", "_", "_about-unpress", "release aboutCopyVersionButton@0.5,0.5 | wait 0.3 | away", "")])
# footer buttons and Configure's sidebar rows hovered (the pointer then leaves, so later shots have no hover).
# Kept out of the default dialogs pack (its image set is fixed): shot only with SHOT_DIALOG_HOVER=1
DIALOG_HOVER = os.environ.get("SHOT_DIALOG_HOVER") == "1"
if DIALOG_HOVER:
  _insert_after("dialogs", "preferences-general", [
    ("dialogs", "dialog", "preferences-footer-hover", "hover preferencesCancelButton 0.6",
     "Configure (General page): its footer's Cancel hovered"),
    ("dialogs", "dialog", "preferences-sidebar-hover", "hover preferencesPage_0 0.6",
     "Configure (General page): the Library row of its sidebar hovered (not the current page)"),
    ("dialogs", "_", "_preferences-hover-away", "away | wait 0.4", "")])
  _insert_after("dialogs", "tag-editor", [
    ("dialogs", "dialog", "tag-editor-footer-hover", "hover tagEditorCancelButton 0.6", "Edit Tags: its footer's Cancel hovered"),
    ("dialogs", "_", "_tag-editor-hover-away", "away | wait 0.4", "")])
# dialog controls hovered and pressed (released off them, so nothing acts)
_insert_after("dialogs", "itunes-results", [
    ("dialogs", "dialog", "itunes-apply-hover", "hover itunesArtworkApplyButton 0.6", "Replace from iTunes: a result's Apply hovered"),
    ("dialogs", "dialog", "itunes-preview-pressed", "press itunesArtworkPreviewButton | wait 0.3",
     "Replace from iTunes: a result's Preview pressed (held)"),
    ("dialogs", "_", "_itunes-unpress", "release itunesArtworkResultsHeading | wait 0.3 | away", "")])
_insert_after("dialogs", "tag-editor-modified", [
    ("dialogs", "dialog", "tag-editor-revert-hover", "hover tagEditorRevert_title 1.0", "Edit Tags: the Title field's revert glyph hovered"),
    ("dialogs", "dialog", "tag-editor-revert-pressed", "press tagEditorRevert_title | wait 0.3",
     "Edit Tags: the Title field's revert glyph pressed (held)"),
    ("dialogs", "_", "_revert-unpress", "release tagEditorLabel_title | wait 0.3", ""),
    ("dialogs", "dialog", "tag-editor-column-revert-hover", "hover tagEditorColumnRevert_title 1.0",
     "Edit Tags: the Title column's revert glyph hovered")])
_insert_after("statusbar", "recent-errors", [
    ("statusbar", "dialog", "recent-errors-link-hover", "hover errorHistoryActionButton 0.6", "Recent Issues: an action link hovered"),
    ("statusbar", "dialog", "recent-errors-link-pressed", "press errorHistoryActionButton | wait 0.3",
     "Recent Issues: an action link pressed (held)"),
    ("statusbar", "_", "_link-unpress", "release errorHistoryTitle | wait 0.3", ""),
    ("statusbar", "dialog", "recent-errors-link-focus", "away | key Tab | wait 0.4",
     "Recent Issues: keyboard focus on the next link after a real Tab (its ring)")])
_insert_after("dialogs", "preferences-lastfm", [
    ("dialogs", "dialog", "preferences-lastfm-account", "macro lastfmaccount | away",
     "Configure, Last.fm: an account connected (a stand-in on the page's bridge: no network, no credentials)")])
_insert_after("dialogs", "tag-editor-one-selected", [
    ("dialogs", "dialog", "tag-editor-editing", "click tagEditorField_title | wait 0.3 | window tagEditorDialog | key End | "
     "type \\s(Live) | wait 0.4 | window ", "Edit Tags: the Title field being edited (clicked, then \u201c (Live)\u201d typed with real keys)")])
_insert_after("dialogs", "itunes-searching-spinner", [
    # the real search failing, recorded in the dialog's own window: from the injected searching state, the
    # app's own search through the refusing proxy, to its failed state
    ("dialogs", "motion", "itunes-search-fails", "strip 1.6 0.1 - dialog=Replace :: macro itunesfail",
     "Replace from iTunes: a real search failing (the local proxy refuses it): searching, then the failed state")])
_insert_after("dialogs", "itunes-search-fails", [
    ("dialogs", "dialog", "itunes-error", "macro itunesfail",
     "Replace from iTunes: a real failed search (the app's application proxy 127.0.0.1:9 refuses every request; "
     "nothing leaves the machine): the app's own failed state")])
_insert_after("viewers", "albumart-windowed", [
    ("viewers", "viewer", "albumart-windowed-corner-hover", "move xy:800,500 | wait 0.4 | hover albumArtPopupInfoButton 1.3",
     "Album art viewer, windowed: the corner plate's info button hovered (its tooltip)"),
    ("viewers", "viewer", "albumart-windowed-corner-focus", "move xy:800,500 | wait 0.3 | focus albumArtPopupCloseButton | wait 0.4",
     "Album art viewer, windowed: the corner plate's close button with keyboard focus")])
_insert_after("viewers", "albumart-windowed-corner-focus", [
    # no pointer or focus residue (F12's Close tooltip): the viewer closed and opened again, the pointer away
    ("viewers", "_", "_corner-reset", "away | js viewerController.closeAlbumArtViewer() | settle 1 | "
     "js viewerController.openAlbumArtViewer() | settle 2 | away | wait 0.4", ""),
    ("viewers", "motion", "albumart-info-toggle", "strip 1.8 0.1 - :: js viewerController.toggleAlbumArtInfoVisible(function(){}) ; "
     "wait 0.9 ; js viewerController.toggleAlbumArtInfoVisible(function(){})",
     "Album art viewer, windowed: its info column shown, then hidden 0.9 s later (the art re-centres)")])
_insert_after("viewers", "spectrogram-fullscreen-hover", [
    ("viewers", "viewer", "spectrogram-fullscreen-close-hover",
     "window spectrogramFullscreenWindow | macro fsitem hover spectrogramFullscreenCloseButton spectrogram",
     "Spectrogram viewer, fullscreen: its strip's Close button hovered (tooltip)"),
    ("viewers", "viewer", "spectrogram-fullscreen-close-focus",
     "window spectrogramFullscreenWindow | macro fsitem reset spectrogramFullscreenViewFocus spectrogram | "
     "macro fsitem focus spectrogramFullscreenCloseButton spectrogram",
     "Spectrogram viewer, fullscreen: its strip's Close button with keyboard focus"),
    ("viewers", "_", "_spec-fs-reset", "macro fsitem reset spectrogramFullscreenViewFocus spectrogram", "")])
_insert_after("viewers", "albumart-fullscreen", [
    ("viewers", "viewer", "albumart-fullscreen-strip-hover", "window albumArtFullscreenWindow | macro fsitem hover albumArtFullscreenInfoButton",
     "Album art viewer, fullscreen: the strip's info button hovered (its tooltip)"),
    ("viewers", "viewer", "albumart-fullscreen-strip-focus", "window albumArtFullscreenWindow | macro fsitem reset albumArtFullscreenViewFocus | "
     "macro fsitem focus albumArtFullscreenInfoButton",
     "Album art viewer, fullscreen: the strip's info button with keyboard focus"),
    ("viewers", "_", "_fs-reset", "macro fsitem reset albumArtFullscreenViewFocus", "")])
_insert_after("viewers", "albumart-windowed-corner-focus", [
    ("viewers", "viewer", "albumart-windowed-corner-pressed", "move xy:800,500 | wait 0.3 | press albumArtPopupInfoButton | wait 0.3",
     "Album art viewer, windowed: the corner plate's info button pressed (held)"),
    ("viewers", "_", "_corner-unpress", "dmove xy:800,500 | release xy:800,500 | wait 0.3", "")])
for _sess, _after in (("dialogs", "_play"), ("viewers", "_play"), ("bigfont", "library-idle")):
    _insert_after(_sess, _after, [(_sess, "_", "_screen", "macro screenfacts", "")])
_insert_after("narrow", "no-art-to-art-narrow", [
    ("narrow", "viewer", "albumart-windowed-info", "settle 1 | js root.uiBridge.setViewerFullscreenMode(0) | "
     "js viewerController.openAlbumArtViewer() | settle 2 | "
     "js viewerController.toggleAlbumArtInfoVisible(function(){}) | wait 0.6",
     "Narrow window (1000x720): the windowed album art viewer with its info column"),
    ("narrow", "_", "_albumart-close", "esc | js viewerController.closeAlbumArtViewer() | settle 1", "")])
_insert_after("motion-ui", "preferences-open", [
    ("motion-ui", "motion", "preferences-page-switch", "settle 0.6 | strip 0.8 0.1 - dialog=Configure :: js preferencesDialog.openPage(1)",
     "Configure: switching from the first page to the second")])

# Edit › Undo and Redo of a queue change (#698): a row removed with Delete, then undone
_insert_after("menus", "help-playing", [
    ("menus", "_", "_queue-remove", "esc | away | click @plainQueueRow@0.5,0.5 | wait 0.3 | key Delete | wait 0.8 | away", ""),
    ("menus", "menu", "edit-undo", "menu 1", "Edit menu after a queue row was removed with Delete: Undo Remove from Queue"),
    ("menus", "_", "_queue-undo", "esc | js undoQueueChangeAction.trigger() | wait 0.8 | away", ""),
    ("menus", "menu", "edit-redo", "menu 1", "Edit menu after that removal was undone: Undo and Redo (Ctrl+Shift+Z)"),
    ("menus", "_", "_queue-redo-close", "esc | away", "")])
# Configure's library folder row with its menu, from the keyboard (#680)
_insert_after("roots", "preferences-library-root-selected", [
    ("roots", "dialog", "preferences-library-root-menu", "window preferencesDialog | key Menu | wait 0.7 | window | macro dialogmenu",
     "Configure, Library: the selected folder's menu, from the keyboard (Menu key)"),
    ("roots", "_", "_root-menu-close", "window preferencesDialog | key Escape | wait 0.4 | window ", "")])
# the issues button with keyboard focus (#631), and the reason of a stopped engine (#621) in its tooltip
_insert_after("statusbar", "issues-seen", [
    ("statusbar", "statusbar", "issues-focus", "focus statusBarErrorsButton | wait 0.3", "Status bar: the issues button with keyboard focus"),
    ("statusbar", "_", "_issues-unfocus", "unfocus", "")])
_insert_after("statusbar", "backend-stopped", [
    ("statusbar", "statusbar", "backend-stopped-hover", "hover statusBarBackendStoppedLabel 1.4",
     "The stopped engine's label hovered: the reason in its tooltip (#621; the reason is injected)"),
    ("statusbar", "_", "_backend-stopped-away", "away | wait 0.3", "")])
# a viewer button held down from the keyboard (#674): Space on the focused info button
_insert_after("viewers", "albumart-fullscreen-strip-focus", [
    ("viewers", "viewer", "albumart-fullscreen-strip-key-pressed",
     "window albumArtFullscreenWindow | macro fsitem focus albumArtFullscreenInfoButton | keydown Space | wait 0.2",
     "Album art viewer, fullscreen: the focused info button held down with Space"),
    # (the focus moved off first, which cancels the press, so the release does not toggle the info column)
    ("viewers", "_", "_fs-keyup", "window albumArtFullscreenWindow | macro fsitem reset albumArtFullscreenViewFocus | "
     "keyup Space | wait 0.3", "")])
# a search result's menu from the keyboard (#632): the Menu key on the selected result
_insert_after("menus", "context-search-result", [
    ("menus", "menu", "context-search-result-keyboard", "esc 1 | away | key Down | kkey Menu | wait 0.9",
     "Search result context menu from the keyboard (a real Menu key on the selected result)")])
# Configure Keyboard Shortcuts (#700, #701): the list filtered from its search field (real keys), cleared again
_insert_after("dialogs", "keyboard-shortcuts-default-focus", [
    ("dialogs", "dialog", "keyboard-shortcuts-filter",
     "click keyboardShortcutsSearchField | wait 0.3 | window keyboardShortcutsDialog | type play | wait 0.6 | window | away",
     "Configure Keyboard Shortcuts: the list filtered by \u201cplay\u201d, typed in its search field"),
    # (the pointer out of the dialog too: left on the search field, its tooltip covered the columns in the later
    # states, which are compared with the one at rest)
    ("dialogs", "_", "_shortcuts-unfilter",
     "window keyboardShortcutsDialog | leave | key Backspace | key Backspace | key Backspace | key Backspace | wait 0.4 | window ", "")])
_insert_after("bigfont", "quit-save-failed", [  # (the next state's Escape closes it)
    ("bigfont", "dialog", "keyboard-shortcuts", "esc | macro opendialog keyboardShortcutsDialog | settle 1.2 | away",
     "Large font: Configure Keyboard Shortcuts")])
# Visualization interaction (the viz-interaction scope): a zoomed waveform and centred spectrogram, the seek bar's
# band marking the view (#656), a middle-drag pan that leaves the view where it is put while playback goes on
# (#654, #657), Follow Playhead (F) bringing it back, seeks from the plot (right click: the view stays) and from the
# seek bar (the view comes back), and middle-click autoscroll by the pointer's distance, faster at the window's
# edge and going on with the pointer outside the window (#659, #662). Whole-window grabs; the focus sheets crop
# them. The probe's middle button (mpress, mdmove, mrelease, mclick) and `leave` go to the window as the
# compositor's events would; the pointer's cursor is not in a grab.
_VW = "mainWaveformSurface"
_VS = "spectrogramPaneItem"
_VI_CROP = "transportBar+spectrogramPane"  # (the strips: the seek bar above the pane)


def _mdrag(target, x0, x1, y=0.5, n=6, dt=0.04, sep=" | "):
    """middle-drag steps on `target` from x0 to x1 (fractions of it), in n moves dt s apart, then a release"""
    xs = [x0 + (x1 - x0) * (k + 1) / n for k in range(n)]
    return sep.join([f"mpress {target}@{x0},{y}", "wait 0.12"] + [f"mdmove {target}@{x:.3f},{y} {dt}" for x in xs]
                    + [f"mrelease {target}@{x1},{y}"])


STATES += [
    ("viz-interaction", "_", "_play", f"queue | play Small_Hours 60 4 | js viewerController.setVisualizationMode(1) | ready 3 | away", ""),
    ("viz-interaction", "main", "vi-waveform-zoomed",
     f"wheel {_VW}@0.5,0.4 120 6 | wait 1.0 | away | wait 0.4",
     "Waveform zoomed in 6 notches, following the playhead: the seek bar's band marks the part of the track shown"),
    ("viz-interaction", "main", "vi-waveform-panned", _mdrag(_VW, 0.2, 0.8) + " | wait 0.4 | away | wait 0.3 | until followPlayheadAction.enabled",
     "A middle-drag of 60 % of the plot to the right: the view moved back in the track, detached from the playhead "
     "(which is now past its right edge); the band on the seek bar moved with it"),
    ("viz-interaction", "main", "vi-waveform-panned-held", "wait 3",
     "The same 3 s later, still playing: the moved view stays where it was put"),
    ("viz-interaction", "motion", "vi-follow-waveform", f"strip 1.2 0.1 {_VI_CROP} :: key F",
     "View › Follow Playhead (F) after a middle-drag: the waveform's view back on the playhead, the band with it"),
    ("viz-interaction", "main", "vi-waveform-followed", "wait 0.6 | away | until !followPlayheadAction.enabled", "After Follow Playhead (F): the view on the playhead again"),
    ("viz-interaction", "motion", "vi-pan-waveform",
     f"strip 1.6 0.1 {_VI_CROP} :: " + _mdrag(_VW, 0.7, 0.3, n=10, dt=0.05, sep=" ; "),
     "A middle-drag of 40 % of the zoomed waveform to the left: the time grabbed stays under the pointer, the band follows"),
    ("viz-interaction", "motion", "vi-seek-plot", f"wait 0.3 | strip 1.2 0.1 {_VI_CROP} :: rclick {_VW}@0.25,0.5 0",
     "A right click on the moved (detached) waveform seeks there: the view stays, the playhead comes into it"),
    ("viz-interaction", "motion", "vi-seek-bar", f"wait 0.6 | strip 1.4 0.1 {_VI_CROP} :: click seekSlider@0.7,0.5",
     "A click on the seek bar while the view is moved: the playhead jumps there and the view follows it again"),
    # (autoscroll from early in the track and deep in: at 6 notches its fastest reaches the track's end in under a
    # second, and the end would hide what happens after)
    ("viz-interaction", "_", "_autoscroll-from",
     f"wait 0.4 | click seekSlider@0.08,0.5 | wait 0.5 | wheel {_VW}@0.5,0.4 120 8 | wait 0.8 | away", ""),
    ("viz-interaction", "main", "vi-autoscroll-near", f"wait 0.2 | mclick {_VW}@0.3,0.5 | move {_VW}@0.5,0.5 | wait 1.2",
     "Autoscroll: the pointer a little right of the middle click: the view moving slowly forward"),
    ("viz-interaction", "main", "vi-autoscroll-edge", "move xy:1598,760 | wait 0.8",
     "Autoscroll with the pointer at the window's right edge: much faster, the band racing along the seek bar"),
    ("viz-interaction", "main", "vi-autoscroll-outside", "leave | wait 0.8",
     "Autoscroll after the pointer left the window: going on at the speed it had where it was last seen"),
    ("viz-interaction", "main", "vi-autoscroll-stopped", f"move {_VW}@0.5,0.5 | mclick {_VW}@0.5,0.5 | wait 0.8 | away",
     "Autoscroll stopped by a second middle click: the view stays where it got to"),
    ("viz-interaction", "_", "_autoscroll-stop", "key F | wait 0.6 | away", ""),
    ("viz-interaction", "motion", "vi-autoscroll",
     f"strip 3.4 0.1 {_VI_CROP} :: mclick {_VW}@0.3,0.5 ; move {_VW}@0.5,0.5 ; wait 0.9 ; move xy:1598,760 ; wait 0.9 ; "
     f"leave ; wait 0.9 ; move {_VW}@0.5,0.5 ; mclick {_VW}@0.5,0.5",
     "Autoscroll from a middle click: the pointer a little right of the click (slow), at the window's edge (fast), "
     "outside the window (going on), then a second middle click stopping it"),
    ("viz-interaction", "_", "_to-spectrogram",
     "wait 0.3 | key F | js resetZoomAction.trigger() | js viewerController.setVisualizationMode(0) | "
     "js root.uiBridge.setSpectrogramDisplayMode(1) | click seekSlider@0.3,0.5 | ready 3 | away", ""),
    ("viz-interaction", "main", "vi-spectrogram-zoomed", f"wheel {_VS}@0.5,0.4 120 6 | wait 1.5 | away | wait 0.4",
     "The centred spectrogram zoomed in 6 notches, following the playhead: the band on the seek bar"),
    ("viz-interaction", "main", "vi-spectrogram-panned", _mdrag(_VS, 0.2, 0.8) + " | away | wait 0.15 | until followPlayheadAction.enabled",
     "A middle-drag of the centred spectrogram 60 % to the right, just released: the view moved back in the track "
     "while its columns are decoded where it is"),
    ("viz-interaction", "main", "vi-spectrogram-panned-decoded", "wait 2.5",
     "The same 2.5 s later, still playing: the moved view filled in and staying where it was put"),
    ("viz-interaction", "motion", "vi-pan-spectrogram",
     f"strip 2.4 0.1 {_VI_CROP} :: " + _mdrag(_VS, 0.7, 0.3, n=10, dt=0.05, sep=" ; ") + " ; wait 1.2",
     "A middle-drag of the zoomed centred spectrogram 40 % to the left, then 1.2 s as its columns come"),
    ("viz-interaction", "motion", "vi-follow-spectrogram", f"wait 0.4 | strip 1.2 0.1 {_VI_CROP} :: key F",
     "Follow Playhead (F) after a middle-drag of the centred spectrogram: back on the playhead"),
    ("viz-interaction", "main", "vi-rolling-zoomed", "wait 0.4 | until !followPlayheadAction.enabled | js root.uiBridge.setSpectrogramDisplayMode(0) | wait 2 | away",
     "The rolling spectrogram, still zoomed: its band on the seek bar"),
]

DLG_FACTS = {("dialogs", st) for st in (
    "about", "about-kde", "confirm-remove-library-root", "library-root-name-rename", "library-root-name-add", "diagnostics",
    "confirm-clear-diagnostics", "quit-save-failed", "tag-editor", "tag-editor-editing", "tag-editor-auto-number",
    "tag-editor-rename-preview", "tag-editor-status-details", "tag-editor-quit-wait", "tag-editor-close-confirm",
    "itunes-empty", "itunes-error", "itunes-results", "confirm-replace-cover", "about-components", "preferences-lastfm-account")} \
    | {("dialogs", f"preferences-{slug}") for _t, slug in PREF_PAGES} \
    | {("statusbar", "recent-errors"), ("menus", "empty"), ("menus", "results"), ("menus", "no-results"),
       ("bigfont", f"preferences-{PREF_PAGES[1][1]}-first"), ("bigfont", "confirm-clear-diagnostics"), ("bigfont", "tag-editor-close-confirm"), ("bigfont", "keyboard-shortcuts"),
       ("dialogs", "keyboard-shortcuts"), ("dialogs", "keyboard-shortcuts-filter"), ("dialogs", "keyboard-shortcuts-reassign"), ("roots", "preferences-library-three-roots-hover"), ("bigfont", "preferences-visualization"), ("bigfont", "itunes-results"), *[("bigfont", st) for st in (
           "about", "about-kde", "library-root-name-add", "library-root-name-rename", "confirm-remove-library-root",
           "confirm-clear-diagnostics", "quit-save-failed", "diagnostics", "recent-errors", "tag-editor", "tag-editor-auto-number",
           "tag-editor-rename-preview", "tag-editor-batch", "itunes-results-loading", "tag-editor-quit-wait", "tag-editor-status-details", "itunes-error", "itunes-empty",
           "confirm-replace-cover", "preferences-library", "preferences-general", "preferences-lastfm")], ("narrow", f"preferences-{PREF_PAGES[1][1]}"), ("narrow", "tag-editor")}
# the viewer's corner plate or strip as grabbed (shown, or auto-hidden after the pointer stopped): its fact
# for the sheets' labels, recorded as the state's last step
PLATE_FACTS = {("viewers", "albumart-windowed-info"): ("albumArtPopupControls", "corner plate"),
               ("viewers", "albumart-fullscreen-info"): ("albumArtFullscreenStrip", "strip"),
               ("viewers", "spectrogram-fullscreen-hover"): ("spectrogramFullscreenStrip", "strip"),
               ("bigfont", "albumart-windowed"): ("albumArtPopupControls", "corner plate")}
STATES = [(l[0], l[1], l[2], l[3] + f" | macro platefact {l[2]} {PLATE_FACTS[(l[0], l[2])][0]} {PLATE_FACTS[(l[0], l[2])][1]}", l[4])
          if (l[0], l[2]) in PLATE_FACTS and l[1] != "_" else l for l in STATES]
# button fills against their surface, measured in the shot just grabbed (fill-* facts)
FILL_FACTS = {
    ("viewers", "albumart-windowed"): [("albumArtPopupInfoButton", "rest")],
    ("viewers", "albumart-windowed-corner-hover"): [("albumArtPopupInfoButton", "hover")],
    ("viewers", "albumart-windowed-corner-pressed"): [("albumArtPopupInfoButton", "pressed")],
    ("viewers", "albumart-windowed-info"): [("albumArtPopupInfoButton", "checked")],
    ("viewers", "albumart-fullscreen-strip-hover"): [("albumArtFullscreenInfoButton", "hover")],
    ("viewers", "spectrogram-fullscreen-close-hover"): [("spectrogramFullscreenCloseButton", "hover")],
    ("dialogs", "about"): [("aboutCloseButton", "default"), ("aboutCopyVersionButton", "rest")],
    ("dialogs", "about-close-hover"): [("aboutCloseButton", "hover")],
    ("dialogs", "about-close-pressed"): [("aboutCloseButton", "pressed")],
    ("dialogs", "itunes-results"): [("itunesArtworkApplyButton", "rest")],
    ("dialogs", "itunes-apply-hover"): [("itunesArtworkApplyButton", "hover")],
    ("dialogs", "itunes-preview-pressed"): [("itunesArtworkPreviewButton", "pressed")],
    ("dialogs", "tag-editor-revert-hover"): [("tagEditorRevert_title", "hover")],
    ("dialogs", "tag-editor-revert-pressed"): [("tagEditorRevert_title", "pressed")],
    ("dialogs", "tag-editor-column-revert-hover"): [("tagEditorColumnRevert_title", "hover")],
    ("statusbar", "recent-errors"): [("errorHistoryActionButton", "rest")],
    ("statusbar", "recent-errors-link-hover"): [("errorHistoryActionButton", "hover")],
    ("statusbar", "recent-errors-link-pressed"): [("errorHistoryActionButton", "pressed")],
}
if DIALOG_HOVER:  # the hover states' fills, with their rest states (off by default)
    FILL_FACTS.update({
        ("dialogs", "preferences-general"): [("preferencesCancelButton", "rest"), ("preferencesPage_0", "rest")],
        ("dialogs", "preferences-footer-hover"): [("preferencesCancelButton", "hover")],
        ("dialogs", "preferences-sidebar-hover"): [("preferencesPage_0", "hover")],
        ("dialogs", "tag-editor"): [("tagEditorCancelButton", "rest")],
        ("dialogs", "tag-editor-footer-hover"): [("tagEditorCancelButton", "hover")]})
_missing_fill = set(FILL_FACTS) - {(l[0], l[2]) for l in STATES}
if _missing_fill:
    raise LookupError(f"FILL_FACTS names states that do not exist: {sorted(_missing_fill)}")
_out = []
for _l in STATES:
    _out.append(_l)
    if (_l[0], _l[2]) in FILL_FACTS and _l[1] != "_":
        for _it, _k in FILL_FACTS[(_l[0], _l[2])]:
            _out.append((_l[0], "_", f"_fill-{_l[2]}-{_it}", f"macro fillfact {_l[1]}/{_l[2]} {_it} {_k}", ""))
STATES = _out
_out = []
for _l in STATES:
    _out.append(_l)
    if (_l[0], _l[2]) in DLG_FACTS and _l[1] != "_":
        _out.append((_l[0], "_", f"_dlgfacts-{_l[2]}", f"macro dlgfacts {_l[1]}/{_l[2]}", ""))
_missing = DLG_FACTS - {(l[0], l[2]) for l in STATES}
if _missing:
    raise LookupError(f"DLG_FACTS names states that do not exist: {sorted(_missing)}")
STATES = _out
if os.environ.get("SHOT_KWINPROBE") == "1":  # diagnostic: m_kwinprobe, at the end of the dialogs session
    STATES.append(("dialogs", "_", "_kwinprobe", "esc 2 | away | macro kwinprobe", ""))
_unused = sorted(k for k in EXPECT_STATES if k not in _seen and not any(l[1] == k[0] and l[2] == k[1] for l in STATES))
if _unused:  # loudly: a check for a state that no longer exists checks nothing
    raise LookupError(f"EXPECT_STATES names states that do not exist: {_unused}")
