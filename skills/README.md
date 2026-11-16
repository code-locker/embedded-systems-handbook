# Skills

Claude skills for maintaining this handbook (`<name>/SKILL.md`, loaded
automatically through `.claude/skills` → `skills/`). Each one captures a
procedure that was worked out once and should not have to be worked out again.

## Index

- [add-lesson/](add-lesson/): write a new lesson or program in any track,
  following the conventions, verify it, and link it into the indexes.
- [concurrency-testing/](concurrency-testing/): ThreadSanitizer (`SAN=tsan`,
  `setarch -R`), helgrind/DRD, writing `*_bug` race/deadlock programs, and
  making threaded or forking programs deterministic and hang-free.
- [kernel-module-lesson/](kernel-module-lesson/): `linux-device-drivers/`
  layout, Kbuild Makefile, build for the host and the BeagleBone kernel,
  self-testing modules, and why we never insmod on the host.
- [git-workflow/](git-workflow/): code-locker identity, `develop` →
  `feat/*` → PR with green CI, and what must never be committed.
- [add-skill/](add-skill/): how to add or update a skill here.

## Adding a skill

Follow [add-skill/SKILL.md](add-skill/SKILL.md): create `skills/<name>/SKILL.md`
with `name` and `description` frontmatter, then add it to the index above.
