"""First-party C/C++ must not use printf's %ll or %j conversions.

newlib-nano, the Beken C library, does not implement them and prints "lu" instead, which broke
the df command and the web status JSON. Use newmota::support::decimal() or digits() from
src/app/support/number.h with %s.

The check reads string literals as the compiler does: comments are dropped, adjacent literals
are joined (so "%" "llu" counts, also across a backslash-newline), escapes are decoded (\\045 is
"%"), a PRI*64 or PRI*MAX macro between literals counts as a 64-bit conversion, raw strings and
digit separators are read whole, and "%%" is literal text.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}
# One token: a comment, a raw or ordinary string literal, a number (which may contain C++14
# digit separators such as 20'000), a character literal, a 64-bit PRI macro, an identifier, or
# any other character.
TOKEN = re.compile(
    r"//[^\n]*|/\*.*?\*/"
    r'|(?P<raw>\b(?:u8|u|U|L)?R"(?P<delimiter>[^()\\\s"]{0,16})\((?P<raw_body>.*?)\)(?P=delimiter)")'
    r'|(?P<string>\b(?:u8|u|U|L)?"(?:\\.|[^"\\\n])*"|"(?:\\.|[^"\\\n])*")'
    r"|\.?\d(?:[eEpP][+-]|'(?=\w)|[\w.])*"
    r"|\b(?:u8|u|U|L)?'(?:\\.|[^'\\\n])*'|'(?:\\.|[^'\\\n])*'"
    r"|(?P<pri>\bPRI[diouxX](?:64|MAX)\b)"
    r"|\w+"
    r"|(?P<space>(?:\s|\\\n)+)"
    r"|.",
    re.DOTALL,
)
# A conversion after an unescaped %: flags, width, precision, then the length and type.
# A bare "." is precision zero, so "%.llu" is a conversion too.
CONVERSION = re.compile(
    r"%[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+)?)?(hh|h|ll|l|j|z|t|L|q|I64|I32|I)?([a-zA-Z])"
)
# Lengths that mean a 64-bit integer argument: ll, intmax_t (j), and the long long spellings L,
# q and I64. Only integer conversions count: %Lf is long double, and "%LEL" is a unit string.
LONG_LONG = {"ll", "j", "L", "q", "I64"}
ESCAPE = re.compile(r"\\(?:([0-7]{1,3})|x([0-9a-fA-F]+)|(.))", re.DOTALL)


def decode(body: str) -> str:
    """The characters of an ordinary literal's body: \\045 and \\x25 are both "%"."""

    def character(match: re.Match[str]) -> str:
        if match.group(1) is not None:
            return chr(int(match.group(1), 8) & 0xFF)
        if match.group(2) is not None:
            return chr(int(match.group(2), 16) & 0xFF)
        return match.group(3) if match.group(3) in "\\\"'?" else " "

    # A backslash-newline inside a literal is a line splice: it joins the lines.
    return ESCAPE.sub(character, body.replace("\\\n", ""))


def literal_runs(text: str):
    """Yields (line, joined text, has PRI*64) for each run of adjacent string literals."""
    run: list[str] = []
    start = 0
    pri = False
    for match in TOKEN.finditer(text):
        if match.group("raw") is not None or match.group("string") is not None:
            if not run:
                start = text.count("\n", 0, match.start()) + 1
            if match.group("raw") is not None:
                run.append(match.group("raw_body"))  # raw text has no escapes
            else:
                body = match.group("string")
                run.append(decode(body[body.index('"') + 1 : -1]))
        elif match.group("pri") is not None and run:
            pri = True
        elif match.group("space") is not None or match.group(0).startswith(("//", "/*")):
            continue
        else:
            if run:
                yield start, "".join(run), pri
            run, pri = [], False
    if run:
        yield start, "".join(run), pri


def long_long_conversions(literal: str) -> list[str]:
    """Returns the %ll and %j conversions in one joined literal, skipping %% escapes."""
    found = []
    index = 0
    while (index := literal.find("%", index)) != -1:
        if literal.startswith("%%", index):
            index += 2
            continue
        match = CONVERSION.match(literal, index)
        if match and match.group(1) in LONG_LONG and match.group(2) in "diouxX":
            found.append(match.group(0))
        index = match.end() if match else index + 1
    return found


def offenders_in(text: str) -> list[tuple[int, str]]:
    result = []
    for line, literal, pri in literal_runs(text):
        found = long_long_conversions(literal)
        if pri:
            found.append("PRI*64/PRI*MAX")
        result.extend((line, item) for item in found)
    return result


class NoLongLongPrintfTest(unittest.TestCase):
    def test_scanner(self) -> None:
        self.assertEqual(offenders_in('f("%llu", x);'), [(1, "%llu")])
        self.assertEqual(offenders_in('f("%" "llu", x);'), [(1, "%llu")])
        self.assertEqual(offenders_in('f("a%0*"\n  "llX", w, x);'), [(1, "%0*llX")])
        self.assertEqual(offenders_in('f("%" PRIu64, x);'), [(1, "PRI*64/PRI*MAX")])
        self.assertEqual(offenders_in('f("%" PRIdMAX, x);'), [(1, "PRI*64/PRI*MAX")])
        self.assertEqual(offenders_in('f("%jd %ju", a, b);'), [(1, "%jd"), (1, "%ju")])
        # Digit separators are not character literals.
        self.assertEqual(offenders_in("f(20'000, \"%llu\", x); // it's"), [(1, "%llu")])
        # A raw string's quotes do not end it; the code after it is still scanned.
        raw = 'g(R"be(print("x") \'%llu\')be");\nf("%llu", x);'
        self.assertEqual(offenders_in(raw), [(1, "%llu"), (2, "%llu")])
        self.assertEqual(offenders_in('g(R"(say "hi")"); f("%lu");'), [])
        # The compiler assembles these into %llu as well.
        self.assertEqual(offenders_in('f("\\045llu", x);'), [(1, "%llu")])
        self.assertEqual(offenders_in('f("\\x25" "lld", x);'), [(1, "%lld")])
        self.assertEqual(offenders_in('#define F "%" \\\n  "llu"\n'), [(1, "%llu")])
        self.assertEqual(offenders_in('f("%l\\\nlu", x);'), [(1, "%llu")])
        self.assertEqual(
            offenders_in('f("%Lu %qd %I64d %I32d %Lf %LEL", a, b, c, d, e);'),
            [(1, "%Lu"), (1, "%qd"), (1, "%I64d")],
        )
        # An empty precision is precision zero.
        self.assertEqual(offenders_in('f("%.llu %.jd", a, b);'), [(1, "%.llu"), (1, "%.jd")])
        # Escapes that are not "%" stay harmless.
        self.assertEqual(offenders_in('f("\\\\llu \\"llu\\" %lu");'), [])
        self.assertEqual(offenders_in('f("%%llu %lu %s");'), [])
        self.assertEqual(offenders_in('// "%llu"\n/* "%lld" */ f("\\"%llu");'), [(2, "%llu")])

    def test_first_party_sources(self) -> None:
        offenders = []
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in SUFFIXES or "third_party" in path.parts:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for line, item in offenders_in(text):
                offenders.append(f"{path.relative_to(ROOT)}:{line}: {item}")
        self.assertEqual(offenders, [], "\n".join(offenders))


if __name__ == "__main__":
    unittest.main()
