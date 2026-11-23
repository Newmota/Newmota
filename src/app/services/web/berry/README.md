# Berry widget module

Applications with Berry and the web interface provide `import widget`. As with Tasmota's
`MI32.widget()`, a script ships a widget's HTML (and, if it wants, its JS); the Home page adds
base code so that most widgets need only short markup and no JS. Unlike Tasmota, the markup is
set once and the values travel separately as data, which the page shows only as text.

```berry
import widget
widget.set("demo", '<div class="w2"><h3>Garage</h3>'
  '<p><span class="value" data-v="temp"></span><span class="unit">°C</span></p>'
  '<div data-chart="line" data-v="history" data-grid></div>'
  '<button data-act="open">Open</button></div>')
widget.data("demo", {"temp": 21.5, "history": [20.1, 20.4, nil, 21.5]})
widget.on("demo", def (name, value) print(name, value) end)
widget.remove("demo")   # true when it existed
```

- `widget.set(id, html [, order])` creates a widget or replaces its markup; its data stays. The id
  is 1–31 characters of `A-Z a-z 0-9 _ - .`; other ids raise `value_error`. `order` (integer)
  sorts widgets, equal orders keep registration order; without it a new widget gets 0 and an
  existing one keeps its order. Widgets appear after the native cards.
- `widget.data(id, value)` replaces the data of a widget that exists, with `json.dump(value)`.
- `widget.on(id, fn)` sets the callback for an existing widget's controls (an unknown id raises
  `value_error`); `widget.on(id, nil)` removes it. It runs on the Berry loop as `fn(name, value)`, both strings. An exception in the callback
  is logged; the widget and callback stay.
- `widget.viewers()` is true while a page has asked for widget updates within the last three
  seconds, so a script can skip work nobody sees.

**Markup is trusted, data is not.** The markup runs in the page with the page's rights, as in
Tasmota. Text that comes from elsewhere (BLE names, MQTT payloads, anything from the network)
belongs in `data()`, never in `set()`: the page writes data only as text, field values or chart
numbers. A widget's own JS should keep to that too, for example with `nm.rows`.

## Markup

The markup's single root element becomes the card; anything else is wrapped in a `div`.
Tasmota's `<div class="box w2 h2">` works as it is.

| Markup | Effect |
| --- | --- |
| `w2`, `w3`, `h2`, `h3` on the root | Span 2 or 3 grid columns or rows on wide screens |
| `data-v="key"` | Shows `data[key]`: as text, as an input's or select's value, or as a checkbox's state. `"a.b"` reaches into nested maps. A text or number field being edited, or a select with its menu open, is left alone |
| `data-chart="line"` with `data-v` | A line chart of a list of numbers, a list of lists, or a list of `{"values": [...], "color": 0xRRGGBB}`; `nil` is a gap |
| `data-chart="hist"` | Bars of a list of numbers |
| `data-chart="gauge"` | A half-circle gauge of a number; or of a map with `value`, `min`, `max`, `unit` and `stops` when the range moves |
| `data-chart="qr"` | A QR code of up to 25 QR alphanumeric characters (digits, `A`–`Z`, space, `$%*+-./:`) |
| `data-chart="matrix"` | Dots from rows of integers, top bit leftmost, with `data-size` dots per row (at most 32) |
| Chart attributes | `data-grid`, `data-min`, `data-max`, `data-height`, `data-labels="a,b,c"` (at most six shown), `data-color="#f93,#3cf"`, `data-unit`, `data-stops="0:#0c0,70:#ca0,90:#c00"` (a stop colours the range from its threshold upward) |
| `data-act="name"` | A click sends `""`; a checkbox sends `"true"`/`"false"`; a text, number or select field sends its value when changed (Enter or leaving the field), at most 63 bytes |
| `<input type="checkbox" class="toggle">` in a `<label>` | Drawn as the page's switch |
| `<button>` without a class | Drawn as the page's button |
| Classes | `value`, `unit`, `label`, `small`, `row` (a line of items), `scroll` (scrolls inside the card); `h2`–`h4` are card titles; tables, inputs and selects are styled |

The page does not change a control on its own beyond the user's input: the script confirms with
its next data, for example the new checkbox state. A failed send reverts a checkbox and shows
the error in the status line. Up to eight actions wait for the Berry loop; a full queue rejects
further ones.

## Scripts in markup

A `<script>` in the markup runs each time the markup is set, and again after a page reload or a
device restart. Wrap it in `(() => { ... })();` so its `let`, `const` and functions stay local:
top-level declarations would clash with the previous run and with other widgets. The page
offers:

| Call | Effect |
| --- | --- |
| `nm.on(id, fn)` | Calls `fn(data, card)` after every data update |
| `nm.send(id, name, value)` | Sends an action, as `data-act` does |
| `nm.card(id)` | The card element |
| `nm.line(series, options)`, `nm.hist(values, options)`, `nm.gauge(value, options)`, `nm.qr(text)` | Chart elements to place yourself |
| `nm.rows(tbody, rows)` | Fills a table body with text-only cells |

Data comes through `nm.on`; a widget script should not start timers or requests of its own.
Its CSS is global, so scope a `<style>` to `[data-id="<id>"]`, which the page sets on the card.

## Limits

16 widgets; markup at most 4096 bytes and data at most 2816 bytes of JSON per widget, otherwise
`set` or `data` raises `value_error` and the previous content stays. Content is copied;
changing a list after `data` does not change the widget. Widgets live in RAM and disappear when
the Berry VM goes away, so scripts create them at start.

Examples: [`examples/widget/widget_multi.be`](../../../../../examples/widget/widget_multi.be),
[`examples/widget/widget_ac.be`](../../../../../examples/widget/widget_ac.be),
[`examples/widget/widget_linegraph_scroll.be`](../../../../../examples/widget/widget_linegraph_scroll.be) and
[`examples/widget/widget_scanner.be`](../../../../../examples/widget/widget_scanner.be) (needs `USE_BLE`),
ports of the Tasmota `berry-examples/dashboard` scripts.
