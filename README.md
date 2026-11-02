<div align="center">

# 🖥️ Terminator

**A terminal emulator *and* a shell, written from scratch in C for Linux.**

![Language](https://img.shields.io/badge/language-C-00599C?style=for-the-badge&logo=c&logoColor=white)
![Platform](https://img.shields.io/badge/platform-Linux-FCC624?style=for-the-badge&logo=linux&logoColor=black)
![GUI](https://img.shields.io/badge/GUI-Xlib%20%2F%20X11-orange?style=for-the-badge)
![Dependencies](https://img.shields.io/badge/dependencies-libX11%20only-brightgreen?style=for-the-badge)

[Quick Start](#-quick-start) •
[Features](#-features) •
[Keyboard](#%EF%B8%8F-keyboard-reference) •
[Config](#%EF%B8%8F-configuration) •
[Limitations](#-known-limitations) •
[Testing](#-how-this-was-tested)

</div>

---

Terminator draws its own X11 window with multiple tabs. Every tab is an independent shell that runs commands with `fork`/`execvp`, pipes, redirection, job control, history and tab-completion — and it **interprets bash script files itself** instead of handing them to `/bin/bash`.

Everything is built on the system calls the course covers (`fork`, `execvp`, `waitpid`, `pipe`, `dup2`, `open`/`read`/`write`, `select`/`poll`, `signal`/`kill`) together with POSIX threads, semaphores and shared memory.

## 📑 Table of Contents

1. [Quick Start](#-quick-start)
2. [Features](#-features)
3. [Keyboard Reference](#%EF%B8%8F-keyboard-reference)
4. [Configuration](#%EF%B8%8F-configuration)
5. [Known Limitations](#-known-limitations)
6. [Repository Contents](#-repository-contents)
7. [Demo](#-demo)
8. [How This Was Tested](#-how-this-was-tested)

---

## 🚀 Quick Start

### 1. Install dependencies

| Requirement | Purpose |
|---|---|
| `gcc` or `clang` | build |
| `make` | build |
| **`libx11-dev`** (Xlib headers + library) | the GUI |
| POSIX threads (`libpthread`, part of glibc) | thread mode |
| An X server (`Xorg`, `Xwayland`, …) | to run it |

```sh
# Debian / Ubuntu
sudo apt install build-essential libx11-dev

# Fedora / RHEL
sudo dnf install gcc make libX11-devel

# Arch
sudo pacman -S base-devel libx11
```

> No ncurses, no GTK, no third-party libraries.

### 2. Build

```sh
make
```

This produces `./terminator`. Under the hood it runs:

```sh
gcc -O2 -std=gnu11 -Wall -Wextra -pthread -o terminator terminator.c -lX11 -pthread
```

`make clean` removes the binary.

### 3. Run

```sh
./terminator                  # thread mode (default): one process, one thread per tab
./terminator useProcess=True  # process mode: one process per tab, GUI in the parent
```

| Mode | Tabs | History | Isolation |
|---|---|---|---|
| **Thread** (default) | one thread per tab, shared address space | protected by a semaphore | — |
| **Process** | one process per tab, talking to the GUI over pipes | shared memory + process-shared semaphore | a crash in one tab can't affect the others |

Both modes behave identically from the user's point of view. Type `exit` (or press <kbd>Ctrl</kbd>+<kbd>D</kbd>) to close a tab; closing the last tab exits.

---

## ✨ Features

All eleven tasks of the brief are implemented.

| # | Feature | Highlights |
|:-:|---|---|
| 1 | **🪟 X11 GUI with tabs** | Xlib window, tab bar showing each tab's current directory, scrollback, mouse-wheel scrolling, click to switch/close, `[+]` to open. A single `poll()` over the X connection and every tab's pipes — no busy-waiting. |
| 2 | **🔧 Builtins & external commands** | External commands run via `fork` + `execvp`. Builtins: `cd` `exit` `history` `multiWatch` `watch` `jobs` `fg` `bg` `source`/`.` `export` `pwd` `clear` `terminator` `[`/`test` `true` `false`. `$?` holds the last status; unknown commands print `terminator: name: command not found`. |
| 3 | **📝 Multiline input & quoting** | Trailing `\` continuation, quotes spanning lines, `$(...)` and backticks (nested, innermost first), bash quoting rules, `$VAR` `${VAR}` `$?` `$#` `$1…`, quote-aware globbing. Incomplete input shows a `> ` prompt. |
| 4 | **↪️ Redirection** | `<` `>` `>>` `2>` `2>>` and combinations (`./a.out < in > out`), applied with `dup2` in the child. A missing input file is reported instead of running the command. |
| 5 | **🔗 Pipes** | Pipelines of any length (N commands, N−1 pipes, N children), combinable with redirection. |
| 6 | **👀 multiWatch** | `multiWatch ["cmd1", "cmd2"]` runs each command in parallel with its own pipe, multiplexed by `poll()`, prints in the required `"cmd", <time>:` / dashed-rule format, re-runs every 2 s, and kills every child on <kbd>Ctrl</kbd>+<kbd>C</kbd> while the shell survives. `watch "cmd"` re-runs a single command. |
| 7 | **⌨️ Hotkeys** | Line editing, history, clipboard and tab shortcuts — all rebindable in `~/.terminatorrc` or with `terminator keybind`, validated and persistent. See the [keyboard reference](#%EF%B8%8F-keyboard-reference). |
| 8 | **🚦 Signals & job control** | <kbd>Ctrl</kbd>+<kbd>C</kbd> (`SIGINT`) and <kbd>Ctrl</kbd>+<kbd>Z</kbd> (`SIGTSTP`) are sent by the shell with `kill()` to the foreground job's process group. `cmd &`, `jobs`, `fg %1`, `bg %1` with bash-style `[1]+  Stopped   sleep 100` output. Terminator itself can never be stopped. |
| 9 | **🕘 Searchable history** | 10,000 commands in `~/.terminator_history`, shared by all tabs; `history` prints the latest 1,000. <kbd>Ctrl</kbd>+<kbd>R</kbd> opens `Enter search term: ` and prints the most recent exact match, else commands sharing the longest common substring (> 2 chars), else `No match for search term in history`. |
| 10 | **⇥ Tab completion** | Completes the word before the cursor from the tab's current directory: one match completes it, several extend to the longest common prefix and show a numbered list to pick from, no match leaves the line alone. |
| 11 | **📜 Bash scripts** | `./script.sh`, `bash script.sh` and `source script.sh` run in Terminator's **own interpreter** — never `/bin/bash`. Supports comments, pipes, redirection, command substitution, variables, `$1…`/`$#`/`$?`, nested `if`/`elif`/`else`/`fi` with `[ ... ]` tests, and `exit [n]`. Syntax errors (e.g. a missing `fi`) are reported instead of running the script. |

---

## ⌨️ Keyboard Reference

### Line editing

| Key | Action |
|---|---|
| <kbd>←</kbd> / <kbd>→</kbd> | Move within the line |
| <kbd>Ctrl</kbd>+<kbd>A</kbd> / <kbd>Ctrl</kbd>+<kbd>E</kbd> | Start / end of line |
| <kbd>↑</kbd> / <kbd>↓</kbd> | Browse history |
| <kbd>Ctrl</kbd>+<kbd>U</kbd> / <kbd>Ctrl</kbd>+<kbd>K</kbd> | Delete to start / end of line |
| <kbd>Ctrl</kbd>+<kbd>W</kbd> | Delete previous word |
| <kbd>Tab</kbd> | Complete a file name |
| <kbd>Ctrl</kbd>+<kbd>R</kbd> | Search history |

### Shell control

| Key | Action |
|---|---|
| <kbd>Ctrl</kbd>+<kbd>C</kbd> | Interrupt the running command |
| <kbd>Ctrl</kbd>+<kbd>Z</kbd> | Suspend the running command |
| <kbd>Ctrl</kbd>+<kbd>D</kbd> | Exit (close the tab) |
| <kbd>Ctrl</kbd>+<kbd>L</kbd> | Clear the screen |

### Window, tabs & clipboard

| Key | Action |
|---|---|
| <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>T</kbd> / <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>W</kbd> | New / close tab |
| <kbd>Ctrl</kbd>+<kbd>Tab</kbd> / <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>Tab</kbd> | Next / previous tab |
| <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>C</kbd> / <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>V</kbd> | Copy / paste (X11 selections; middle-click pastes PRIMARY) |
| <kbd>Page Up</kbd> / <kbd>Page Down</kbd> | Scroll |

---

## ⚙️ Configuration

| File | Purpose |
|---|---|
| `~/.terminatorrc` | Key bindings, one `bind <keyspec> <action>` per line (see `terminatorrc.sample`). Takes effect without recompiling and survives restarts. An invalid or conflicting line is reported and the default is kept. |
| `~/.terminator_history` | The shared command history (10,000 entries). |

**Useful commands**

```sh
terminator keybinds              # list bindings
terminator keybind Ctrl+G eol    # rebind and save
terminator reload                # reload ~/.terminatorrc
terminator history clear         # wipe history
```

---

## ⚠️ Known Limitations

<details>
<summary><b>Click to expand</b></summary>

<br>

- **Command substitution** and the children of `multiWatch`/`watch` run through `/bin/sh -c` so pipes and quoting inside them work. The scripts themselves, the shell's own pipelines, redirection and globbing are implemented directly — `bash script.sh` never invokes bash.
- **Scripts:** no loops or functions (not required by the brief), no arrays, no `case`, no here-documents. `[ ... ]` supports `-f -d -e -r -w -x -s -z -n`, `=`, `!=`, `-eq -ne -lt -le -gt -ge` and `!`.
- **`2>&1`** and `<<` here-documents are tokenised but not wired up.
- **Type-ahead** is not buffered: keys pressed while a command runs are ignored (control and tab hotkeys still work).
- Output is handled at byte level (no UTF-8 glyph composition) and very long lines are clipped rather than wrapped.
- No controlling terminal is allocated, so `vim`/`top`-style full-screen programs are not supported — out of scope for this brief.

</details>

---

## 📦 Repository Contents

| File | Purpose |
|---|---|
| `terminator.c` | The entire program |
| `Makefile` | `make` builds `./terminator` |
| `README.md` | This file |
| `DESIGN.md` | Design document (architecture, modes, IPC, data structures, signals) |
| `terminatorrc.sample` | Example key-binding config |
| `example-check.sh`, `example-demo.sh` | Demo scripts to run inside Terminator |

---

## 🎬 Demo

Inside Terminator:

```text
user@terminator> ls | wc -l
user@terminator> echo "Files in $(basename $(pwd)): $(ls | wc -l)"
user@terminator> sort < somefile.txt > sorted.txt
user@terminator> multiWatch ["date +%T", "ls | wc -l"]      # Ctrl+C to stop
user@terminator> sleep 100                                   # Ctrl+Z, then: bg %1, jobs, fg %1
user@terminator> ./example-check.sh README.md
user@terminator> bash example-check.sh nothere.c
```

---

## 🧪 How This Was Tested

There is no X server in the build environment, so the program was verified in two ways:

1. **Shell engine harnesses** — `execute_line()` and the worker's byte protocol were driven directly.
2. **Real GUI event loop, headless** — run against a small stand-in for Xlib with scripted events.

Both thread mode and process mode pass the full suite, which covers every example in the brief: pipelines, redirection, multiWatch's exact output format, <kbd>Ctrl</kbd>+<kbd>C</kbd>/<kbd>Ctrl</kbd>+<kbd>Z</kbd> job control, `jobs`/`fg`/`bg`, <kbd>Ctrl</kbd>+<kbd>R</kbd> search, Tab completion, and script interpretation. See `DESIGN.md` §16 for details.
