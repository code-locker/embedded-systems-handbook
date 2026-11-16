# CLAUDE.md — embedded-systems-handbook

Personal learning repo (GitHub owner **code-locker**, public): embedded
systems from basic to advanced in C and C++. Tracks: `dsa/`, `gstreamer/`,
`linux-internals/` (concurrency + IPC), `linux-device-drivers/` (kernel
modules). Separate from `beaglebone-ai64`, the board workspace.

## Identity

- Personal identity only. `git config user.name` must be `code-locker`.
  Never write personal emails or key material into the repo.

## Rules for content

- Every lesson follows [docs/CONVENTIONS.md](docs/CONVENTIONS.md): fixed README
  sections, standalone self-checking programs, comments that explain *why*.
- Each `.c`/`.cpp` is one program built by the top-level `Makefile` into `build/`.
  Verify a new or changed program with `make run P=<path>`: zero warnings,
  exit 0, clean under ASan + UBSan. Run `make test` / `make test-gst` before committing.
- `linux-internals/` programs must also be clean under ThreadSanitizer:
  `make run P=<path> SAN=tsan` and `make test-tsan` (skill: `concurrency-testing`).
- `linux-device-drivers/<lesson>/kernel/` modules: build with `make drivers`
  (zero warnings, also `W=1`). **Never insmod on the host PC.** Load them on the
  board or in a VM (skill: `kernel-module-lesson`).
- `*_bug.c` / `*_bug.cpp` are intentionally broken (debugging lessons); `make test` skips them.
- GStreamer programs must run headless and terminate (test sources +
  `num-buffers`, fakesink/appsink by default, `--display` to show video).
- New lesson ⇒ link it from its track README (`dsa/`, `gstreamer/`,
  `linux-internals/`, `linux-device-drivers/`) and `docs/ROADMAP.md` (skill: `add-lesson`).
- Never commit `build/`, generated files or kernel build outputs (`*.ko`, `*.mod*`, `*.cmd`).

## Workflow

- `develop` is the working branch; `main` only gets release merges from `develop`.
- Both are protected: changes go in only through a PR, with the CI `test` check green.
  Work on `feat/<topic>` / `fix/<topic>` off `develop` and open the PR into `develop`
  (merged branches are auto-deleted). Skill: `git-workflow`.

## Skills: keep adding them

Claude skills live in `skills/<name>/SKILL.md`. `.claude/skills` is a symlink to
`skills/`, so they load on any clone. When a task teaches something reusable
(a build fix, a sanitizer trick, a lesson-writing procedure), create or update
a skill and add it to `skills/README.md`. Follow `skills/add-skill/SKILL.md`.
