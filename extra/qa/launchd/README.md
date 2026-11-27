# Daily property-test cycle: the part that runs on your computer

Claude's scheduled sessions cannot push to GitHub, so the git half of the
daily cycle runs here, with your git credentials:

- `com.percona.pbs-daily-sync` - 07:15 Monday to Friday: `extra/qa/daily_sync.sh sync`
  (merge percona/main into main and main into pbt-init-tomislav, push both;
  it stops on uncommitted changes or a conflict and records why).
- `com.percona.pbs-review-push` - every 10 minutes: `extra/qa/daily_sync.sh
  push-review` (pushes the review branches `pbt-auto/*` that Claude commits
  locally, so that Jenkins can run them).

Both write their status to `.qa-results/` in the repository, where the
morning session reads it. Install (macOS):

```sh
REPO_DIR="$HOME/Development/percona/percona-binlog-server"
for plist in com.percona.pbs-daily-sync com.percona.pbs-review-push; do
  sed "s|REPO_DIR|${REPO_DIR}|" "${REPO_DIR}/extra/qa/launchd/${plist}.plist" \
    > "$HOME/Library/LaunchAgents/${plist}.plist"
  launchctl bootstrap "gui/$(id -u)" "$HOME/Library/LaunchAgents/${plist}.plist"
done
launchctl kickstart "gui/$(id -u)/com.percona.pbs-daily-sync"   # try it now
cat /tmp/com.percona.pbs-daily-sync.log "${REPO_DIR}/.qa-results/daily_sync.json"
```

Remove with `launchctl bootout "gui/$(id -u)/<label>"`. A Mac that sleeps
at 07:15 runs the job when it wakes. git must be able to push without a
prompt (the macOS keychain credential helper does that once you have pushed
from a terminal).
