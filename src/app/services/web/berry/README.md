# Berry widget module

Applications with Berry and the web interface provide `import widget`. A script describes a
dashboard card as data; the Home page draws it with the same components as the native cards.
The module replaces Tasmota's `MI32.widget()`: there is no HTML, no inline script and no chart
DSL.

```berry
import widget
widget.set("garage", {"title": "Garage", "w": 2, "parts": [
    {"value": 21.5, "unit": "°C", "label": "Inside"},
    {"line": [{"values": [20.1, 20.4, nil, 21.5], "color": 0xFF9632}],
     "labels": ["06:00", "12:00", "18:00"], "grid": true},
    {"gauge": 64, "min": 0, "max": 100, "unit": "%",
     "stops": [[0, 0x00C800], [70, 0xC8A000], [90, 0xC80000]]}]})
widget.on("garage", def (part, value) print(part, value) end)
widget.remove("garage")   # true when it existed
```

- `widget.set(id, map)` creates or replaces a widget with `json.dump(map)`. Use an id of 1–31
  characters of `A-Z a-z 0-9 _ - .`, other than `.` and `..`. The map takes `title`, `w` and
  `h` (1–3 columns and rows on wide screens, default 1), `order` (integer, default 0; equal
  orders keep registration order) and `parts`.
- Each part has exactly one kind key from the table below and that kind's fields; any part may
  also carry `name` (1–31 characters of `A-Z a-z 0-9 _ -`, not starting with a digit).
- The module trusts the script: it does not check the map. A wrong structure shows up as a
  missing or partial widget on the page. Keeping a widget within its bounds is the script's
  job; `json.dump` builds the whole string before `set` compares it. `set` raises `value_error`
  only when a bound would break memory or the response: id length, an empty map, more than
  3968 bytes of JSON, more than 16 widgets.
- `widget.on(id, fn)` sets the callback for an existing widget's controls (an unknown id is
  ignored); `widget.on(id, nil)` removes it, and `widget.remove(id)` drops it with the widget. The callback runs on the Berry
  loop as `fn(part, value)`: `part` is the part's `name`, or its zero-based index as an integer
  when it has none, and `value` is a string. An exception in the callback is logged; the widget
  and callback stay.
- `widget.viewers()` is true while a page has fetched the widget list within the last three
  seconds, so a script can skip work nobody sees.

| Part | Fields |
| --- | --- |
| `text` | string; `small` (bool) |
| `value` | number or string; `unit`, `label` |
| `line` | list of series `{"values": [...], "color": 0xRRGGBB}`; `labels`, `grid`, `min`, `max` |
| `hist` | list of numbers; `color`, `labels`, `grid`, `min`, `max` |
| `gauge` | number; `min` (default 0), `max` (default 100), `unit`, `label`, `stops` |
| `button` | label; sends `""` |
| `toggle` | label; `state` (bool); sends `"true"` or `"false"` |
| `number` | label; `value`, `min`, `max`, `step`, `unit`; sends the entered number as text |
| `input` | label; `value` (string); sends the entered text, at most 63 bytes |
| `select` | label; `options` (strings or numbers), `selected` (index); sends the chosen index |
| `table` | list of rows, each a list of strings or numbers; `columns` |
| `matrix` | list of rows as integers, top bit leftmost; `size` (dots per row, at most 32). Drawn dark on white, for QR codes |
| `qr` | text, at most 25 QR alphanumeric characters (digits, `A`–`Z`, space, `$%*+-./:`), such as a Matter setup payload. The browser encodes it as a 21×21 QR code and draws it like `matrix`; other text draws nothing |

`nil` in `values` or `hist` is a gap. `labels` are strings or numbers spread evenly along the
x axis; at most six are shown. A gauge stop `[threshold, color]` colours the range from its
threshold upward, and the value arc takes the colour of the highest stop it reached.

Controls send their value when clicked, toggled or changed (Enter or leaving the field). The
page does not change the widget itself: the script answers by setting the widget again, for
example with the new toggle `state`. A failed send reverts a toggle and shows the error in the
status line. While someone types into a field or has a select's menu open, that widget is not
redrawn. A table scrolls inside its widget; with the 3968-byte limit, a script that lists many
rows should filter or page them itself. Up to eight actions
wait for the Berry loop; a full queue rejects further ones.

Limits: 16 widgets, each at most 3968 bytes as JSON, otherwise `set` raises and the previous
content stays. Content is copied; changing a list after `set` does not change the widget.
Widgets live in RAM and disappear when the Berry VM goes away, so scripts create them at start.
The compact Beken page draws only `text`, `value`, `button`, `toggle` and `matrix` parts, plus `qr`
in builds with `USE_MATTER`, and skips the others, enough for the Matter pairing card.

Examples: [`examples/widget/widget_multi.be`](../../../../../examples/widget/widget_multi.be),
[`examples/widget/widget_ac.be`](../../../../../examples/widget/widget_ac.be),
[`examples/widget/widget_linegraph_scroll.be`](../../../../../examples/widget/widget_linegraph_scroll.be) and
[`examples/widget/widget_scanner.be`](../../../../../examples/widget/widget_scanner.be) (needs `USE_BLE`),
ports of the Tasmota `berry-examples/dashboard` scripts.
