#!/usr/bin/env bash
# Copyright (c) 2026 Percona and/or its affiliates.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

# The git half of the daily property-test cycle, run on the developer's own
# computer (launchd / cron), where git can push with the developer's
# credentials. Claude's scheduled sessions read its status file and do the
# rest (fix and add tests, Jenkins, the dashboard); they cannot push.
#
#   extra/qa/daily_sync.sh sync
#     1. main: fetch the 'percona' remote, merge percona/main, push origin main
#     2. pbt-init-tomislav: merge main, push origin pbt-init-tomislav
#     Stops without pushing on a dirty working tree or a merge conflict
#     (the merge is aborted).
#   extra/qa/daily_sync.sh push-review
#     Pushes every local review branch pbt-auto/* that origin does not have
#     or that is ahead of origin (Claude commits them; you merge them).
#
# Status: .qa-results/daily_sync.json and .qa-results/review_push.json in
# the repository (untracked), a log line per step on stdout.
#
# Variables: PBS_UPSTREAM_REMOTE (percona), PBS_ORIGIN_REMOTE (origin),
# PBS_MAIN_BRANCH (main), PBS_TEST_BRANCH (pbt-init-tomislav).

set -uo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
upstream="${PBS_UPSTREAM_REMOTE:-percona}"
origin="${PBS_ORIGIN_REMOTE:-origin}"
main_branch="${PBS_MAIN_BRANCH:-main}"
test_branch="${PBS_TEST_BRANCH:-pbt-init-tomislav}"
status_dir="${repo_dir}/.qa-results"
mkdir -p "${status_dir}"
cd "${repo_dir}" || exit 2

log() { echo "[$(date '+%F %T')] $*"; }

write_status() { # file result detail
  python3 - "$@" <<'PY'
import json, sys, subprocess, datetime
path, result, detail = sys.argv[1:4]
def rev(ref):
    out = subprocess.run(["git", "rev-parse", "--short", ref], capture_output=True, text=True)
    return out.stdout.strip() or None
extra = json.loads(detail) if detail.startswith("{") else {"detail": detail}
status = {"time": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
          "result": result, **extra}
json.dump(status, open(path, "w"), indent=1)
PY
}

sync() {
  local status="${status_dir}/daily_sync.json"
  if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
    log "working tree has uncommitted changes: nothing done"
    write_status "${status}" dirty "uncommitted changes in $(git branch --show-current)"
    return 1
  fi
  local start_branch main_before test_before new_commits
  start_branch="$(git branch --show-current)"
  git fetch -q "${upstream}" && git fetch -q "${origin}" || {
    write_status "${status}" error "git fetch failed"; return 1; }
  main_before="$(git rev-parse --short "${main_branch}")"
  test_before="$(git rev-parse --short "${test_branch}")"
  new_commits="$(git log --oneline "${main_branch}..${upstream}/${main_branch}" | head -50)"

  git checkout -q "${main_branch}" || { write_status "${status}" error "checkout ${main_branch}"; return 1; }
  if ! git merge -q --no-edit "${upstream}/${main_branch}"; then
    git merge --abort 2>/dev/null
    git checkout -q "${start_branch}"
    log "conflict merging ${upstream}/${main_branch} into ${main_branch}"
    write_status "${status}" conflict "merging ${upstream}/${main_branch} into ${main_branch}"
    return 1
  fi
  git push -q "${origin}" "${main_branch}" || { git checkout -q "${start_branch}";
    write_status "${status}" error "push ${origin} ${main_branch}"; return 1; }

  git checkout -q "${test_branch}" || { write_status "${status}" error "checkout ${test_branch}"; return 1; }
  if ! git merge -q --no-edit "${main_branch}"; then
    git merge --abort 2>/dev/null
    git checkout -q "${start_branch}"
    log "conflict merging ${main_branch} into ${test_branch}"
    write_status "${status}" conflict "merging ${main_branch} into ${test_branch}"
    return 1
  fi
  git push -q "${origin}" "${test_branch}" || { git checkout -q "${start_branch}";
    write_status "${status}" error "push ${origin} ${test_branch}"; return 1; }
  git checkout -q "${start_branch}"

  local detail
  detail="$(python3 -c 'import json,sys; print(json.dumps({"main_before": sys.argv[1], "main_after": sys.argv[2], "test_before": sys.argv[3], "test_after": sys.argv[4], "upstream_new_commits": [l for l in sys.argv[5].split("\n") if l]}))' \
    "${main_before}" "$(git rev-parse --short "${main_branch}")" \
    "${test_before}" "$(git rev-parse --short "${test_branch}")" "${new_commits}")"
  write_status "${status}" ok "${detail}"
  log "synced: ${main_branch} ${main_before} -> $(git rev-parse --short "${main_branch}"), ${test_branch} ${test_before} -> $(git rev-parse --short "${test_branch}")"
}

push_review() {
  local status="${status_dir}/review_push.json" pushed=() branch
  git fetch -q "${origin}" 2>/dev/null
  while IFS= read -r branch; do
    [[ -z "${branch}" ]] && continue
    if ! git rev-parse -q --verify "refs/remotes/${origin}/${branch}" >/dev/null ||
       [[ -n "$(git log --oneline "${origin}/${branch}..${branch}" 2>/dev/null)" ]]; then
      if git push -q "${origin}" "${branch}"; then
        pushed+=("${branch}")
        log "pushed ${branch}"
      else
        log "push of ${branch} failed"
      fi
    fi
  done < <(git for-each-ref --format='%(refname:short)' 'refs/heads/pbt-auto/*')
  if [[ ${#pushed[@]} -gt 0 ]]; then
    write_status "${status}" pushed "{\"branches\": [$(printf '"%s",' "${pushed[@]}" | sed 's/,$//')]}"
  fi
}

case "${1:-}" in
  sync) sync ;;
  push-review) push_review ;;
  *) echo "usage: $0 sync|push-review" >&2; exit 2 ;;
esac
