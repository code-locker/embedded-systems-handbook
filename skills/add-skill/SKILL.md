---
name: add-skill
description: Create or update a Claude skill in this handbook's skills/ directory so a procedure learned while writing or testing lessons (a sanitizer trick, a build fix, a lesson-writing recipe) is kept. Use after solving something non-obvious in this repo, when the user asks to save/remember a procedure, or when an existing skill turned out wrong or incomplete.
---

# Add or update a skill

Skills live in `skills/<kebab-name>/SKILL.md`. `.claude/skills` is a symlink
to `skills/`, so they are picked up on every clone.

## Decide

- An existing skill covers the topic → **update it**: fix the wrong step, or add
  the gotcha to its symptom → cause table. Don't create near-duplicates.
- A new, reusable procedure (one that would be repeated, or would cost real time to
  rediscover) → a new skill.
- Lesson *content* (theory, explanations) belongs in the lesson README, not in a skill.
  Skills are about *maintaining* the repo.

## Format

```markdown
---
name: <kebab-name>              # matches the folder
description: <what it does + "Use when …" triggers, one paragraph>
---

# Title
Steps with exact commands, then a symptom → cause table, then rules.
```

- Use only commands you have actually run in this repo, and mark anything untested.
- Point to repo files by path (`Makefile`, `docs/CONVENTIONS.md`) instead of copying them.
- No work/office names, hosts, emails, or personal paths.

## Finish

1. Add a line to the index in `skills/README.md`.
2. Commit on a feature branch (see `git-workflow`): `git commit -m "skills: add <name>"`.
