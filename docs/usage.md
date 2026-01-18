# 2048 AI Usage Guide

This document explains how to run the 2048 AI binaries both from the command line
and over the built-in HTTP interface. It supplements `README.md` with concrete
examples, payload formats, and small Python helpers that stay within the GPLv3
license terms by invoking the program unmodified.

## Build and Artifacts

```bash
make
```

The default target produces:

- `./2048` – baseline executable that loads the smaller lookup tables.
- `./2048b` – identical entry point compiled with `-DBIG_TUPLES` to enable the
  larger lookup tables (stronger play, much higher memory footprint).

All usage described below applies to both binaries; pick the one that fits your
resource budget.

## Command-Line Modes

### Simulation runs

```
./2048 -d5 -i100 2001
```

Key flags parsed in `2048.cc:389`:

- `-d <depth>` – maximum search depth (`options.max_depth`). Also adjusts the
  minimum probability via `Options::UpdateMinProbFromDepth`.
- `-i <games>` – number of games to run in batch (`options.iterations`).
- `-p <prob>` – override the minimum branch probability (`options.min_prob`).
- `-s <threshold>` – save threshold for tuple tables (`options.save_threshold`).
- `-v` – verbose mode; prints every board and chosen move.
- Seed handling – any positional argument after the flags overrides
  `options.seed` so runs can be reproduced.
- `-T` – disable tuple lookup moves and force pure search.
- `-J <prefix>` – log structured data under `<prefix>_gameNNNNNN`: each game
  gets its own step-level `.jsonl` (one move per line) plus a matching
  `.meta.json` summary.
- `-Z` – when used with `-J`, gzip each game's `.jsonl` payload on close,
  producing `.jsonl.gz` files while leaving metadata plain JSON.
- `-F <games>` – emit a progress message every `<games>` completed (default 100);
  set to 0 to silence progress updates.
- `-q` – quiet mode; suppresses the usual board dumps and progress summaries so
  the JSON logs are the only output.

Results are printed to stdout after each run. With `-v`, every intermediate
board is shown using the format defined in `Node::Show()`.

### Parallel self-play logging

```bash
scripts/run_selfplay.sh -n 8 -g 200 -d 5 -o selfplay_logs/depth5_batch1
```

- `scripts/run_selfplay.sh` launches multiple CLI runs in parallel and invokes
  the engine's `-J` option so every game produces its own step-level `.jsonl`
  file (board exponents, action, EV-per-branch) plus a `.meta.json` summary.
- `-n` picks how many workers to spawn (defaults to detected CPU cores).
- `-g` controls how many games each worker runs (`-i` flag under the hood);
  total games ≈ `n * g`.
- `-d` passes the search depth through to the engine so the logged valuations
  match the policy you plan to imitate.
- `-o` places the per-game `.jsonl` and `.meta.json` outputs in the given
  directory; file names encode the worker, seed, and game index for easy
  bookkeeping.
- `-s` overrides the randomized base seed (optional). When omitted the script
  draws a high-entropy base value and reserves a unique block of `g` seeds per
  worker, so concurrent runs never collide. Add `-z` to write each game's `.jsonl` as a gzip-compressed
  `.jsonl.gz` while keeping the accompanying `.meta.json` files uncompressed.
- Each worker still prints a brief progress heartbeat (default every 100 games)
  to `stderr` so long runs remain visible even in `-q` mode; tune it with `-F`.

Each move is flushed as its own JSON object, so concatenating the individual
`.jsonl` files yields a replay-ready pool of steps. Sidecar `.meta.json` files
capture per-game aggregates (score, max tile, total moves, runtime).

Example step (prettified for clarity):

```json
{
  "seed": 123456789,
  "step_index": 117,
  "max_rank": 12,
  "move": "left",
  "valuation_type": "tuple11",
  "valuation": 0.998524,
  "board": [
    15,14,10, 5,
    13,11, 9, 3,
     8, 7, 4, 1,
     3, 2, 1, 0
  ],
  "branch_evs": {
    "up": 0.642325,
    "left": 0.998524,
    "right": 0.213456,
    "down": null
  }
}
```

Valuation sources:
- tuple10/tuple11 lookup tables emit probabilities in [0, 1).
- search valuations come from depth-limited expectimax; the JSON log stores `value/1000`, so numbers can exceed 1.0 and even go negative when the position is bad.
- block_plan and line_plan heuristics return probabilities on the same scale as tuple lookups.

Corresponding metadata:

```json
{
  "seed": 123456789,
  "depth": 5,
  "game_index": 0,
  "steps_file": "..._game000000.jsonl",
  "num_moves": 142,
  "score": 689432,
  "max_tile": 32768,
  "max_rank": 15,
  "sum_tile": 131072,
  "seconds": 4.812500
}
```

### Interactive suggestion mode

```
./2048 -d5 -I 2001
```

- `-I` turns on `options.interactive`. The AI still plays the game but pauses for
  keyboard input (see `InteractivePlay`).
- The terminal prints the current board and the recommended move with an
  estimated probability. Press `Space` to accept, `W/A/S/D` (or arrow-key
  equivalents) to override, `U` to undo, and `Q` to exit.

### Critiquing moves from logs

```
./2048 -L path/to/game.log -O 0.95
```

- `-L` loads a text log and feeds it to `AnalyzeLog`. This procedure recreates
  the game locally and prints a warning whenever a recorded move falls below the
  optimality threshold.
- `-O <ratio>` sets the acceptable sub-optimality margin. The default is `0.9`.
- `-R <max-rank>` can be used to stop analysis once a tile of the given rank is
  reached.

`AnalyzeLog` expects the [2048league](https://2048league.ml/) replay encoding.
Each character encodes the move that was taken and the random tile that
appeared afterward. When the AI detects a significantly better move, it prints
an annotated board followed by a line such as:

```
***** left 0.786 < up 1.000 *****
```

Use `/dev/stdin` to critique logs piped in from another process:

```bash
cat game.log | ./2048 -L /dev/stdin -O 0.95
```

#### Minimal Python helper to encode custom logs

The snippet below demonstrates how to encode a short sequence (initial two
spawns followed by one player move) into 2048league ASCII so it can be piped to
`-L`. Extend it to export your own datasets.

```python
#!/usr/bin/env python3
# Helper script that emits a tiny 2048league-compatible log so it can be
# inspected with `./2048 -L /dev/stdin`.
ASCII = [chr(i + 32) for i in range(96)] + [
    chr(code)
    for code in (
        0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
        0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
        0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
        0x00FF, 0x00D6, 0x00DC, 0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x0192,
        0x00E1,
    )
]
MOVE_BITS = {0: 0, 1: 3, 2: 1, 3: 2}  # up, left, right, down -> encoded field

def encode(move, x, y, is_four):
    code = x * 4 + y  # location bits
    if is_four:
        code += 16
    code += MOVE_BITS.get(move, 0) * 32
    return ASCII[code]

if __name__ == "__main__":
    # Example: spawn two tiles, then log a LEFT move that causes a new 2 at (3,3).
    sequence = [
        encode(0, 0, 0, False),  # initial 2 in the top-left
        encode(0, 1, 0, False),  # second spawn
        encode(1, 3, 3, False),  # player moved LEFT, new 2 appears at bottom-right
    ]
    print(''.join(sequence))
```

Pipe its output into `./2048 -L` to observe the same critique pipeline that the
HTTP API uses.

### Python subprocess example

```python
#!/usr/bin/env python3
import subprocess

command = ["./2048", "-L", "/dev/stdin", "-O", "0.95"]
log_data = " $É"  # Replace with data from your generator or a real replay.
result = subprocess.run(
    command,
    input=log_data.encode(),
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    check=True,
)
print(result.stdout.decode())
```

This code executes the unmodified GPL program and leaves interpretation of the
output to your own application, keeping your wrapper outside the derivative-work
boundary.

## HTTP (REST-style) Interface

Start the agent as a TCP server:

```
./2048 -S 8080 -d5 -O 0.95 -I
```

- `-S <port>` enables `RunServer`. The AI prints `Server ready` when the socket is
  listening.
- All other CLI flags still apply. For example, `-d` picks the search depth,
  `-T` toggles tuple lookups, and `-O` sets the critique sensitivity used by the
  `POST /analyze` endpoint.
- There is no per-request override: the depth, probability thresholds, and
  tuple settings are fixed for the lifetime of the process.
- Requests are processed sequentially per connection, but the server handles
  multiple clients by spawning a detached thread for each accepted socket.

### Board encoding

The `board` query parameter is a 16-character string laid out row by row from the
upper-left corner. Every character encodes a tile rank (log2 of the tile value).
Use the map below:

| Character | Rank | Tile |
|-----------|------|------|
| `0`       | 0    | empty |
| `1`       | 1    | 2 |
| `2`       | 2    | 4 |
| `3`       | 3    | 8 |
| `4`       | 4    | 16 |
| `5`       | 5    | 32 |
| `6`       | 6    | 64 |
| `7`       | 7    | 128 |
| `8`       | 8    | 256 |
| `9`       | 9    | 512 |
| `A`/`a`   | 10   | 1024 |
| `B`/`b`   | 11   | 2048 |
| `C`/`c`   | 12   | 4096 |
| `D`/`d`   | 13   | 8192 |
| `E`/`e`   | 14   | 16384 |
| `F`/`f`   | 15   | 32768 |
| `G`/`g`   | 16   | 65536 |

Example board string `EDC1BA9187611111` (taken from `README.md`) corresponds to
this grid:

```
E D C 1
B A 9 1
8 7 6 1
1 1 1 1
```

### Endpoints

#### `GET /move?board=<tiles>`

Returns the AI’s recommended move as a single character in the HTTP body:

- `u`, `l`, `r`, `d` – up, left, right, down.
- `g` – no valid move (game over).

Sample transaction (line breaks added for clarity):

```
GET /move?board=EDC1BA9187611111 HTTP/1.1
Host: localhost:8080

HTTP/1.1 200 OK
Access-Control-Allow-Origin: *
Content-Type: text/plain
Content-Length: 1

l
```

No batching is available; send one board per request.

#### `POST /analyze?board=<tiles>&direction=<u|l|r|d>`

Registers an actual move and prints any critique to the server’s stdout. The HTTP
response body is empty (`Content-Length: 0`). Example:

```
POST /analyze?board=EDC1BA9187611111&direction=d HTTP/1.1
Host: localhost:8080
Content-Length: 0

HTTP/1.1 200 OK
Access-Control-Allow-Origin: *
Content-Type: text/plain
Content-Length: 0
```

If the reported move is worse than the AI’s suggestion by more than the `-O`
threshold, the server prints a message like the CLI example above. This is ideal
for rejection-sampling loops in reinforcement learning pipelines.

### Python `requests` example

```python
#!/usr/bin/env python3
import requests

BASE = "http://localhost:8080"
BOARD = "EDC1BA9187611111"  # Replace with your board string

move = requests.get(f"{BASE}/move", params={"board": BOARD}, timeout=5)
move.raise_for_status()
print("Suggested move:", move.text.strip())

# Suppose an external agent played DOWN; send it back for critique.
critique = requests.post(
    f"{BASE}/analyze",
    params={"board": BOARD, "direction": "d"},
    timeout=5,
)
critique.raise_for_status()
# Any warnings are printed in the server's stdout; the client body is empty.
```

The example keeps the GPL-covered engine in a separate process and talks to it
through simple HTTP calls, a common strategy for keeping proprietary orchestration
code outside the derivative scope.

## Troubleshooting

- On the first run the tuple tables (`tuple_moves.10a`, `tuple_moves.11a`) are
  generated alongside the executable; later runs load them instantly.
- If `GET /move` ever responds with `g`, the board is either terminal or invalid.
- When using `./2048b`, be prepared for multi-gigabyte memory usage.

## Next Steps

- Record server stdout when using `POST /analyze` so you can programmatically
  harvest critique signals.
- Wrap the CLI or HTTP interface in your own GPL-compatible tooling to persist
  game states for curriculum learning or rejection sampling.
