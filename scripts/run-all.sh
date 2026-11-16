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
  out=$(timeout 60 "$bin" 2>&1 </dev/null); rc=$?
  if [ "$rc" -eq 0 ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1)); failed+=("$bin")
    # Show why right away: on CI you cannot "re-run it directly".
    echo "---- FAIL (exit $rc): $bin -- last lines:"
    printf '%s\n' "$out" | tail -n 15 | sed 's/^/    /'
  fi
done < <(for d in "${dirs[@]}"; do [ -d "$d" ] && find "$d" -type f -perm -u+x; done | sort)

echo "passed: $pass   failed: $fail"
for f in "${failed[@]}"; do echo "  FAIL: $f"; done
[ "$fail" -eq 0 ]
