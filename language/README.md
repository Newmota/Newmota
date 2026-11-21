# Newmota locale catalogs

Each firmware selects exactly one locale at build time through the environment's
`language` field. Catalog IDs use `ll_CC` form (`en_GB`, `de_DE`); the legacy `en` value
is accepted as an alias for `en_GB`.

`en_GB.toml` is the canonical key set. Every other catalog must contain the same
`[strings.web]` and `[strings.js]` keys. `tools/newmota/i18n.py` rejects missing or unknown
keys, validates locale metadata, escapes HTML and inline-JavaScript substitutions, and
renders the result into the deterministic HTTP+Gzip `web_assets.h` header. There is no
runtime translation dictionary or language switch, which keeps Compact firmware small.

Protocol-facing strings remain English: command names, JSON keys, MQTT topics, and server
error responses are not translated. Add browser presentation text to the catalog only when
it is safe to localize independently of those protocol contracts. The active login,
navigation, dashboard, console, Wi-Fi, information, filesystem browser and text editor, and browser status surfaces are
cataloged now; staged mockup panels may retain fixed technical/demo labels until their
backing APIs are implemented.
