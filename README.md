<div align="center">

# 🖥️ Terminator

### A terminal emulator *and* a shell, written from scratch in C for Linux.

**One window. Many tabs. Its own shell. Its own bash-script interpreter. Zero third-party GUI toolkits.**

![Language](https://img.shields.io/badge/language-C11-00599C?style=for-the-badge&logo=c&logoColor=white)
![Platform](https://img.shields.io/badge/platform-Linux-FCC624?style=for-the-badge&logo=linux&logoColor=black)
![GUI](https://img.shields.io/badge/GUI-Xlib%20%2F%20X11-orange?style=for-the-badge)
![Dependencies](https://img.shields.io/badge/dependencies-libX11%20only-brightgreen?style=for-the-badge)
![Modes](https://img.shields.io/badge/modes-thread%20%7C%20process-blueviolet?style=for-the-badge)

[Why](#-why-terminator) •
[Quick Start](#-quick-start) •
[Features](#-features) •
[Cheat Sheet](#-cheat-sheet) •
[Architecture](#-architecture) •
[Config](#%EF%B8%8F-configuration) •
[Scripting](#-scripting-language-reference) •
[Limitations](#%EF%B8%8F-known-limitations) •
[Testing](#-testing) •
[Troubleshooting](#-troubleshooting)

</div>

---

## 💡 Why Terminator?

Most terminals are thin windows around `/bin/bash`. Terminator is different: **it is the window, the tab manager, the shell, the job controller, the history engine and the script interpreter, all in one small C codebase.**

- **No toolkit bloat.** The only library dependency is `libX11`. No GTK, no Qt, no ncurses.
- **Two concurrency models, one behavior.** Run each tab as a *thread* or as an isolated *process* with a single flag.
- **Scripts run in-house.** `./script.sh`, `bash script.sh` and `source script.sh` are interpreted by Terminator itself and never hand off to `/bin/bash`.
- **Built on the fundamentals.** `fork`, `execvp`, `waitpid`, `pipe`, `dup2`, `poll`, `kill`, POSIX threads, semaphores and shared memory. Readable systems code.
- **Fully rebindable.** Every hotkey is configurable, validated and persistent.

---

## 📑 Table of Contents

1. [Quick Start](#-quick-start)
2. [Features](#-features)
3. [Cheat Sheet](#-cheat-sheet)
4. [Architecture](#-architecture)
5. [Configuration](#%EF%B8%8F-configuration)
6. [Scripting Language Reference](#-scripting-language-reference)
7. [Feature Removed in This Build](#-feature-removed-in-this-build)
8. [Known Limitations](#%EF%B8%8F-known-limitations)
9. [Repository Layout](#-repository-layout)
10. [Demo](#-demo)
11. [Testing](#-testing)
12. [Troubleshooting](#-troubleshooting)
13. [Contributing & Roadmap](#-contributing--roadmap)

---

## 🚀 Quick Start

### 1. Install dependencies

| Requirement | Purpose |
|---|---|
| `gcc` or `clang` | Compiler |
| `make` | Build driver |
| **`libx11-dev`** (Xlib headers + library) | GUI |
| POSIX threads (`libpthread`, part of glibc) | Thread mode |
| An X server (`Xorg`, `Xwayland`, …) | Needed at run time |

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

Each module compiles to an object file and links into `./terminator`. The build is equivalent to:

```sh
gcc -O2 -std=gnu11 -Wall -Wextra -pthread -c main.c -o main.o
# ... the same step for every module ...
gcc -O2 -std=gnu11 -Wall -Wextra -pthread -o terminator *.o -lX11 -pthread
```

`make clean` removes the object files and the binary.

### 3. Run

```sh
./terminator                  # Thread mode (default): one process, one thread per tab
./terminator useProcess=True  # Process mode: one process per tab, GUI in the parent
```

### 4. Choose a mode

| | **Thread mode** (default) | **Process mode** |
|---|---|---|
| Tab = | one thread | one process |
| Memory | shared address space | isolated |
| GUI ⇄ tab link | in-memory | pipes |
| History protection | semaphore | shared memory + process-shared semaphore |
| A tab crashes… | may affect the whole app | other tabs are unaffected |
| Best for | low overhead | robustness |

Both modes behave identically from the user's point of view.

Type `exit` or press <kbd>Ctrl</kbd>+<kbd>D</kbd> to close a tab. Closing the last tab exits Terminator.

---

## ✨ Features

### At a glance

| | Feature | Highlights |
|:-:|---|---|
| 🪟 | **X11 GUI with tabs** | Own Xlib window, per-tab current directory in the tab bar, scrollback, mouse-wheel scrolling, click to switch/close, `[+]` to open a tab. |
| 🔧 | **Builtins & external commands** | `fork` + `execvp`, 15+ builtins, `$?` exit status, bash-style "command not found" errors. |
| 📝 | **Multiline input & quoting** | `\` continuation, quotes spanning lines, nested `$(...)` and backticks, `$VAR`/`${VAR}`/`$?`/`$#`/`$1…`, quote-aware globbing. |
| ↪️ | **Redirection** | `<`, `>`, `>>`, `2>`, `2>>` and combinations, applied with `dup2` in the child. |
| 🔗 | **Pipes** | Pipelines of any length (N commands, N−1 pipes), combinable with redirection. |
| 👀 | **multiWatch / watch** | Parallel commands multiplexed by `poll()`, refreshed every 2 seconds, clean teardown on <kbd>Ctrl</kbd>+<kbd>C</kbd>. |
| ⌨️ | **Rebindable hotkeys** | Line editing, history search, clipboard and tab shortcuts, configurable in `~/.terminatorrc`. |
| 🚦 | **Signals & job control** | `Ctrl+C`, `Ctrl+Z`, `&`, `jobs`, `fg`, `bg`, process-group delivery. |
| 🕘 | **Searchable shared history** | 10,000 entries shared by every tab, with fuzzy `Ctrl+R` search. |
| 📜 | **Built-in script interpreter** | Runs bash-subset scripts itself, with `if`/`elif`/`else`, tests, variables and syntax checking. |

### Details

<details>
<summary><b>🪟 GUI and tabs</b></summary>

<br>

- A single `poll()` watches the X connection and every tab's pipes together. There is **no busy-waiting**.
- The tab bar shows each tab's current working directory.
- Scroll with the mouse wheel or <kbd>Page Up</kbd>/<kbd>Page Down</kbd>.
- Click a tab to switch to it, click its close control to close it, or click `[+]` for a new one.
- Clipboard uses X11 selections, and middle-click pastes PRIMARY.

</details>

<details>
<summary><b>🔧 Builtins and command execution</b></summary>

<br>

External commands run via `fork` + `execvp`. Builtins:

`cd` · `exit` · `history` · `multiWatch` · `watch` · `jobs` · `fg` · `bg` · `source` / `.` · `export` · `pwd` · `clear` · `terminator` · `[` / `test` · `true` · `false`

- `$?` holds the last exit status.
- Unknown commands print `terminator: <name>: command not found`.

</details>

<details>
<summary><b>📝 Input, quoting and expansion</b></summary>

<br>

- Trailing `\` continues a line, and single or double quotes may span lines. Incomplete input shows a `> ` continuation prompt.
- Command substitution with `$(...)` and backticks, nested with the innermost evaluated first.
- Bash quoting rules, plus `$VAR`, `${VAR}`, `$?`, `$#` and positional parameters `$1…`.
- Globbing respects quoting, so quoted wildcards stay literal.

</details>

<details>
<summary><b>↪️ Redirection and 🔗 pipes</b></summary>

<br>

- Supported: `<`, `>`, `>>`, `2>`, `2>>`, for example `./a.out < in > out`.
- Redirections are applied with `dup2` in the child process.
- A missing input file is **reported**, and the command is not run.
- Pipelines of N commands use N−1 pipes and N children, and may be combined with redirection.

</details>

<details>
<summary><b>👀 multiWatch and watch</b></summary>

<br>

```text
multiWatch ["cmd1", "cmd2", ...]
```

- Every command runs in parallel with its own pipe, multiplexed by `poll()`.
- Output is printed per command in the required `"cmd", <time>` format, followed by a 52-dash rule.
- Commands re-run every **2 seconds**.
- <kbd>Ctrl</kbd>+<kbd>C</kbd> terminates every child, and the shell itself survives.
- `watch "cmd"` does the same for a single command.

</details>

<details>
<summary><b>🚦 Signals and job control</b></summary>

<br>

- <kbd>Ctrl</kbd>+<kbd>C</kbd> (`SIGINT`) and <kbd>Ctrl</kbd>+<kbd>Z</kbd> (`SIGTSTP`) are delivered by the shell with `kill()` to the **foreground job's process group**.
- `cmd &` starts a background job. `jobs`, `fg %N` and `bg %N` manage it.
- Output follows bash style, for example `[1]+  Stopped   sleep 100`.
- Terminator itself can never be stopped.

</details>

<details>
<summary><b>🕘 History</b></summary>

<br>

- 10,000 commands are stored in `~/.terminator_history` and shared by all tabs.
- `history` prints the latest 1,000.
- <kbd>Ctrl</kbd>+<kbd>R</kbd> opens `Enter search term: ` and then:
  1. prints the most recent **exact** match, otherwise
  2. prints commands sharing the **longest common substring** (only when it is longer than 2 characters), otherwise
  3. prints `No match for search term in history`.

</details>

<details>
<summary><b>📜 Script interpreter</b></summary>

<br>

- `./script.sh`, `bash script.sh` and `source script.sh` all run in Terminator's **own interpreter**.
- Syntax errors (for example a missing `fi`) are reported **before** anything runs.
- Full language details are in the [Scripting Language Reference](#-scripting-language-reference).

</details>

---

## 🧭 Cheat Sheet

### Line editing

| Key | Action |
|---|---|
| <kbd>←</kbd> / <kbd>→</kbd> | Move within the line |
| <kbd>Ctrl</kbd>+<kbd>A</kbd> / <kbd>Ctrl</kbd>+<kbd>E</kbd> | Start / end of line |
| <kbd>↑</kbd> / <kbd>↓</kbd> | Browse history |
| <kbd>Ctrl</kbd>+<kbd>U</kbd> / <kbd>Ctrl</kbd>+<kbd>K</kbd> | Delete to start / end of line |
| <kbd>Ctrl</kbd>+<kbd>W</kbd> | Delete previous word |
| <kbd>Ctrl</kbd>+<kbd>R</kbd> | Search history |

### Shell control

| Key | Action |
|---|---|
| <kbd>Ctrl</kbd>+<kbd>C</kbd> | Interrupt the running command |
| <kbd>Ctrl</kbd>+<kbd>Z</kbd> | Suspend the running command |
| <kbd>Ctrl</kbd>+<kbd>D</kbd> | Exit (close the tab) |
| <kbd>Ctrl</kbd>+<kbd>L</kbd> | Clear the screen |

### Window, tabs and clipboard

| Key | Action |
|---|---|
| <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>T</kbd> / <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>W</kbd> | New / close tab |
| <kbd>Ctrl</kbd>+<kbd>Tab</kbd> / <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>Tab</kbd> | Next / previous tab |
| <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>C</kbd> / <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>V</kbd> | Copy / paste (middle-click pastes PRIMARY) |
| <kbd>Page Up</kbd> / <kbd>Page Down</kbd> | Scroll |

### Job control

| Command | Action |
|---|---|
| `cmd &` | Run in the background |
| `jobs` | List jobs |
| `fg %1` | Bring job 1 to the foreground |
| `bg %1` | Resume job 1 in the background |

### The `terminator` builtin

| Command | Action |
|---|---|
| `terminator keybinds` | List current key bindings |
| `terminator keybind Ctrl+G eol` | Rebind a key and save it |
| `terminator reload` | Reload `~/.terminatorrc` |
| `terminator history clear` | Clear history |

---

## 🏛️ Architecture

```text
                    ┌────────────────────────────────────────────┐
                    │                  gui.c                     │
                    │  X11 window · tab bar · render · poll()    │
                    └───────────────┬────────────────────────────┘
                                    │  byte protocol (keys in, text out)
              ┌─────────────────────┴─────────────────────┐
              │ Thread mode: in-memory                    │ Process mode: pipes
      ┌───────▼───────┐                            ┌──────▼────────┐
      │  worker.c     │  × one per tab             │  worker.c     │  × one per tab
      │  (the shell)  │                            │  (the shell)  │
      └───────┬───────┘                            └──────┬────────┘
              │                                           │
   ┌──────────▼───────────────────────────────────────────▼──────────┐
   │ shell.c  →  lexer.c → exec.c / builtins.c / script.c / multiwatch.c │
   │            edit.c · bindings.c · history.c · util.c              │
   └─────────────────────────────────────────────────────────────────┘
        fork/execvp · pipe · dup2 · setpgid · kill · waitpid
```

**Command lifecycle**

1. `gui.c` turns key events into bytes for the active tab.
2. `worker.c` runs the line editor (`edit.c`), applying key bindings (`bindings.c`).
3. A completed line is dispatched by `shell.c`.
4. `lexer.c` tokenises, quotes, expands and parses it into a pipeline.
5. `exec.c` forks the children, wires pipes and redirections with `dup2`, assigns process groups and records jobs.
6. Output returns to the GUI, which renders it.

A deeper write-up of modes, IPC, data structures and signals lives in **`DESIGN.md`**.

---

## ⚙️ Configuration

| File | Purpose |
|---|---|
| `~/.terminatorrc` | Key bindings, one `bind <keyspec> <action>` per line (see `terminatorrc.sample`). Takes effect without recompiling and survives restarts. |
| `~/.terminator_history` | Shared command history (10,000 entries). |

Invalid or conflicting lines are **reported**, and the default binding is kept.

```sh
# ~/.terminatorrc
bind Ctrl+G eol
```

---

## 📜 Scripting Language Reference

Scripts run in Terminator's own interpreter.

**Supported**

| Area | Details |
|---|---|
| Structure | Comments, multiple commands, `exit [n]` |
| Control flow | Nested `if` / `elif` / `else` / `fi` |
| Tests | `[ ... ]` and `test` |
| Variables | Assignment, `$VAR`, `${VAR}`, `$?`, `$#`, `$1…` |
| Substitution | `$(...)` and backticks |
| I/O | Pipes, redirection, globbing |
| Safety | Syntax errors (for example a missing `fi`) are reported instead of running the script |

**Test operators**

| Kind | Operators |
|---|---|
| File | `-f` `-d` `-e` `-r` `-w` `-x` `-s` |
| String | `-z` `-n` `=` `!=` |
| Integer | `-eq` `-ne` `-lt` `-le` `-gt` `-ge` |
| Logic | `!` |

**Example**

```sh
#!/bin/bash
# example-check.sh: report whether a file exists
if [ -f "$1" ]; then
    echo "$1 exists ($(wc -l < "$1") lines)"
else
    echo "$1 not found"
    exit 1
fi
```

Run it any of these ways, all handled by Terminator:

```sh
./example-check.sh README.md
bash example-check.sh nothere.c
source example-check.sh README.md
```

---

## 🚫 Feature Removed in This Build

One feature was **removed from the code** (not merely disabled).

| Removed | Details |
|---|---|
| **File-name (Tab) completion** | The numbered chooser and longest-common-prefix extension are gone, including `gui_complete`, `replace_word`, `print_choice_list`, `cmp_str`, the `choosing` / `nchoices` / `choices` fields on `Tab`, the `ACT_COMPLETE` action and the Tab binding. <kbd>Tab</kbd> is unbound and does nothing. |

---

## ⚠️ Known Limitations

<details>
<summary><b>Click to expand</b></summary>

<br>

- **Command substitution** runs through `/bin/sh -c`, so pipes and quoting inside substitutions work. Scripts, the shell's own pipelines, redirection and globbing are implemented directly, and `bash script.sh` never invokes bash.
- **Scripts:** no loops or functions, no arrays, no `case`, and no here-documents.
- **`2>&1`** and `<<` here-documents are tokenised but not wired up.
- **Type-ahead** is not buffered. Keys pressed while a command runs are ignored (control and tab hotkeys still work).
- Output is handled at byte level (no UTF-8 glyph composition), and very long lines are clipped rather than wrapped.
- No controlling terminal is allocated, so full-screen programs such as `vim` and `top` are not supported.

</details>

---

## 📦 Repository Layout

| File | Purpose |
|---|---|
| `terminator.h` | Shared internal interface: tunables, types, shared state, API |
| `state.c` | Definitions of the shared state declared in the header |
| `util.c` | Low-level helpers and output buffer |
| `history.c` | Shared command history and `~/.terminator_history` |
| `lexer.c` | Tokeniser, quoting, expansion and pipeline parsing |
| `exec.c` | `fork`/`exec`, pipes, redirection, process groups, job table |
| `builtins.c` | Shell builtins (`cd`, `jobs`, `fg`, `bg`, `[`, …) |
| `script.c` | Bash-subset script interpreter |
| `multiwatch.c` | `multiWatch` and `watch` (parallel children over `poll()`) |
| `shell.c` | Command dispatch |
| `worker.c` | Per-tab worker (the actual shell) |
| `bindings.c` | Key bindings and `~/.terminatorrc` |
| `edit.c` | Line editor: Ctrl+R search, Ctrl+C, Ctrl+Z |
| `gui.c` | X11 front end: window, tabs, rendering, event loop |
| `main.c` | Entry point |
| `Makefile` | `make` builds `./terminator` |
| `README.md` | This file |
| `DESIGN.md` | Design document: architecture, modes, IPC, data structures, signals |
| `terminatorrc.sample` | Example key-binding configuration |
| `example-check.sh`, `example-demo.sh` | Demo scripts to run inside Terminator |

---

## 🎬 Demo

Inside Terminator:

```text
user@terminator> ls | wc -l
user@terminator> echo "Files in $(basename $(pwd)): $(ls | wc -l)"
user@terminator> sort < somefile.txt > sorted.txt
user@terminator> ./example-check.sh README.md
user@terminator> bash example-check.sh nothere.c
user@terminator> multiWatch ["date +%T", "ls | wc -l"]      # Ctrl+C to stop
user@terminator> sleep 100                                   # Ctrl+Z, then: bg %1, jobs, fg %1
```

---

## 🧪 Testing

There is no X server in the build environment, so Terminator was verified in two complementary ways:

1. **Shell engine harnesses.** `execute_line()` and the worker's byte protocol were driven directly.
2. **Headless GUI event loop.** The real event loop ran against a small stand-in for Xlib with scripted events.

**Both thread mode and process mode pass the full suite**, which covers:

- pipelines and redirection
- quoting and command substitution
- `multiWatch`'s exact output format
- `Ctrl+C` / `Ctrl+Z` job control, plus `jobs`, `fg` and `bg`
- `Ctrl+R` history search
- per-tab working directories
- script interpretation

See `DESIGN.md` §13 for the full testing notes.

---

## 🩺 Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| `fatal error: X11/Xlib.h: No such file` | Install the dev package: `libx11-dev` / `libX11-devel`. |
| `cannot open display` | No X server reachable. Start one, or check `$DISPLAY`. Under Wayland, make sure Xwayland is running. |
| Link error about `pthread` | Build with `make` so `-pthread` is passed at both compile and link time. |
| Keybinding ignored | Check the line in `~/.terminatorrc`. Invalid or conflicting lines are reported and the default is kept. Run `terminator reload`. |
| `vim` or `top` look broken | Full-screen programs are unsupported because no controlling terminal is allocated. |
| Keys typed during a command vanish | Type-ahead is not buffered (see limitations). |
| Stale or huge history | `terminator history clear`. |

---

## 🤝 Contributing & Roadmap

**Contributing:** keep modules small and single-purpose, build warning-free with `-Wall -Wextra`, and test in both thread and process mode before sending changes.

**Ideas for the future**

- [ ] `2>&1` and here-document wiring
- [ ] Loops (`for`, `while`) and functions in scripts
- [ ] Pseudo-terminal allocation for full-screen programs
- [ ] UTF-8 rendering and line wrapping
- [ ] Type-ahead buffering
- [ ] Tab completion, re-implemented as an optional module

---

<div align="center">

**Built with `fork`, `pipe`, `poll`, and a healthy disregard for toolkits.** 🖥️

</div>
