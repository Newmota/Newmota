"""Build-time locale catalogs and deterministic web UI localization.

Newmota deliberately keeps protocol-facing strings (commands, JSON keys, and
MQTT topics) in English. Only presentation strings are selected here, once per
firmware build, and rendered into the immutable compressed web asset.
"""

from __future__ import annotations

import hashlib
import html
import json
import re
from collections.abc import Mapping
from dataclasses import dataclass
from functools import cache
from pathlib import Path
from typing import Any

from newmota.compression import zopfli_gzip
from newmota.host import write_if_changed
from newmota.model import ROOT

LANGUAGE_DIR = ROOT / "language"
DEFAULT_LANGUAGE = "en_GB"
_LANGUAGE_ID = re.compile(r"[a-z]{2}_[A-Z]{2}")
# Build-time page sections: <!--#if USE_X--> in markup, /*#if USE_X*/ in scripts and styles.
_SECTION = re.compile(r"<!--#(if !?USE_[A-Z0-9_]+|endif)-->|/\*#(if !?USE_[A-Z0-9_]+|endif)\*/")
_TOKEN = re.compile(r"\{\{i18n\.([a-z][a-z0-9_]*)\.([a-z][a-z0-9_.]*)\}\}")
_KEY = re.compile(r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)*")


class I18nError(ValueError):
    """Raised when a locale catalog is missing or malformed."""


@dataclass(frozen=True)
class LocaleCatalog:
    """One validated locale with the complete presentation key set."""

    id: str
    name: str
    html: str
    direction: str
    lcid: int
    strings: dict[str, str]
    missing: tuple[str, ...]

    def text(self, key: str) -> str:
        try:
            return self.strings[key]
        except KeyError as error:
            raise I18nError(f"locale {self.id} has no string '{key}'") from error

    def metadata(self, key: str) -> str | int:
        values: dict[str, str | int] = {
            "id": self.id,
            "name": self.name,
            "html": self.html,
            "direction": self.direction,
            "lcid": self.lcid,
        }
        try:
            return values[key]
        except KeyError as error:
            raise I18nError(f"locale {self.id} has no metadata '{key}'") from error


def canonical_locale_id(language: str) -> str:
    """Return the stable catalog ID for an environment language value."""
    if not isinstance(language, str):
        raise I18nError("language must be a string")
    if not _LANGUAGE_ID.fullmatch(language):
        raise I18nError(f"invalid language '{language}' (expected ll_CC)")
    return language


def _flatten_strings(value: Any, prefix: str = "") -> dict[str, str]:
    if not isinstance(value, dict):
        raise I18nError("[strings] must contain tables of UTF-8 strings")
    flattened: dict[str, str] = {}
    for key, child in value.items():
        if not isinstance(key, str) or not key:
            raise I18nError("locale string keys must be non-empty strings")
        full_key = f"{prefix}.{key}" if prefix else key
        if isinstance(child, dict):
            flattened.update(_flatten_strings(child, full_key))
        elif isinstance(child, str) and _KEY.fullmatch(full_key):
            flattened[full_key] = child
        else:
            raise I18nError(f"locale string '{full_key}' must be a UTF-8 string")
    return flattened


def _read_catalog(path: Path) -> tuple[dict[str, Any], dict[str, str]]:
    try:  # Keep platform registry imports usable without optional TOML packages.
        import tomllib
    except ModuleNotFoundError:  # pragma: no cover - exercised on Python 3.10
        try:
            import tomli as tomllib
        except ModuleNotFoundError as error:
            raise I18nError(
                "locale catalogs require Python tomllib or the tomli package"
            ) from error
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        raise I18nError(f"cannot read {path.relative_to(ROOT)}: {error}") from error
    unknown = sorted(set(data) - {"locale", "strings"})
    if unknown:
        raise I18nError(
            f"{path.relative_to(ROOT)} has unknown top-level tables: {', '.join(unknown)}"
        )
    metadata = data.get("locale")
    if not isinstance(metadata, dict):
        raise I18nError(f"{path.relative_to(ROOT)} requires a [locale] table")
    strings = _flatten_strings(data.get("strings", {}))
    return metadata, strings


def _validate_metadata(
    path: Path, metadata: dict[str, Any], expected: str
) -> tuple[str, str, str, str, int]:
    unknown = sorted(set(metadata) - {"id", "name", "html", "direction", "lcid"})
    if unknown:
        raise I18nError(f"{path.relative_to(ROOT)} has unknown locale fields: {', '.join(unknown)}")
    locale_id = metadata.get("id")
    name = metadata.get("name")
    html_language = metadata.get("html")
    direction = metadata.get("direction")
    lcid = metadata.get("lcid")
    if (
        locale_id != expected
        or not isinstance(locale_id, str)
        or not _LANGUAGE_ID.fullmatch(locale_id)
    ):
        raise I18nError(f"{path.relative_to(ROOT)} locale.id must be '{expected}'")
    if not isinstance(name, str) or not name:
        raise I18nError(f"{path.relative_to(ROOT)} locale.name must be non-empty")
    if not isinstance(html_language, str) or not re.fullmatch(
        r"[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8})*", html_language
    ):
        raise I18nError(f"{path.relative_to(ROOT)} locale.html must be a BCP-47 language tag")
    if direction not in {"ltr", "rtl"}:
        raise I18nError(f"{path.relative_to(ROOT)} locale.direction must be ltr or rtl")
    if not isinstance(lcid, int) or isinstance(lcid, bool) or not 1 <= lcid <= 9999:
        raise I18nError(f"{path.relative_to(ROOT)} locale.lcid must be a positive integer")
    return locale_id, name, html_language, direction, lcid


@cache
def load_locale(language: str = DEFAULT_LANGUAGE) -> LocaleCatalog:
    """Load one complete locale catalog; partial translations fail closed."""
    locale_id = canonical_locale_id(language)
    selected_path = LANGUAGE_DIR / f"{locale_id}.toml"
    fallback_path = LANGUAGE_DIR / f"{DEFAULT_LANGUAGE}.toml"
    if not selected_path.is_file():
        raise I18nError(f"missing locale catalog {selected_path.relative_to(ROOT)}")
    if not fallback_path.is_file():
        raise I18nError(f"missing fallback locale catalog {fallback_path.relative_to(ROOT)}")

    selected_meta, selected_strings = _read_catalog(selected_path)
    selected_id, name, html_language, direction, lcid = _validate_metadata(
        selected_path, selected_meta, locale_id
    )
    fallback_meta, fallback_strings = _read_catalog(fallback_path)
    _validate_metadata(fallback_path, fallback_meta, DEFAULT_LANGUAGE)
    unknown = sorted(set(selected_strings) - set(fallback_strings))
    if unknown:
        raise I18nError(
            f"{selected_path.relative_to(ROOT)} has unknown string keys: {', '.join(unknown)}"
        )
    missing = tuple(sorted(set(fallback_strings) - set(selected_strings)))
    if missing:
        raise I18nError(
            f"{selected_path.relative_to(ROOT)} is missing string keys: {', '.join(missing)}"
        )
    return LocaleCatalog(selected_id, name, html_language, direction, lcid, selected_strings, ())


def render_web_source(
    language: str = DEFAULT_LANGUAGE, features: Mapping[str, bool] | None = None
) -> bytes:
    """Render the HTML template for a locale and its feature sections before compression."""
    locale = load_locale(language)
    template_path = ROOT / "src/app/services/web/index.html"
    try:
        template = template_path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise I18nError(f"cannot read {template_path.relative_to(ROOT)}: {error}") from error
    template = select_sections(template, features or {}, template_path.relative_to(ROOT).as_posix())

    def replace(match: re.Match[str]) -> str:
        namespace, key = match.groups()
        if namespace == "locale":
            value: str | int = locale.metadata(key)
            return html.escape(str(value), quote=True)
        value = locale.text(f"{namespace}.{key}")
        if namespace == "js":
            # Keep translated strings safe inside the inline script element as
            # well as valid JavaScript string literals.
            return (
                json.dumps(value, ensure_ascii=False)
                .replace("<", "\\u003c")
                .replace(">", "\\u003e")
                .replace("&", "\\u0026")
                .replace("\u2028", "\\u2028")
                .replace("\u2029", "\\u2029")
            )
        return html.escape(value, quote=True)

    rendered, count = _TOKEN.subn(replace, template)
    if "{{i18n." in rendered:
        raise I18nError("web template contains an unresolved i18n token")
    if count == 0:
        raise I18nError("web template does not contain any i18n tokens")
    return rendered.encode("utf-8")


def locale_identity(language: str = DEFAULT_LANGUAGE) -> dict[str, Any]:
    """Return reproducible locale metadata and an input digest for build caching."""
    locale = load_locale(language)
    selected = LANGUAGE_DIR / f"{locale.id}.toml"
    fallback = LANGUAGE_DIR / f"{DEFAULT_LANGUAGE}.toml"
    template = ROOT / "src/app/services/web/index.html"
    digest = hashlib.sha256()
    for path in (Path(__file__), template, fallback, selected):
        digest.update(str(path.relative_to(ROOT)).encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    total = len(locale.strings)
    return {
        "id": locale.id,
        "html": locale.html,
        "direction": locale.direction,
        "lcid": locale.lcid,
        "coverage": 1.0 if total == 0 else (total - len(locale.missing)) / total,
        "missing": list(locale.missing),
        "sha256": digest.hexdigest(),
    }


# Script and style bodies are minified; pre and textarea keep their text verbatim.
_RAW_ELEMENT = re.compile(
    r"(<(script|style|pre|textarea)\b[^>]*>)(.*?)(</\2>)", re.DOTALL | re.IGNORECASE
)


def _minify_markup(text: str) -> str:
    """Drop HTML comments and collapse whitespace runs, which HTML renders as one space."""
    return re.sub(r"\s+", " ", re.sub(r"<!--.*?-->", "", text, flags=re.DOTALL))


def minify_html(source: bytes) -> bytes:
    """Remove comments and layout whitespace from a page without rewriting its code."""
    import rcssmin
    import rjsmin

    text = source.decode("utf-8")
    parts, position = [], 0
    for match in _RAW_ELEMENT.finditer(text):
        parts.append(_minify_markup(text[position : match.start()]))
        tag, body = match.group(2).lower(), match.group(3)
        if tag == "script":
            body = rjsmin.jsmin(body)
        elif tag == "style":
            body = rcssmin.cssmin(body)
        parts.append(_minify_markup(match.group(1)) + body + match.group(4))
        position = match.end()
    parts.append(_minify_markup(text[position:]))
    return "".join(parts).strip().encode("utf-8")


def _write_compressed_asset(
    source: bytes, destination: Path, namespace: str, description: str, encoding: str
) -> Path:
    """Write deterministic minified and precompressed bytes as one immutable C++ asset."""
    source = minify_html(source)
    if encoding == "gzip":
        compressed = zopfli_gzip(source)
    elif encoding == "br":
        import brotli

        compressed = brotli.compress(source, mode=brotli.MODE_TEXT, quality=11, lgwin=22)
    else:
        raise ValueError(f"unsupported web asset encoding: {encoding}")
    rows = [
        "    " + ", ".join(f"0x{byte:02x}" for byte in compressed[index : index + 16]) + ","
        for index in range(0, len(compressed), 16)
    ]
    write_if_changed(
        destination,
        "\n".join(
            [
                f"/* Generated from {description}; do not edit. */",
                "#pragma once",
                f"namespace newmota::generated::{namespace} {{",
                "inline constexpr unsigned char kIndex[] = {",
                *rows,
                "};",
                "}",
                "",
            ]
        ),
    )
    return destination


def generate_web_assets(
    directory: Path,
    language: str = DEFAULT_LANGUAGE,
    features: Mapping[str, bool] | None = None,
    *,
    encoding: str = "gzip",
) -> Path:
    """Embed deterministic Gzip or Brotli bytes for the locale, features and transport."""
    locale = load_locale(language)
    return _write_compressed_asset(
        render_web_source(language, features),
        directory / "web_assets.h",
        "web",
        f"src/app/services/web/index.html ({locale.id})",
        encoding,
    )


def generate_beken_web_assets(
    directory: Path, features: Mapping[str, bool], *, encoding: str = "gzip"
) -> Path:
    """Embed Beken's compact application page with the sections its features select."""
    return _generate_static_web_asset(
        ROOT / "src/platform/beken/web/index.html",
        directory / "beken_web_assets.h",
        "beken_web",
        encoding=encoding,
        features=features,
    )


def select_sections(text: str, features: Mapping[str, bool], description: str) -> str:
    """Keep a marked section only while its feature is on, or off for `!USE_X`. Sections nest."""
    kept, stack, position = [], [], 0
    for match in _SECTION.finditer(text):
        if all(stack):
            kept.append(text[position : match.start()])
        position = match.end()
        directive = match.group(1) or match.group(2)
        if directive == "endif":
            if not stack:
                raise I18nError(f"{description}: #endif without #if")
            stack.pop()
            continue
        name = directive.removeprefix("if ").removeprefix("!")
        if name not in features:
            raise I18nError(f"{description}: unknown feature {name} in #if")
        stack.append(features[name] != directive.startswith("if !"))
    if stack:
        raise I18nError(f"{description}: #if without #endif")
    kept.append(text[position:])
    return "".join(kept)


def _generate_static_web_asset(
    source: Path,
    destination: Path,
    namespace: str,
    *,
    encoding: str = "gzip",
    features: Mapping[str, bool] | None = None,
) -> Path:
    """Embed one small role-specific page as deterministic compressed bytes."""
    relative = source.relative_to(ROOT).as_posix()
    text = select_sections(source.read_text(encoding="utf-8"), features or {}, relative)
    return _write_compressed_asset(text.encode("utf-8"), destination, namespace, relative, encoding)


def generate_recovery_web_assets(directory: Path) -> Path:
    """Generate the small Safeboot recovery page without normal application assets."""
    return _generate_static_web_asset(
        ROOT / "src/app/services/web/recovery/index.html",
        directory / "recovery_web_assets.h",
        "recovery_web",
    )
