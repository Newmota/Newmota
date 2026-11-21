#!/usr/bin/env python3
"""Configure a user build, export it to user_config/, and build it (`make config`).

Interactive:
  make config                         start from a checked-in environment
  make config ENV=esp32c3-4m          start from that environment, or a user configuration

Scripted (no terminal needed):
  uv run python tools/menuconfig.py --env esp32c3-4m --enable USE_BLE --name my-c3 --write
  uv run python tools/menuconfig.py --env my-c3 --build
  uv run python tools/menuconfig.py --env esp32c3-4m --list-features
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from dataclasses import dataclass

from newmota.catalog import CatalogError, load_environments
from newmota.configurator import (
    PROFILES,
    ConfigSession,
    application_environments,
    bases,
    chips,
    default_base,
    grouped_features,
    languages,
    platforms,
    user_files,
)
from newmota.model import ROOT

TABS = ("Target", "Features", "Summary", "Help")
PLATFORM_NAMES = {
    "esp-idf": "ESP-IDF (Espressif ESP32 family)",
    "beken": "Beken (BK72xx)",
    "bouffalo": "Bouffalo Lab (BL60x, BL61x)",
    "amebad": "Realtek AmebaD (RTL872x)",
    "pico-sdk": "Raspberry Pi Pico (RP2040, RP2350)",
}
MARKERS = {
    "selected": "[*]",
    "default": "[=]",
    "implied": "[+]",
    "off": "[ ]",
    "disabled": "[ ]",
    "blocked": "[-]",
}
HELP = (
    "On Target pick the platform family, then the chip and environment to start from.",
    "Choose features on Features. Summary previews the file.",
    "w writes user_config/<name>.yaml; b writes it and builds it.",
    "",
    "Feature markers",
    "  [*] selected by you           [=] on in the base environment",
    "  [+] on because a selected feature needs it (cannot be turned off alone)",
    "  [ ] off                       [-] unavailable here; the detail line says why",
    "",
    "Keys",
    "  Left/Right, Tab   switch tab         Up/Down, PgUp/PgDn, Home/End   move",
    "  Space, Enter      toggle / change    /   filter features (Esc clears)",
    "  a / z             expand / collapse all groups",
    "  w                 write user_config/<name>.yaml",
    "  b                 write, then build (make build ENV=<name>)",
    "  q                 quit (asks before discarding changes)",
    "",
    "Turning a feature off also turns off the features you selected on top of it.",
    "Selecting one pulls in its dependencies; they stay out of the file.",
    "The name follows the chosen environment until you set one yourself.",
)


@dataclass
class Row:
    text: str
    style: str = "item"  # item, header, dim, error
    key: str | None = None
    action: str = ""


def detail_env(name):
    item = load_environments()[name]
    return [
        f"Environment: {item.name}  ({PLATFORM_NAMES.get(item.platform_directory, item.platform)})",
        f"Board: {item.board}  Chip: {item.chip}  Flash: {item.flash_kb}K",
        f"Profile: {item.profile}  Language: {item.language}  Safeboot: {item.safeboot or '-'}",
        f"Features set by the environment: {', '.join(item.features) or 'none'}",
    ]


def environment_label(item) -> str:
    return f"{item.name:<28} {item.board:<28} {item.flash_kb // 1024}M"


def build_command(name: str) -> tuple[list[str], dict[str, str]]:
    """The command `make build ENV=<name>` runs, with its environment."""

    env = {**os.environ, "PYTHONPATH": str(ROOT / "tools")}
    return [sys.executable, "-m", "newmota", "build", name], env


def run_build(name: str) -> int:
    command, env = build_command(name)
    print(f"$ make build ENV={name}", flush=True)
    return subprocess.run(command, cwd=ROOT, env=env, check=False).returncode


class Configurator:
    """Rows and details for each tab; no terminal access, so it can run headless."""

    def __init__(self, session: ConfigSession):
        self.session = session
        self.collapsed = {group for group, _ in grouped_features()}
        self.filter = ""
        self.saved = session.document()

    @property
    def dirty(self) -> bool:
        return self.session.document() != self.saved

    def target_rows(self) -> list[Row]:
        s = self.session
        origin = f"  (opened from {s.loaded_path.name})" if s.loaded_path else ""
        name = f"{s.name}  (follows the environment)" if s.auto_name else s.name
        platform = s.base.platform_directory
        return [
            Row(f"Platform family   {PLATFORM_NAMES.get(platform, platform)}", action="platform"),
            Row(f"Chip              {s.base.chip}", action="chip"),
            Row(f"Environment       {s.base.name}{origin}", action="base"),
            Row(f"Name              {name}", action="name"),
            Row(f"Profile           {s.profile}", action="profile"),
            Row(f"Language          {s.language}", action="language"),
            Row("", "dim", action="none"),
            Row("Open a user configuration...", action="open"),
            Row("Write and build", action="build"),
        ]

    def target_detail(self, row: Row) -> list[str]:
        s = self.session
        lines = {
            "platform": ["Choose the SDK family first; chips and environments follow it."],
            "chip": [f"Chips with an application environment on {s.base.platform_directory}."],
            "base": ["The checked-in environment this one inherits board and defaults from."],
            "name": [f"Becomes user_config/{s.name}.yaml and the target: make build ENV={s.name}"],
            "profile": ["Feature baseline: Compact, Standard or Large."],
            "language": ["Build-time web UI language (language/*.toml). Not a runtime switch."],
            "none": [""],
            "open": [f"{len(user_files())} file(s) in user_config/. Opening replaces this one."],
            "build": [f"Write user_config/{s.name}.yaml, then run make build ENV={s.name}."],
        }[row.action]
        return [*lines, "", *detail_env(s.base.name)]

    def feature_rows(self) -> list[Row]:
        s = self.session
        needle = self.filter.lower()
        rows: list[Row] = []
        for group, names in grouped_features():
            shown = [
                name
                for name in names
                if not needle or needle in name.lower() or needle in s.state(name).summary.lower()
            ]
            if not shown:
                continue
            opened = bool(needle) or group not in self.collapsed
            on = sum(1 for name in names if s.values.get(name))
            rows.append(
                Row(
                    f"{'v' if opened else '>'} {group}  ({on}/{len(names)} on)",
                    "header",
                    key=group,
                    action="group",
                )
            )
            if not opened:
                continue
            for name in shown:
                state = s.state(name)
                style = "dim" if state.kind == "blocked" else "item"
                rows.append(
                    Row(
                        f"  {MARKERS[state.kind]} {name:<34} {state.summary}",
                        style,
                        key=name,
                        action="feature",
                    )
                )
        return rows

    def feature_detail(self, row: Row) -> list[str]:
        if row.action == "group":
            return ["Enter opens or closes the group."]
        state = self.session.state(row.key)
        lines = [f"{state.name}: {state.summary}"]
        lines.append(
            {
                "selected": "Selected by you.",
                "default": "On in the base environment and profile.",
                "implied": "On because "
                + ", ".join(state.required_by or ("a selected feature",))
                + " needs it.",
                "disabled": "Turned off by you although the base enables it.",
                "off": "Off.",
                "blocked": "Unavailable: " + state.reason,
            }[state.kind]
        )
        if state.needs:
            lines.append("Needs: " + ", ".join(state.needs))
        if state.required_by and state.kind != "implied":
            lines.append("Needed by: " + ", ".join(state.required_by))
        return lines

    def summary_lines(self) -> list[str]:
        s = self.session
        problem = s.check() or s.error
        lines = [("Problem: " + problem) if problem else "Valid: this configuration builds."]
        selected = [n for n, v in s.requested.items() if v]
        removed = [n for n, v in s.requested.items() if not v]
        lines.append(
            f"Selected {len(selected)}, turned off {len(removed)}, "
            f"added through dependencies {len(s.implied())}."
        )
        lines.append(
            f"File: {s.path.relative_to(ROOT)}" + ("  (exists)" if s.path.exists() else "")
        )
        lines += ["", *s.document().splitlines()]
        return lines


def run_curses(app: Configurator) -> None:
    import curses

    os.environ.setdefault("ESCDELAY", "25")

    def main(scr):
        try:
            curses.curs_set(0)
        except curses.error:
            pass
        scr.keypad(True)
        curses.use_default_colors()
        attrs = {
            "item": curses.A_NORMAL,
            "header": curses.A_BOLD,
            "dim": curses.A_DIM,
            "error": curses.A_BOLD,
        }
        if curses.has_colors():
            curses.start_color()
            curses.use_default_colors()
            curses.init_pair(1, curses.COLOR_WHITE, curses.COLOR_BLUE)
            curses.init_pair(2, curses.COLOR_BLACK, curses.COLOR_CYAN)
            curses.init_pair(3, curses.COLOR_YELLOW, -1)
            curses.init_pair(4, curses.COLOR_RED, -1)
            attrs["header"] = curses.color_pair(3) | curses.A_BOLD
            attrs["error"] = curses.color_pair(4) | curses.A_BOLD
        bar_on = curses.color_pair(1) | curses.A_BOLD if curses.has_colors() else curses.A_REVERSE
        cursor_attr = curses.color_pair(2) if curses.has_colors() else curses.A_REVERSE

        def put(win, y, x, text, attr=curses.A_NORMAL):
            height, width = win.getmaxyx()
            if 0 <= y < height and 0 <= x < width - 1:
                try:
                    win.addstr(y, x, text[: width - x - 1], attr)
                except curses.error:
                    pass

        def box(title, footer, rows, columns):
            height, width = scr.getmaxyx()
            box_h = min(height - 2, rows + 2)
            box_w = min(width - 2, max(columns + 4, len(footer) + 4, len(title) + 4))
            win = curses.newwin(
                box_h, box_w, max(0, (height - box_h) // 2), max(0, (width - box_w) // 2)
            )
            win.box()
            put(win, 0, 2, f" {title} ", curses.A_BOLD)
            put(win, box_h - 1, 2, f" {footer} ", curses.A_DIM)
            return win

        def pick(title, items, current=0):
            """Choose from a list; typing filters it. Return the index into `items` or None."""
            text, index = "", current
            while True:
                found = [i for i, item in enumerate(items) if text.lower() in item.lower()]
                index = max(0, min(index, len(found) - 1))
                visible = min(len(items), scr.getmaxyx()[0] - 8)
                win = box(
                    title,
                    "Enter select  Esc cancel  type to filter",
                    visible + 1,
                    max(len(item) for item in items),
                )
                put(win, 1, 2, "Filter: " + text, curses.A_DIM)
                top = max(0, min(index - visible + 1, len(found) - visible))
                for line, i in enumerate(found[top : top + visible]):
                    attr = cursor_attr if top + line == index else curses.A_NORMAL
                    put(win, 2 + line, 2, items[i].ljust(win.getmaxyx()[1] - 5), attr)
                win.refresh()
                key = scr.getch()
                if key == 27:
                    return None
                if key in (curses.KEY_ENTER, 10, 13):
                    return found[index] if found else None
                if key == curses.KEY_UP:
                    index -= 1
                elif key == curses.KEY_DOWN:
                    index += 1
                elif key in (curses.KEY_BACKSPACE, 127, 8):
                    text = text[:-1]
                elif 32 <= key < 127:
                    text += chr(key)

        def prompt(title, initial=""):
            text = initial
            while True:
                win = box(title, "Enter accept  Esc cancel", 1, 40)
                put(win, 1, 2, text + "_")
                win.refresh()
                key = scr.getch()
                if key == 27:
                    return None
                if key in (curses.KEY_ENTER, 10, 13):
                    return text
                if key in (curses.KEY_BACKSPACE, 127, 8):
                    text = text[:-1]
                elif 32 <= key < 127:
                    text += chr(key)

        def confirm(question):
            win = box("Confirm", "y yes  n no", 1, len(question))
            put(win, 1, 2, question)
            win.refresh()
            while True:
                key = scr.getch()
                if key in (ord("y"), ord("Y")):
                    return True
                if key in (ord("n"), ord("N"), 27):
                    return False

        views = {tab: {"cursor": 0, "top": 0} for tab in TABS}
        tab = 0
        message, bad = "", False

        def rows_for_tab():
            if TABS[tab] == "Target":
                return app.target_rows(), app.target_detail
            if TABS[tab] == "Features":
                return app.feature_rows(), app.feature_detail
            if TABS[tab] == "Summary":
                return [
                    Row(line, "error" if line.startswith("Problem") else "item")
                    for line in app.summary_lines()
                ], None
            return [Row(line) for line in HELP], None

        def write():
            """Validate and write; return whether the file on disk matches the session."""
            nonlocal message, bad
            s = app.session
            problem = s.check()
            if problem:
                message, bad = problem, True
                return False
            if (
                s.path.exists()
                and s.path != s.loaded_path
                and not confirm(f"{s.path.name} exists. Overwrite?")
            ):
                return False
            problem = s.write(overwrite=True)
            if problem:
                message, bad = problem, True
                return False
            s.auto_name = False
            app.saved = s.document()
            message = f"Wrote {s.path.relative_to(ROOT)} - b builds it"
            return True

        def build():
            nonlocal message, bad
            if not write():
                return
            name = app.session.name
            curses.def_prog_mode()
            curses.endwin()
            try:
                code = run_build(name)
            except OSError as error:
                print(f"build could not start: {error}")
                code = -1
            print(
                f"\nBuild {'succeeded' if code == 0 else f'failed (exit {code})'}. "
                "Press Enter to return to make config.",
                flush=True,
            )
            try:
                input()
            except EOFError:
                pass
            curses.reset_prog_mode()
            scr.clear()
            message, bad = (
                (f"Built {name}: build/firmware/{name}/", False)
                if code == 0
                else (
                    f"Build of {name} failed (exit {code}); rerun make build ENV={name} to read it",
                    True,
                )
            )

        def retarget(base):
            nonlocal message, bad
            s = app.session
            if base.name == s.base.name:
                return
            app.session, dropped = s.retarget(base)
            message = ("Dropped what does not fit: " + ", ".join(dropped)) if dropped else ""
            bad = bool(dropped)

        def edit_target(row):
            nonlocal message, bad
            s = app.session
            platform = s.base.platform_directory
            if row.action == "platform":
                options = platforms()
                chosen = pick(
                    "Platform family",
                    [PLATFORM_NAMES.get(item, item) for item in options],
                    options.index(platform) if platform in options else 0,
                )
                if chosen is not None and options[chosen] != platform:
                    retarget(default_base(options[chosen]))
            elif row.action == "chip":
                options = chips(platform)
                labels = [
                    f"{chip:<12} {len(bases(platform, chip))} environment(s)" for chip in options
                ]
                chosen = pick(
                    "Chip", labels, options.index(s.base.chip) if s.base.chip in options else 0
                )
                if chosen is not None and options[chosen] != s.base.chip:
                    retarget(default_base(platform, options[chosen]))
            elif row.action == "base":
                options = bases(platform, s.base.chip)
                chosen = pick(
                    f"Environment for {s.base.chip}",
                    [environment_label(item) for item in options],
                    next((i for i, b in enumerate(options) if b.name == s.base.name), 0),
                )
                if chosen is not None:
                    retarget(options[chosen])
            elif row.action == "name":
                value = prompt("Name (lowercase, kebab-case)", s.name)
                if value and value.strip() != s.name:
                    s.name, s.auto_name = value.strip(), False
            elif row.action == "profile":
                chosen = pick(
                    "Profile",
                    list(PROFILES),
                    PROFILES.index(s.profile) if s.profile in PROFILES else 0,
                )
                if chosen is not None:
                    problem = s.set_profile(PROFILES[chosen])
                    message, bad = problem, bool(problem)
            elif row.action == "language":
                options = languages()
                chosen = pick(
                    "Language", options, options.index(s.language) if s.language in options else 0
                )
                if chosen is not None:
                    s.language = options[chosen]
            elif row.action == "open":
                files = user_files()
                if not files:
                    message = "user_config/ has no user configurations yet"
                    return
                chosen = pick("User configuration", [f.name for f in files])
                if chosen is not None and (not app.dirty or confirm("Discard unsaved changes?")):
                    try:
                        app.session = ConfigSession.from_file(files[chosen])
                        app.saved = app.session.document()
                    except CatalogError as error:
                        message, bad = str(error), True
            elif row.action == "build":
                build()

        while True:
            height, width = scr.getmaxyx()
            if height < 12 or width < 50:
                scr.erase()
                put(scr, 0, 0, "Terminal too small (need 50x12). Resize, or q to quit.")
                scr.refresh()
                if scr.getch() in (ord("q"), ord("Q")):
                    return
                continue
            rows, detail = rows_for_tab()
            view = views[TABS[tab]]
            view["cursor"] = max(0, min(view["cursor"], len(rows) - 1))
            detail_h = 7 if detail else 0
            list_h = height - 3 - detail_h - (1 if detail else 0)
            view["top"] = max(0, min(view["top"], max(0, len(rows) - list_h)))
            view["top"] = min(view["top"], view["cursor"])
            if view["cursor"] >= view["top"] + list_h:
                view["top"] = view["cursor"] - list_h + 1

            scr.erase()
            x = 0
            for i, title in enumerate(TABS):
                label = f" {title} "
                put(scr, 0, x, label, bar_on if i == tab else curses.A_REVERSE)
                x += len(label) + 1
            dirty = "*" if app.dirty else " "
            put(
                scr,
                0,
                max(x + 1, width - 40),
                f"{dirty}{app.session.name} <- {app.session.base.name}"[:38],
                curses.A_DIM,
            )
            scrolls = TABS[tab] in ("Summary", "Help")
            for line, row in enumerate(rows[view["top"] : view["top"] + list_h]):
                index = view["top"] + line
                attr = attrs.get(row.style, curses.A_NORMAL)
                current = index == view["cursor"] and not scrolls
                put(
                    scr,
                    1 + line,
                    0,
                    row.text.ljust(width - 1) if current else row.text,
                    cursor_attr if current else attr,
                )
            if detail and rows:
                put(scr, height - 3 - detail_h, 0, "-" * (width - 1), curses.A_DIM)
                for line, text in enumerate(detail(rows[view["cursor"]])[:detail_h]):
                    put(
                        scr,
                        height - 2 - detail_h + line,
                        1,
                        text,
                        attrs["header"] if line == 0 else curses.A_NORMAL,
                    )
            status = message or app.session.error
            put(
                scr,
                height - 2,
                0,
                status,
                attrs["error"] if (bad or app.session.error) else curses.A_DIM,
            )
            hint = {
                "Target": "Enter change  w write  b build  q quit",
                "Features": "Space toggle  / filter  a/z open/close all  w write  b build  q quit",
                "Summary": "Up/Down scroll  w write  b build  q quit",
                "Help": "Up/Down scroll  q quit",
            }[TABS[tab]]
            put(scr, height - 1, 0, hint, curses.A_DIM)
            scr.refresh()

            key = scr.getch()
            message, bad = "", False
            if key in (ord("q"), ord("Q")):
                if not app.dirty or confirm("Discard unsaved changes?"):
                    return
            elif key in (curses.KEY_RIGHT, 9):
                tab = (tab + 1) % len(TABS)
            elif key in (curses.KEY_LEFT, curses.KEY_BTAB):
                tab = (tab - 1) % len(TABS)
            elif key == curses.KEY_DOWN:
                view["cursor"] += 1
            elif key == curses.KEY_UP:
                view["cursor"] -= 1
            elif key == curses.KEY_NPAGE:
                view["cursor"] += list_h - 1
            elif key == curses.KEY_PPAGE:
                view["cursor"] -= list_h - 1
            elif key == curses.KEY_HOME:
                view["cursor"] = 0
            elif key == curses.KEY_END:
                view["cursor"] = len(rows) - 1
            elif key in (ord("w"), ord("W")):
                write()
            elif key in (ord("b"), ord("B")):
                build()
            elif key == curses.KEY_RESIZE:
                pass
            elif TABS[tab] == "Features":
                row = rows[view["cursor"]] if rows else None
                if key == ord("/"):
                    value = prompt("Filter features", app.filter)
                    if value is not None:
                        app.filter, view["cursor"] = value.strip(), 0
                elif key == 27:
                    app.filter = ""
                elif key == ord("a"):
                    app.collapsed.clear()
                elif key == ord("z"):
                    app.collapsed.update(group for group, _ in grouped_features())
                elif row and key in (ord(" "), 10, 13, curses.KEY_ENTER):
                    if row.action == "group":
                        app.collapsed.symmetric_difference_update({row.key})
                    else:
                        problem = app.session.toggle(row.key)
                        message, bad = problem, bool(problem)
            elif TABS[tab] == "Target" and key in (ord(" "), 10, 13, curses.KEY_ENTER):
                edit_target(rows[view["cursor"]])

    curses.wrapper(main)


def build_session(args: argparse.Namespace) -> ConfigSession:
    """Open the session the command line asks for."""

    environments = {item.name: item for item in application_environments()}
    user = {path.stem: path for path in user_files()}
    target = args.env
    if target in user:
        session = ConfigSession.from_file(user[target])
    elif target in environments:
        session = ConfigSession.from_base(environments[target])
    elif target:
        raise CatalogError(f"unknown environment '{target}'")
    else:
        session = ConfigSession.from_base(default_base(platforms()[0]))
    if args.name:
        session.name, session.auto_name = args.name, False
    if args.profile and (problem := session.set_profile(args.profile)):
        raise CatalogError(problem)
    if args.language:
        if args.language not in languages():
            raise CatalogError(f"unknown language '{args.language}'")
        session.language = args.language
    for name in args.enable:
        if problem := session.enable(name):
            raise CatalogError(f"--enable {name}: {problem}")
    for name in args.disable:
        if problem := session.disable(name):
            raise CatalogError(f"--disable {name}: {problem}")
    return session


def list_features(session: ConfigSession) -> None:
    for group, names in grouped_features():
        print(group)
        for name in names:
            state = session.state(name)
            reason = f"  [{state.reason}]" if state.reason else ""
            print(f"  {MARKERS[state.kind]} {name:<34} {state.summary}{reason}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Configure a user build in user_config/.")
    parser.add_argument("--env", help="checked-in environment or user configuration to start from")
    parser.add_argument("--name", help="name of the user configuration")
    parser.add_argument("--profile", choices=PROFILES)
    parser.add_argument("--language", help="build-time locale, such as de_DE")
    parser.add_argument("--enable", action="append", default=[], metavar="USE_X")
    parser.add_argument("--disable", action="append", default=[], metavar="USE_X")
    parser.add_argument("--write", action="store_true", help="write user_config/<name>.yaml")
    parser.add_argument("--build", action="store_true", help="write, then build the result")
    parser.add_argument("--force", action="store_true", help="overwrite an existing file")
    parser.add_argument("--print", action="store_true", help="print the file instead of writing")
    parser.add_argument(
        "--list-features", action="store_true", help="show every feature's state for this base"
    )
    args = parser.parse_args()
    scripted = args.write or args.build or args.print or args.list_features
    try:
        session = build_session(args)
        if args.list_features:
            list_features(session)
        if args.print:
            sys.stdout.write(session.document())
        if args.write or args.build:
            if problem := session.write(overwrite=args.force):
                raise CatalogError(problem)
            print(f"Wrote {session.path.relative_to(ROOT)}")
        if args.build:
            return run_build(session.name)
        if scripted:
            return 0
        if not (sys.stdin.isatty() and sys.stdout.isatty()):
            raise CatalogError("make config needs a terminal; use --print or --write to script it")
        run_curses(Configurator(session))
    except (CatalogError, OSError, ValueError) as error:
        print(f"configuration error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
