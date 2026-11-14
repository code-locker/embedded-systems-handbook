#!/usr/bin/env bash
# run-all.sh <build-dir>...
# Runs every compiled example under <build-dir> and reports pass/fail.
# Each example checks itself with assert()/CHECK and exits non-zero on failure.
# Programs named *_bug are intentionally broken (debugging lessons) and skipped.
set -u
dirs=("$@"); [ ${#dirs[@]} -gt 0 ] || { echo "usage: run-all.sh <build-dir>..."; exit 2; }
pass=0; fail=0; failed=()

while IFS= read -r bin; do
  case "$bin" in *_bug) continue ;; esac
  if timeout 60 "$bin" >/dev/null 2>&1 </dev/null; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1)); failed+=("$bin")
  fi
done < <(for d in "${dirs[@]}"; do [ -d "$d" ] && find "$d" -type f -perm -u+x; done | sort)

echo "passed: $pass   failed: $fail"
for f in "${failed[@]}"; do echo "  FAIL: $f   (re-run it directly to see the output)"; done
[ "$fail" -eq 0 ]
