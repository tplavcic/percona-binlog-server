#!/usr/bin/env python3
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

"""Updates the "PBS Property Test Findings" dashboard with one test run.

The dashboard is template.html with report_data.json inserted. This script
reads a run's results/summary.md and results/run_info.json (the Jenkins job
"PBS property based tests" archives both), applies an optional changes file
written during triage, and writes report_data.json and the page:

  extra/qa/report/update_report.py --summary summary.md --run-info run_info.json \\
      --build 46 [--changes changes.json] [--html pbs-findings.html]

From the summary it takes the run, the totals, the per-executable counts and
the failures of the "Known issues" section, which it attaches to the issues
by id (a failure's Hegel report text is carried over from the previous run
unless the changes say otherwise). An issue id that the summary names but
report_data.json lacks, and the changes file does not add, gets a stub from
extra/qa/known_bugs.md (a triage should replace it with a real analysis).

changes.json (every key optional):
  "issues":          [{"id", "severity", "component", "title", "summary",
                       "description", "root_cause", "fix", "verification",
                       "impact", "properties": [...]}]  new or replaced
                       issues ("status" defaults to "open")
  "issue_updates":   {"PBT-043": {"status": "fixed", ...}}  fields to change
  "first_run":       ["exe::Property", ...]  report text "(first run)"
  "reports":         {"exe::Property": "report text"}  for this run
  "notes":           [{"test", "verdict", "detail"}]  "New in this run"
                     (replaces the previous run's notes)
  "cross_add":       [{"after": "PBT-082" or null, "rows": [{"ours": [...],
                       "manish": "", "hrvoje": "#59", "note": ""}]}]
  "cross_updates":   [{"hrvoje": "#19", "fields": {...}}]
  "only_hrvoje_remove": [59, 69]
  "only_hrvoje_updates": {"19": {"note": "..."}}
  "only_manish_remove": [9]
  "sources":         {"hrvoje": {...}, "manish": {...}}  comparison sources

Then it checks the result: every known failure is attached to an issue,
totals add up. Exit code 0 on success.
"""

import argparse
import collections
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
BUILD_DIR = "/work/percona-binlog-server-build-{variant}-pbt"
SRC_DIR = "/work/percona-binlog-server"


def table_rows(text: str, heading: str) -> list:
    """The rows of the first markdown table after '## heading'."""
    start = text.find("## " + heading)
    if start < 0:
        return []
    end = text.find("\n## ", start + 3)
    section = text[start:end if end > 0 else None]
    rows = []
    for line in section.split("\n"):
        if not line.startswith("| ") or set(line) <= set("|-: "):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        rows.append(cells)
    return rows[1:] if rows else []  # without the header row


def parse_summary(text: str) -> dict:
    header = re.search(r"\|\s*total\s*\|.*\n\|[-:| ]+\|\n(\|[^\n]+\|)", text)
    if not header:
        raise SystemExit("summary.md: no totals table")
    numbers = [int(x) for x in re.findall(r"\d+", header.group(1))]
    keys = ["total", "passed", "failed", "errors", "new_issues", "known_issues",
            "regressions", "possibly_fixed"]
    totals = dict(zip(keys, numbers))
    severity = {"high": 0, "medium": 0, "low": 0, "unknown": 0}
    found = re.search(r"Known issues by severity: ([^\n]+)", text)
    if found:
        for name, count in re.findall(r"(\w+) (\d+)", found.group(1)):
            severity[name] = int(count)
    totals["known_by_severity"] = severity
    known = []
    for cells in table_rows(text, "Known issues"):
        if len(cells) >= 4 and cells[1].startswith("PBT-"):
            known.append({"severity": cells[0], "id": cells[1],
                          "test": cells[2].strip("`"), "message": cells[3]})
    executables = collections.OrderedDict()
    for cells in table_rows(text, "All results"):
        if len(cells) >= 2 and cells[0].startswith("`"):
            exe = cells[0].strip("`").split("::")[0]
            entry = executables.setdefault(exe, {"passed": 0, "failed": 0})
            entry["passed" if cells[1] == "passed" else "failed"] += 1
    return {"totals": totals, "known": known, "executables": executables}


def known_bug_descriptions() -> dict:
    """issue id -> (severity, description) from extra/qa/known_bugs.md"""
    result = {}
    path = REPO / "extra" / "qa" / "known_bugs.md"
    for line in path.read_text(encoding="utf-8").split("\n"):
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) >= 6 and cells[2].startswith("PBT-") and cells[2] not in result:
            result[cells[2]] = (cells[4], cells[5])
    return result


def stub_issue(issue_id: str, severity: str, description: str, test: str) -> dict:
    first = re.split(r"(?<=[.:])\s", description, maxsplit=1)[0]
    return {"id": issue_id, "severity": severity or "unknown", "status": "open",
            "component": "(untriaged)", "title": first[:120],
            "summary": description[:600], "description": description,
            "root_cause": "Not analysed yet (stub from known_bugs.md).",
            "fix": "", "verification": f"`run_property_tests.py --filter '{test}'` must pass.",
            "impact": "", "properties": [test], "failures": []}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--summary", required=True)
    parser.add_argument("--run-info", required=True)
    parser.add_argument("--build", required=True, type=int, help="Jenkins build number")
    parser.add_argument("--master", default="ps80")
    parser.add_argument("--changes")
    parser.add_argument("--data", default=str(HERE / "report_data.json"))
    parser.add_argument("--template", default=str(HERE / "template.html"))
    parser.add_argument("--html", help="write the dashboard page here")
    args = parser.parse_args()

    data = json.loads(Path(args.data).read_text(encoding="utf-8"))
    summary = parse_summary(Path(args.summary).read_text(encoding="utf-8"))
    info = json.loads(Path(args.run_info).read_text(encoding="utf-8"))
    changes = json.loads(Path(args.changes).read_text(encoding="utf-8")) if args.changes else {}
    build = args.build

    data["run"] = {key: info.get(key) for key in
                   ("run_id", "started", "finished", "duration_s", "variant", "build_preset",
                    "sanitizers", "git_revision", "git_branch", "git_dirty")}
    data["run"]["host"] = f"Jenkins {args.master}, build #{build}"
    data["totals"] = summary["totals"]
    data["executables"] = summary["executables"]
    totals = summary["totals"]
    counted = sum(e["passed"] + e["failed"] for e in summary["executables"].values())
    failed = sum(e["failed"] for e in summary["executables"].values())
    if counted != totals["total"] or failed != totals["failed"]:
        raise SystemExit(f"summary.md: All results ({counted} / {failed} failed) does not match "
                         f"the totals ({totals['total']} / {totals['failed']} failed)")
    if len(summary["known"]) != totals["known_issues"]:
        raise SystemExit(f"summary.md: {len(summary['known'])} known-issue rows, "
                         f"totals say {totals['known_issues']}")

    by_id = {issue["id"]: issue for issue in data["issues"]}
    previous = {f["test"]: f.get("report", "") for i in data["issues"] for f in i.get("failures", [])}
    for issue in changes.get("issues", []):
        issue.setdefault("status", "open")
        issue.setdefault("failures", [])
        issue.setdefault("properties", [])
        by_id[issue["id"]] = issue
    for issue_id, fields in changes.get("issue_updates", {}).items():
        by_id[issue_id].update(fields)
    descriptions = None
    for issue in by_id.values():
        issue["failures"] = []

    variant = info.get("variant") or "asan_gcc14-ubsan"
    build_dir = BUILD_DIR.format(variant=variant)
    first_run = set(changes.get("first_run", []))
    reports = changes.get("reports", {})
    stubs = []
    for failure in sorted(summary["known"], key=lambda f: f["test"]):
        issue_id, test = failure["id"], failure["test"]
        if issue_id not in by_id:
            if descriptions is None:
                descriptions = known_bug_descriptions()
            severity, description = descriptions.get(issue_id, (failure["severity"], ""))
            by_id[issue_id] = stub_issue(issue_id, failure["severity"] or severity, description, test)
            stubs.append(issue_id)
        exe, name = test.split("::", 1)
        log = f"results/logs/{exe}/{name}.log"
        old = previous.get(test, "")
        if test in reports:
            report = reports[test]
        elif test in first_run:
            report = f"Hegel report: Jenkins #{build} artifact {log} (first run)."
        elif old.startswith("(Counterexample of an earlier run"):
            report = (f"(Counterexample of an earlier run; this run's Hegel report is in the "
                      f"Jenkins #{build} artifact {log})" + old[old.index(")") + 1:])
        elif old and not old.startswith("Hegel report"):
            report = old
        else:
            report = f"Hegel report: Jenkins #{build} artifact {log}"
        prefix = (f'{SRC_DIR}/extra/qa/mysqld.sh start && eval "$({SRC_DIR}/extra/qa/mysqld.sh env)" && '
                  if exe == "mysql_property_test" else "")
        issue = by_id[issue_id]
        issue["failures"].append({"test": test, "message": failure["message"], "report": report,
                                  "command": f"cd {build_dir} && {prefix}{build_dir}/tests/property/{exe} --run_test={name}",
                                  "duration": 0})
        if test not in issue.setdefault("properties", []):
            issue["properties"].append(test)
    data["issues"] = [by_id[key] for key in sorted(by_id)]

    if "notes" in changes:
        data["notes"] = changes["notes"]
    comparison = data["comparison"]
    for name, source in changes.get("sources", {}).items():
        comparison["sources"][name] = source
    for block in changes.get("cross_add", []):
        position = len(comparison["cross"])
        if block.get("after"):
            matches = [k for k, row in enumerate(comparison["cross"]) if block["after"] in row["ours"]]
            if not matches:
                raise SystemExit(f"cross_add: no cross row with {block['after']}")
            position = matches[-1] + 1
        comparison["cross"][position:position] = block["rows"]
    for update in changes.get("cross_updates", []):
        for row in comparison["cross"]:
            if row.get("hrvoje", "").split(" ")[0] == update["hrvoje"]:
                row.update(update["fields"])
    removed = set(changes.get("only_hrvoje_remove", []))
    comparison["only_hrvoje"] = [h for h in comparison["only_hrvoje"] if h["num"] not in removed]
    for number, fields in changes.get("only_hrvoje_updates", {}).items():
        for ticket in comparison["only_hrvoje"]:
            if str(ticket["num"]) == str(number):
                ticket.update(fields)
    removed = set(changes.get("only_manish_remove", []))
    comparison["only_manish"] = [m for m in comparison["only_manish"] if m.get("num") not in removed]
    matched = {issue_id for row in comparison["cross"] for issue_id in row["ours"]}
    notes = {entry["id"]: entry["note"] for entry in comparison["only_ours"]}
    comparison["only_ours"] = [{"id": issue["id"], "note": notes.get(issue["id"], "")}
                               for issue in data["issues"]
                               if issue["status"] != "fixed" and issue["id"] not in matched]

    attached = sum(len(issue["failures"]) for issue in data["issues"])
    if attached != totals["known_issues"]:
        raise SystemExit(f"attached {attached} failures, the run has {totals['known_issues']} known")
    Path(args.data).write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    if args.html:
        page = Path(args.template).read_text(encoding="utf-8")
        payload = json.dumps(data, ensure_ascii=False).replace("</", "<\\/")
        Path(args.html).write_text(page.replace("__DATA__", payload), encoding="utf-8")
    print(f"build #{build}: {totals['total']} properties, {totals['failed']} failed "
          f"({totals['known_issues']} known, {totals['new_issues']} new); "
          f"{len(data['issues'])} issues, {len(comparison['cross'])} cross rows, "
          f"{len(comparison['only_hrvoje'])} only Hrvoje, {len(comparison['only_ours'])} only ours"
          + (f"; STUB issues (need an analysis): {', '.join(stubs)}" if stubs else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
