"""Shared console feedback for device operations: one growing bar and wait notices."""

from __future__ import annotations

import sys
from typing import TextIO

# How long device commands wait for a user to put a board into its boot/download mode.
WAIT_SECONDS = 15
BAR_WIDTH = 30


def _kib(value: int) -> str:
    return f"{value / 1024:.0f}" if value >= 10 * 1024 else f"{value / 1024:.1f}"


def format_elapsed(seconds: float) -> str:
    """Format a duration as minutes and seconds."""

    minutes, remainder = divmod(round(seconds), 60)
    return f"{minutes}m {remainder:02d}s"


class Progress:
    """Draw one line whose bar grows in place; non-terminals get a line per quarter."""

    def __init__(self, label: str, total: int, stream: TextIO | None = None) -> None:
        self.label = label
        self.total = max(total, 1)
        self.stream = stream if stream is not None else sys.stdout
        self.interactive = self.stream.isatty()
        self.done = 0
        self.active = False
        self.quarter = -1

    def update(self, done: int, label: str | None = None) -> None:
        """Show done bytes of the total, optionally under a new label."""

        self.done = min(max(done, 0), self.total)
        if label is not None:
            self.label = label
        percent = self.done * 100 // self.total
        if self.interactive:
            filled = self.done * BAR_WIDTH // self.total
            bar = "#" * filled + "-" * (BAR_WIDTH - filled)
            self.stream.write(
                f"\r{self.label:<28} [{bar}] {percent:3d}%  "
                f"{_kib(self.done)}/{_kib(self.total)} KiB\x1b[K"
            )
            self.active = True
        elif percent // 25 > self.quarter:
            self.quarter = percent // 25
            self.stream.write(f"{self.label}: {percent}%\n")
        self.stream.flush()

    def notice(self, text: str) -> None:
        """Print a message on its own line without breaking an active bar."""

        self.end_line()
        self.stream.write(text + "\n")
        self.stream.flush()

    def end_line(self) -> None:
        """Move past the bar line so later output starts on a fresh line."""

        if self.active:
            self.stream.write("\n")
            self.stream.flush()
            self.active = False

    def finish(self) -> None:
        """Complete the bar at 100 % and end its line."""

        self.update(self.total)
        self.end_line()


def notice(text: str, stream: TextIO | None = None) -> None:
    """Print one operator-facing line immediately."""

    target = stream if stream is not None else sys.stdout
    target.write(text + "\n")
    target.flush()
