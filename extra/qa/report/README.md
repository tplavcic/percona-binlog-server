# PBS Property Test Findings - the dashboard

The dashboard ("PBS Property Test Findings", a claude.ai artifact) is
`template.html` with `report_data.json` inserted. `report_data.json` is the
state: the issues with their analyses and the failures of the latest run,
the run's totals, the notes of the run ("New in this run") and the
comparison with Hrvoje's tickets and Manish's findings.

After a Jenkins run of "PBS property based tests" (master ps80), with its
`results/summary.md` and `results/run_info.json`:

```sh
extra/qa/report/update_report.py --summary summary.md --run-info run_info.json \
  --build 46 --changes changes.json --html pbs-findings.html
node extra/qa/report/check_report.js "$PWD/pbs-findings.html"   # needs Playwright
```

`changes.json` holds what the triage decided: new issues with their
analysis, field updates (for example `"status": "fixed"`), the notes of the
run, new cross-reference rows, and tickets that leave the "only Hrvoje"
list. `update_report.py --help` documents its keys. An issue id that the run
reports but neither `report_data.json` nor `changes.json` has gets a stub
from `extra/qa/known_bugs.md`, and the script names it: replace it with an
analysis.

Commit `report_data.json` with the triage's other changes
(`known_bugs.md`, `PBT_PLAN.md`), then publish the page to the artifact.
