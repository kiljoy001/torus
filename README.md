# torus

An ASCII toy for 9front: a torus spins at the decode rate reported by a
local llama.cpp server, and a shape bounces for every tool call `oai`
makes.

The point is to make a local model's state visible while it works —
idle, prefilling, decoding, or offline — without a GPU load graph.

## Build

```rc
mk
```

## Run

```rc
torus
```

`q` (or delete) quits.

Options:

| flag | meaning |
| --- | --- |
| `-u baseurl` | llama.cpp server, default `$oaiurl` or `http://127.0.0.1:8080` |
| `-f toolfeed` | tool-call feed, default `$oaitoolfeed` or `/tmp/oai.toolcalls` |
| `-t` | print one frame to stdout and exit |
| `-a` | spawn one of each sample tool shape at startup (also with `-t`) |
| `-s` | print the status line 20 times and exit |

## How it works

Every 300 ms torus polls the server's `/slots` endpoint for
`timings.predicted_per_second` and the prompt/decode counters. The spin
rate follows decode speed. The status line shows the state and the last
three tool names.

`oai` appends one line per requested tool to `$oaitoolfeed`. Torus tails
that file and maps tool names to shapes:

| tools | shape |
| --- | --- |
| `cmd`, `man`, `plumb` | cube |
| `read`, `write`, `list`, `memory` | pyramid |
| `fetch`, `searxng` | octahedron |
| `agent`, `summarize` | tetrahedron |

At startup only the torus is shown. Shapes appear as tool calls arrive;
`torus -a` seeds one of each so the renderer can be checked without
running `oai`.

The view follows the window. Resizing recomputes the character grid
(up to 240x80 cells) and scales the scene uniformly, so a larger window
shows a larger, more detailed torus. The status line stays at the
bottom of the grid.

## Files

- `torus.c` — window, polling, feed tailing, status line
- `scene.c`, `scene.h` — 3D ASCII renderer and shape state
- `mkfile`
