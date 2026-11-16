---
name: git-workflow
description: Commit, branch, push and open PRs in the embedded-systems-handbook repo with the personal code-locker identity — develop-based feat/fix branches, PRs into develop with the CI test check green, release merges to main, and what must never be committed. Use before any commit, push, or PR in this repo.
---

# Git workflow

## Pre-flight (every commit)

```bash
git config user.name            # must be code-locker; stop otherwise
git status --short              # no build/, *.ko, *.mod*, *.cmd, Module.symvers, *.dot
make test && make test-tsan     # (and make test-gst if gstreamer/ changed)
```

Pushes go over the SSH alias `github-codelocker` (`git remote -v` shows
`git@github-codelocker:code-locker/embedded-systems-handbook.git`). Never use a
work identity, email, key or host here.

## Day-to-day

```bash
git switch develop && git pull
git switch -c feat/<topic>                 # or fix/<topic>
# ... edit, verify (skill add-lesson) ...
git add <paths> && git commit              # message: "<area>: <what>", e.g. "linux-internals: add 05 semaphores"
git push -u origin feat/<topic>
gh pr create --repo code-locker/embedded-systems-handbook --base develop   # only if gh is logged in as code-locker
```

- `develop` and `main` are protected: changes go in only through a PR, and the CI `test`
  job (`.github/workflows/ci.yml`: ASan/UBSan, TSan, driver build, GStreamer) must be green.
- Merged branches are auto-deleted. `main` only receives release merges from `develop`.
- One track or one coherent topic per PR keeps reviews readable.

## Symptom → cause

| Symptom | Cause / fix |
|---|---|
| `Permission denied (publickey)` on push | the code-locker SSH key or alias isn't loaded on this PC; see the beaglebone-ai64 `workspace-setup` skill |
| commit shows the wrong author | `includeIf` identity not active in this directory; fix it before pushing, and amend the commit |
| CI TSan step fails but local passes | timing-dependent assertion, or a fixed IPC name. See `concurrency-testing` |
