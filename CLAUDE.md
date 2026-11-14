# CLAUDE.md — dsa-gstreamer-learning

Personal learning repo (GitHub owner **code-locker**, public): DSA and
GStreamer from basic to advanced in C and C++. Separate from `beaglebone-ai64`.

## Identity

- Personal identity only. `git config user.name` must be `code-locker`.
  Never write personal emails or key material into the repo.

## Rules for content

- Every lesson follows [docs/CONVENTIONS.md](docs/CONVENTIONS.md): fixed README
  sections, standalone self-checking programs, comments that explain *why*.
- Each `.c`/`.cpp` is one program built by the top-level `Makefile` into `build/`.
  Verify a new or changed program with `make run P=<path>`: zero warnings,
  exit 0, clean under ASan + UBSan. Run `make test` / `make test-gst` before committing.
- `*_bug.c` / `*_bug.cpp` are intentionally broken (debugging lessons); `make test` skips them.
- GStreamer programs must run headless and terminate (test sources +
  `num-buffers`, fakesink/appsink by default, `--display` to show video).
- New lesson ⇒ link it from `dsa/README.md` or `gstreamer/README.md` and `docs/ROADMAP.md`.
- Never commit `build/` or generated files.

## Workflow

- `develop` is the working branch; `main` only gets release merges from `develop`.
- Both are protected: changes go in only through a PR, with the CI `test` check green.
  Work on `feat/<topic>` / `fix/<topic>` off `develop` and open the PR into `develop`
  (merged branches are auto-deleted).
