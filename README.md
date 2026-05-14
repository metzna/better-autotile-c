# better-autotile

Smart autotiling for i3, written in C.

Automatically sets the split direction based on each window's dimensions using a mark-and-move approach: the previously focused window is marked, and when a new window opens it gets placed next to it via `move to mark`.

## Dependencies

- i3
- gcc

cJSON is vendored — no external libraries needed.

## Build & Install

```bash
make
sudo make install
```

Or download the pre-built binary from the [releases page](../../releases).

## Usage

```bash
better_autotile            # run normally
better_autotile -d         # debug output
better_autotile -v         # print version
```

Add to your i3 config:

```
exec_always --no-startup-id pkill better_autotile; better_autotile
```
