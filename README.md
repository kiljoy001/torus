# torus

An ASCII toy for 9front: a shape spins at the decode rate reported by a
local llama.cpp server, and the harness's recent actions ripple across
it as coloured bands.

The point is to make a local model's state visible while it works —
idle, prefilling, decoding, or offline — without a GPU load graph, and
to show what kind of work the harness is doing without spawning a
separate object for every call.

## Build

```rc
mk
```

## Run

```rc
torus
```

`q` (or delete) quits.

| flag | meaning |
| --- | --- |
| `-g shape` | `torus` (default), `cube`, `pyramid`, `tetra`, or `octa` |
| `-u baseurl` | llama.cpp server, default `$oaiurl` or `http://127.0.0.1:8080` |
| `-f toolfeed` | action feed, default `$oaitoolfeed` or `/tmp/oai.toolcalls` |
| `-d seed` | deterministic initial orientation (default: time) |
| `-t` | print one frame to stdout and exit |
| `-a` | seed one sample action of each class at startup |
| `-v` | log model updates and actions to stderr |
| `-c action` | print the colour class for an action name and exit |
| `-p line` | parse one feed line and print its fields |
| `-s` | print the status line 20 times and exit |

Examples:

```rc
torus -g cube
torus -g octa -a
torus -t -g pyramid -d 1
torus -p 'agent task start'
torus -c web_search
```

## How it works

Every 300 ms a monitor proc polls the server's `/slots` endpoint for
`timings.predicted_per_second` and the prompt/decode counters, and tails
the action feed. Updates cross a channel to the UI thread, so a slow or
hung HTTP read stalls only the monitor; the shape keeps spinning and the
window stays responsive.

The spin rate follows decode speed. If the server is unreachable the
shape turns red and the status line says so.

## The action feed

`oai` appends one line per action to `$oaitoolfeed`. Torus tails the file
from its current end, so only actions that happen while it runs appear.
Three line forms are accepted (whitespace-separated):

```text
read                    legacy: start, class guessed from the name
read start              name and state
agent task start        class, name, state
net web_search error    class, name, state
```

States are `start`, `end`, `done`, `ok`, `manual`, `error`, and `fail`.
A `start` adds a band to the wave. A completion (`end`, `done`, `ok`,
`manual`) or an error recolours the newest matching action instead of
adding a second band; an error turns that band red. If no matching start
is in the buffer, the line still appears as a band.

The leading class is optional. When present it must be one of the classes
below; otherwise the name is classified heuristically by prefix. That
lets a harness say exactly what it means instead of relying on tool
names.

The current `oai` emits `name start`, then `name ok`, `name manual`,
or `name error` for each tool call. A different harness can use the
three-field form to name the class explicitly.

## Classes and colours

| class | actions | colour |
| --- | --- | --- |
| `cmd` | `cmd`, `plumb`, `man`, `window`, `rc`, `exec`, `shell`, `run`, `command` | amber |
| `read` | `read`, `list`, `glob`, `grep`, `find`, `open`, `search` | cyan |
| `write` | `write`, `edit`, `patch`, `create`, `apply` | green |
| `net` | `fetch`, `searx`, `web`, `curl`, `hget` | violet |
| `agent` | `agent`, `task`, `delegate`, `summar` | pink |
| `mem` | `memory`, `skill`, `todo`, `plan` | yellow |
| `view` | `image`, `screen`, `view`, `screenshot` | pale blue |
| `tool` | anything else | grey |
| `err` | any action with state `error` or `fail` | red |

The key is drawn above the status line, with `*` on the most recent
class. The status line shows the model state and the last three action
names, and is tinted with the most recent action for 2.5 seconds.

## Waves

The last 32 actions are kept in a ring buffer for 10 seconds. On every
frame, each cell of the shape picks a buffer slot from its distance to
the centre, minus a moving phase. Advancing the phase scrolls the colour
bands outward, so new work enters at the centre and older colours drift
toward the edge. Changes to a band (a completion or an error) recolour
the existing band rather than inserting a new one.

With no recent actions the whole shape falls back to the model state
colour: green while decoding, amber while prefilling, red when offline,
dim when idle.

## Scaling

The renderer scales the shape to fill about 85% of the limiting
dimension, using the font's cell aspect ratio so the projection stays
circular in pixels. The grid goes up to 600x160 characters. When the
window grows, the shape grows with it; when the window is wide and short,
height is the limit and the shape stays circular.

The scene is recomputed on resize and the view is centred. Runs at 30 fps
while the model is working or a wave is moving, and at 10 fps when idle
with no recent actions.

## Files

- `torus.c` — window, channel, polling, feed parsing, waves, status lines
- `scene.c`, `scene.h` — 3D ASCII renderer, shape selection, scaling
- `mkfile`
