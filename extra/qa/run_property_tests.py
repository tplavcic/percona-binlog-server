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

"""Runs all property-based tests and writes a test report.

Every property (Boost.Test test case) of every property test executable
(CTest label "property") is run in its own process, so a crash or hang in
one property cannot hide the results of the others.

Output: $WORKDIR/testruns/<YYYYmmdd-HHMMSS>-<variant>/
    summary.md     human readable report (also printed to stdout)
    summary.json   machine readable report
    junit.xml      JUnit report (Jenkins: junit 'testruns/latest/junit.xml')
    run_info.json  git revision, preset, knobs used for the run
    logs/<executable>/<property>.log   full output of every property
and $WORKDIR/testruns/latest and latest-<variant> -> <YYYYmmdd-HHMMSS>-<variant>

Build variant: the preset plus extra sanitizers (e.g. asan_gcc14,
release_gcc14-ubsan), read from pbt_build_info.json written by
build_property_tests.sh. It prefixes the junit suite names
(property.<variant>.<executable>) so runs of several variants can be told
apart in Jenkins.

Sanitizer reports (AddressSanitizer, UBSan "runtime error:",
ThreadSanitizer) are reported as failures of kind 'sanitizer' with the
sanitizer's first report line as the message; a failed assert() (Debug /
ASan builds) as kind 'assertion' with the assert location and expression.
LeakSanitizer reports count only when nothing else failed (an abort leaves
objects behind).

Every failure is classified using extra/qa/known_bugs.md:
    known      - matches an open entry of known_bugs.md (old issue)
    new        - matches no entry (new issue)
    regression - matches an entry whose status is 'fixed'
An open known_bugs.md entry whose test passed is reported as
"possibly fixed". Every known issue carries the severity of its
known_bugs.md row (high / medium / low); new issues are "untriaged".
Reports list issues ordered by severity and count known issues per
severity.

Exit code (--fail-on): 'new' (default) -> 1 if there are new issues,
regressions or infrastructure errors; 'any' -> 1 on any failure;
'never' -> always 0.

Only the Python standard library is used (Python >= 3.8).
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import datetime
import fnmatch
import json
import os
import platform
import re
import shutil
import shlex
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Dict, List, Optional

QA_DIR = Path(__file__).resolve().parent
REPO_DIR = QA_DIR.parent.parent


# ---------------------------------------------------------------------------
# known bugs
# ---------------------------------------------------------------------------
@dataclasses.dataclass
class KnownBug:
    test: str          # fnmatch pattern on "<executable>::<property>"
    match: str         # optional regex searched in the failure text
    issue: str
    status: str        # open | fixed | wontfix
    description: str
    variant: str = ""  # optional fnmatch pattern on the build variant
    severity: str = "unknown"  # high | medium | low

    def applies_to(self, variant: str) -> bool:
        return not self.variant or fnmatch.fnmatchcase(variant, self.variant)

    def matches(self, test_id: str, failure_text: str) -> bool:
        if not fnmatch.fnmatchcase(test_id, self.test):
            return False
        if self.match and not re.search(self.match, failure_text):
            return False
        return True


def _strip_md(cell: str) -> str:
    cell = cell.strip()
    if len(cell) >= 2 and cell[0] == "`" and cell[-1] == "`":
        cell = cell[1:-1]
    return cell.replace("\\|", "|").strip()


SEVERITIES = ("high", "medium", "low")
# report order: most severe first, then unrated / not yet triaged
SEVERITY_ORDER = {name: index for index, name in
                  enumerate(SEVERITIES + ("unknown", "untriaged"))}


def normalize_severity(value: str) -> str:
    value = (value or "").strip().lower()
    if value in SEVERITIES:
        return value
    if value:
        print(f"warning: unknown severity '{value}' in known bugs file",
              file=sys.stderr)
    return "unknown"


def severity_key(severity: str) -> int:
    return SEVERITY_ORDER.get(severity, len(SEVERITY_ORDER))


def load_known_bugs(path: Path) -> List[KnownBug]:
    """Parses every markdown table whose header has 'Test' and 'Issue'."""
    if not path.exists():
        return []
    bugs: List[KnownBug] = []
    header: Optional[List[str]] = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line.startswith("|"):
            header = None  # a table ends at the first non-table line
            continue
        cells = [_strip_md(c) for c in re.split(r"(?<!\\)\|", line)[1:-1]]
        if header is None:
            lowered = [c.lower() for c in cells]
            if "test" in lowered and "issue" in lowered:
                header = lowered
            continue
        if all(re.fullmatch(r":?-+:?", c) for c in cells if c):
            continue  # separator row
        row = dict(zip(header, cells))
        if not row.get("test"):
            continue
        bugs.append(KnownBug(
            test=row.get("test", ""),
            match=row.get("match", ""),
            issue=row.get("issue", ""),
            status=(row.get("status") or "open").lower(),
            description=row.get("description", ""),
            variant=row.get("variant", ""),
            severity=normalize_severity(row.get("severity", "")),
        ))
    return bugs


# ---------------------------------------------------------------------------
# discovery
# ---------------------------------------------------------------------------
@dataclasses.dataclass
class Executable:
    name: str
    command: List[str]
    workdir: Optional[str]


def discover_executables(build_dir: Path, label: str) -> List[Executable]:
    out = subprocess.run(
        ["ctest", "--show-only=json-v1", "-L", label],
        cwd=build_dir, check=True, capture_output=True, text=True).stdout
    data = json.loads(out)
    result = []
    for test in data.get("tests", []):
        command = test.get("command") or []
        workdir = None
        for prop in test.get("properties", []):
            if prop.get("name") == "WORKING_DIRECTORY":
                workdir = prop.get("value")
        # drop options we set ourselves
        command = [c for c in command if c != "--no_color_output"]
        result.append(Executable(test["name"], command, workdir))
    return result


def list_properties(exe: Executable) -> List[str]:
    proc = subprocess.run(exe.command + ["--list_content"], cwd=exe.workdir,
                          capture_output=True, text=True, check=False)
    text = proc.stderr + proc.stdout
    names: List[str] = []
    stack: List[str] = []
    for line in text.splitlines():
        m = re.match(r"^( *)([A-Za-z_][A-Za-z0-9_]*)\*?$", line)
        if not m:
            continue
        depth = len(m.group(1)) // 4
        stack = stack[:depth] + [m.group(2)]
        names.append("/".join(stack))
    # keep leaves only (test suites are prefixes of other entries)
    return [n for n in names if not any(o.startswith(n + "/") for o in names)]


# ---------------------------------------------------------------------------
# running
# ---------------------------------------------------------------------------
@dataclasses.dataclass
class Result:
    executable: str
    name: str
    status: str = "passed"      # passed | failed | error
    kind: str = ""              # violation | assertion | sanitizer | crash
                                # | timeout | infra
    classification: str = ""    # new | known | regression (failures only)
    issue: str = ""
    severity: str = ""          # from known_bugs.md; "untriaged" for new
    message: str = ""
    report: str = ""            # Hegel minimal counterexample block
    blob: str = ""
    duration: float = 0.0
    returncode: Optional[int] = None
    log: str = ""
    command: str = ""

    @property
    def test_id(self) -> str:
        return f"{self.executable}::{self.name}"


FAILURE_BLOCK_RE = re.compile(r"^--- Failure.*?(?=^rerun with:|\Z)",
                              re.S | re.M)
BLOB_RE = re.compile(r'rerun with:.*?\{"([A-Za-z0-9+/=]+)"\}')
VIOLATION_RE = re.compile(r"PROPERTY VIOLATED: .*")
BOOST_ERROR_RE = re.compile(r'(?:fatal error|error): in "[^"]*": (.*)')
# sanitizer reports that are a root cause on their own
SANITIZER_RE = re.compile(
    r"^(?:==\d+==)?\s*((?:ERROR|WARNING): (?:Address|Thread|Memory)"
    r"Sanitizer.*|.*?: runtime error: .*)$", re.M)
# leaks are only a root cause when nothing else failed: an abort (failed
# assert) or a Boost.Test signal catch leaves objects behind
LEAK_RE = re.compile(r"^(?:==\d+==)?\s*(ERROR: LeakSanitizer.*)$", re.M)
# std::terminate() after an uncaught exception (e.g. thrown from a noexcept
# function): "terminate called after throwing an instance of '<type>'"
# followed by "  what():  <message>"
TERMINATE_RE = re.compile(
    r"terminate called after throwing an instance of '([^']*)'\n\s*what\(\):\s*(.*)")
# glibc assert(): "<prog>: <file>:<line>: <function>: Assertion `<expr>' failed."
ASSERT_RE = re.compile(
    r"^[^\n]*?: ([^\s:]+:\d+): [^\n]*?(Assertion `.*' failed\.)$", re.M)


def extract_failure(log_text: str, result: Result) -> None:
    block = FAILURE_BLOCK_RE.search(log_text)
    if block:
        # drop Boost.Test progress lines interleaved with Hegel's report
        result.report = "\n".join(
            l for l in block.group(0).strip().splitlines()
            if not re.match(r"^Running \d+ test cases?\.\.\.$", l))
    blob = BLOB_RE.search(log_text)
    if blob:
        result.blob = blob.group(1)
    violation = VIOLATION_RE.search(log_text)
    boost_error = BOOST_ERROR_RE.search(log_text)
    if violation:
        result.message = violation.group(0).strip()
    elif boost_error:
        result.message = boost_error.group(1).strip()
    elif not result.message:
        tail = [l for l in log_text.strip().splitlines() if l.strip()][-5:]
        result.message = " | ".join(tail) or "test failed without output"


def run_property(exe: Executable, name: str, args, env: Dict[str, str],
                 log_dir: Path) -> Result:
    result = Result(exe.name, name)
    cmd = exe.command + [f"--run_test={name}", "--no_color_output"]
    result.command = " ".join(shlex.quote(c) for c in cmd)
    log_path = log_dir / exe.name / (name.replace("/", "__") + ".log")
    log_path.parent.mkdir(parents=True, exist_ok=True)
    result.log = str(log_path)
    start = time.monotonic()
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        log.write(f"$ {result.command}\n")
        log.flush()
        try:
            proc = subprocess.Popen(cmd, cwd=exe.workdir, env=env,
                                    stdout=log, stderr=subprocess.STDOUT,
                                    start_new_session=True)
            try:
                result.returncode = proc.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
                result.status, result.kind = "failed", "timeout"
                result.message = f"timeout after {args.timeout}s"
        except OSError as exc:
            result.status, result.kind = "error", "infra"
            result.message = f"cannot start: {exc}"
    result.duration = time.monotonic() - start
    log_text = log_path.read_text(encoding="utf-8", errors="replace")

    if result.kind:
        pass
    elif result.returncode == 0:
        result.status = "passed"
    elif result.returncode is not None and result.returncode < 0:
        result.status, result.kind = "failed", "crash"
        sig = -result.returncode
        try:
            sig_name = signal.Signals(sig).name
        except ValueError:
            sig_name = str(sig)
        result.message = f"crashed with signal {sig_name}"
    elif "PBT_INFRA:" in log_text:
        # the test could not run (e.g. no S3 endpoint): not a finding
        result.status, result.kind = "error", "infra"
        match = re.search(r"PBT_INFRA: ([^\n\[]*)", log_text)
        result.message = match.group(1).strip() if match else "infrastructure"
    elif "PROPERTY VIOLATED" in log_text or "--- Failure" in log_text:
        result.status, result.kind = "failed", "violation"
    elif "Test setup error" in log_text or "no test cases matching" in log_text:
        result.status, result.kind = "error", "infra"
    else:
        result.status, result.kind = "failed", "violation"
    # root cause, in priority order: sanitizer error, failed assert(),
    # property violation / crash, and a leak report only when nothing else
    # failed
    sanitizer = SANITIZER_RE.search(log_text)
    assertion = ASSERT_RE.search(log_text)
    leak = LEAK_RE.search(log_text)
    if result.kind != "timeout":
        if sanitizer:
            result.status, result.kind = "failed", "sanitizer"
        elif assertion:
            result.status, result.kind = "failed", "assertion"
        elif leak and result.status == "passed":
            result.status, result.kind = "failed", "sanitizer"
            sanitizer = leak
    if result.kind == "infra":
        # keep the PBT_INFRA reason, no failure analysis
        return result
    if result.status != "passed":
        crash_message = result.message if result.kind == "crash" else ""
        extract_failure(log_text, result)
        if sanitizer:
            result.message = "SANITIZER: " + sanitizer.group(1).strip()
            # every distinct report (UBSan keeps running after a report and
            # prints each source location once)
            reports = list(dict.fromkeys(
                m.group(1).strip() for m in SANITIZER_RE.finditer(log_text)))
            if not reports:
                reports = [sanitizer.group(1).strip()]
            listing = "\n".join(reports[:20])
            if len(reports) > 20:
                listing += f"\n... {len(reports) - 20} more"
            result.report = (f"sanitizer reports ({len(reports)}):\n{listing}"
                             + (f"\n\n{result.report}" if result.report else ""))
        elif assertion:
            result.message = (f"ASSERTION: {assertion.group(1)}: "
                              f"{assertion.group(2)}")
        elif (terminate := TERMINATE_RE.search(log_text)) is not None:
            result.kind = "crash"
            result.message = (f"TERMINATE: uncaught {terminate.group(1)}: "
                              f"{terminate.group(2).strip()}")
        if crash_message:
            result.message = f"{crash_message}: {result.message}"
        # Boost.Test's execution monitor turns SIGSEGV/SIGABRT/... into a
        # regular test failure; report those as crashes
        if result.kind == "violation" and re.search(
                r"memory access violation|signal: SIG|fatal error: in .*: "
                r"(?:SIG|signal)", result.message + "\n" + log_text):
            if "PROPERTY VIOLATED" not in result.message:
                result.kind = "crash"
        if leak and result.kind != "sanitizer":
            result.report += ("\n\n(LeakSanitizer also reported leaks, "
                              "expected after an abort; see the log)")
    return result


def classify(results: List[Result], bugs: List[KnownBug], variant: str):
    """Classifies failures; returns open known bugs whose test passed."""
    possibly_fixed = []
    # rows restricted to other variants (e.g. UBSan-only findings) neither
    # match failures nor count as "possibly fixed" in this run
    bugs = [b for b in bugs if b.applies_to(variant)]
    for res in results:
        if res.status != "failed":
            continue  # infrastructure errors are not product issues
        text = "\n".join([res.message, res.report])
        matched = next((b for b in bugs if b.matches(res.test_id, text)), None)
        if matched is None:
            res.classification, res.severity = "new", "untriaged"
        elif matched.status == "fixed":
            res.classification, res.issue = "regression", matched.issue
            res.severity = matched.severity
        else:
            res.classification, res.issue = "known", matched.issue
            res.severity = matched.severity
    by_id = {r.test_id: r for r in results}
    for bug in bugs:
        if bug.status != "open":
            continue
        ran = [r for tid, r in by_id.items()
               if fnmatch.fnmatchcase(tid, bug.test)]
        if ran and all(r.status == "passed" for r in ran):
            possibly_fixed.append(bug)
    return possibly_fixed


# ---------------------------------------------------------------------------
# reports
# ---------------------------------------------------------------------------
def reproduce_command(res: Result, build_dir: Path) -> str:
    cmd = f"cd {shlex.quote(str(build_dir))} && "
    if res.blob:
        cmd += f"PBT_REPRODUCE={shlex.quote(res.blob)} "
    return cmd + res.command


def write_junit(path: Path, results: List[Result], known_as: str,
                run_name: str, variant: str) -> None:
    suites: Dict[str, List[Result]] = {}
    for res in results:
        suites.setdefault(res.executable, []).append(res)
    root = ET.Element("testsuites", name=run_name)
    totals = {"tests": 0, "failures": 0, "errors": 0, "skipped": 0}
    total_time = 0.0
    for exe, items in sorted(suites.items()):
        suite_name = f"property.{variant}.{exe}" if variant else f"property.{exe}"
        suite = ET.SubElement(root, "testsuite", name=suite_name)
        counts = {"tests": 0, "failures": 0, "errors": 0, "skipped": 0}
        suite_time = 0.0
        for res in items:
            counts["tests"] += 1
            suite_time += res.duration
            case = ET.SubElement(suite, "testcase", classname=suite_name,
                                 name=res.name, time=f"{res.duration:.3f}")
            if res.status == "passed":
                continue
            body = "\n\n".join(x for x in [
                res.message,
                (f"severity: {res.severity}" if res.severity else ""),
                res.report,
                f"reproduce: {res.command}" +
                (f"\nblob: {res.blob}" if res.blob else ""),
                f"log: {res.log}"] if x)
            label = {"known": f"[KNOWN {res.issue} {res.severity}] ",
                     "regression": f"[REGRESSION {res.issue} {res.severity}] ",
                     "new": "[NEW untriaged] "}.get(res.classification, "")
            if res.status == "error":
                counts["errors"] += 1
                el = ET.SubElement(case, "error", type=res.kind,
                                   message=label + res.message[:500])
                el.text = body
            elif res.classification == "known" and known_as == "skipped":
                counts["skipped"] += 1
                ET.SubElement(case, "skipped",
                              message=label + res.message[:500])
            else:
                counts["failures"] += 1
                el = ET.SubElement(case, "failure",
                                   type=f"{res.kind}/{res.classification}",
                                   message=label + res.message[:500])
                el.text = body
        for key, value in counts.items():
            suite.set(key, str(value))
            totals[key] += value
        suite.set("time", f"{suite_time:.3f}")
        total_time += suite_time
    for key, value in totals.items():
        root.set(key, str(value))
    root.set("time", f"{total_time:.3f}")
    if hasattr(ET, "indent"):  # Python >= 3.9
        ET.indent(root)
    ET.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


def build_summary(results, possibly_fixed, run_info) -> Dict:
    def count(pred):
        return sum(1 for r in results if pred(r))
    return {
        "run": run_info,
        "totals": {
            "total": len(results),
            "passed": count(lambda r: r.status == "passed"),
            "failed": count(lambda r: r.status == "failed"),
            "errors": count(lambda r: r.status == "error"),
            "new_issues": count(lambda r: r.classification == "new"),
            "known_issues": count(lambda r: r.classification == "known"),
            "regressions": count(lambda r: r.classification == "regression"),
            "possibly_fixed": len(possibly_fixed),
            # failing properties per severity (known issues and regressions)
            "known_by_severity": {
                name: count(lambda r, n=name: r.classification in
                            ("known", "regression") and r.severity == n)
                for name in SEVERITIES + ("unknown",)},
        },
        "results": [{**dataclasses.asdict(r), "test_id": r.test_id}
                    for r in results],
        "possibly_fixed": [dataclasses.asdict(b) for b in possibly_fixed],
    }


def md_table(header: List[str], rows: List[List], right: str = "") -> List[str]:
    """Markdown table whose columns are padded to the same width, so it
    also reads well as plain text. `right` holds one character per column:
    'r' right-aligns that column, anything else left-aligns it."""
    cells = [[str(c) for c in header]] + [[str(c) for c in row] for row in rows]
    widths = [max(3, *(len(row[i]) for row in cells))
              for i in range(len(header))]
    align = [(right[i:i + 1] == "r") for i in range(len(header))]

    def line(row):
        return "| " + " | ".join(
            c.rjust(w) if r else c.ljust(w)
            for c, w, r in zip(row, widths, align)) + " |"

    rule = "|" + "|".join(("-" * (w + 1) + ":") if r else ("-" * (w + 2))
                          for w, r in zip(widths, align)) + "|"
    return [line(cells[0]), rule] + [line(row) for row in cells[1:]]


def write_markdown(path: Path, summary: Dict, results: List[Result],
                   possibly_fixed: List[KnownBug], build_dir: Path) -> str:
    t = summary["totals"]
    info = summary["run"]
    lines = [
        f"# Property test run {info['run_id']} ({info['variant']})",
        "",
        f"- revision: `{info.get('git_revision', '?')}` "
        f"({info.get('git_branch', '?')}{', dirty' if info.get('git_dirty') else ''})",
        f"- variant: `{info['variant']}` (preset `{info['build_preset']}`"
        f"{', sanitize=' + info['sanitizers'] if info['sanitizers'] else ''})",
        f"- build dir: `{info['build_dir']}`",
        f"- host: {info['host']}, duration: {info['duration_s']:.1f}s, "
        f"test cases scale: {info['knobs'].get('PBT_CASES_SCALE', '1')}",
        "",
        *md_table(["total", "passed", "failed", "errors", "new issues",
                   "known issues", "regressions", "possibly fixed"],
                  [[t["total"], t["passed"], t["failed"], t["errors"],
                    t["new_issues"], t["known_issues"], t["regressions"],
                    t["possibly_fixed"]]], right="rrrrrrrr"),
        "",
        "Known issues by severity: " + ", ".join(
            f"{name} {t['known_by_severity'][name]}"
            for name in SEVERITIES + ("unknown",)
            if name != "unknown" or t['known_by_severity'][name]),
        "",
    ]

    def section(title, items):
        if not items:
            return
        lines.extend([f"## {title}", ""])
        for res in items:
            issue = f" ({res.issue})" if res.issue else ""
            severity = f", severity {res.severity}" if res.severity else ""
            lines.append(f"### `{res.test_id}`{issue} - {res.kind}{severity}")
            lines.append("")
            lines.append("```")
            lines.append(res.message)
            if res.report:
                lines.append("")
                lines.append(res.report)
            lines.append("```")
            lines.append("")
            lines.append(f"- log: `{res.log}`")
            lines.append(f"- reproduce: `{reproduce_command(res, build_dir)}`")
            lines.append("")

    if info.get("s3_problem"):
        lines.extend([f"S3 endpoint for the S3 properties: not available - "
                      f"{info['s3_problem']}", ""])
    errors = [r for r in results if r.status == "error"]
    if errors:
        lines.extend(["## Errors (the test could not run)", ""])
        lines.extend(md_table(
            ["test", "message"],
            [[f"`{res.test_id}`", res.message.replace("|", "/")]
             for res in errors]))
        lines.append("")
    section("New issues", [r for r in results if r.classification == "new"])
    section("Regressions (known bug marked fixed fails again)",
            sorted((r for r in results if r.classification == "regression"),
                   key=lambda r: severity_key(r.severity)))
    known = sorted((r for r in results if r.classification == "known"),
                   key=lambda r: (severity_key(r.severity), r.issue))
    if known:
        lines.extend(["## Known issues", ""])
        lines.extend(md_table(
            ["severity", "issue", "test", "message"],
            [[res.severity, res.issue, f"`{res.test_id}`",
              (res.message.replace("|", "\\|").splitlines() or [""])[0][:160]]
             for res in known]))
        lines.append("")
    if possibly_fixed:
        lines.extend(["## Possibly fixed (open known bug, test passed)", ""])
        lines.extend(md_table(
            ["severity", "issue", "test", "description"],
            [[bug.severity, bug.issue, f"`{bug.test}`", bug.description]
             for bug in sorted(possibly_fixed,
                               key=lambda b: severity_key(b.severity))]))
        lines.append("")
    lines.extend(["## All results", ""])
    rows = []
    for res in results:
        status = res.status if res.status == "passed" else \
            f"{res.status} ({res.classification or res.kind}" + \
            (f", {res.severity})" if res.severity else ")")
        rows.append([f"`{res.test_id}`", status, f"{res.duration:.1f}"])
    lines.extend(md_table(["test", "status", "time [s]"], rows, right="llr"))
    text = "\n".join(lines) + "\n"
    path.write_text(text, encoding="utf-8")
    return text


def variant_name(preset: str, sanitizers: str) -> str:
    """Same naming as pbt_variant() in pbt_env.sh."""
    suffix = {"undefined": "ubsan", "address": "asan", "thread": "tsan"}
    variant = preset
    for san in filter(None, sanitizers.split(",")):
        variant += "-" + suffix.get(san, san)
    return variant


def git_info() -> Dict:
    def git(*a):
        try:
            return subprocess.run(["git", "-C", str(REPO_DIR), *a],
                                  capture_output=True, text=True,
                                  check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            return ""
    return {"git_revision": git("rev-parse", "HEAD"),
            "git_branch": git("rev-parse", "--abbrev-ref", "HEAD"),
            "git_dirty": bool(git("status", "--porcelain", "--untracked-files=no"))}


# ---------------------------------------------------------------------------
def seaweedfs(env: Dict[str, str], workdir: Path, *command: str):
    script_env = dict(env)
    script_env["WORKDIR"] = str(workdir)
    return subprocess.run([str(QA_DIR / "seaweedfs.sh"), *command],
                          env=script_env, capture_output=True, text=True,
                          check=False)


def ensure_seaweedfs(env: Dict[str, str], workdir: Path, log_dir: Path):
    """Puts PBT_S3_* into env, starting SeaweedFS if needed. Returns
    (started here, problem text or ""). On failure the S3 properties report
    an infrastructure error and the reason goes into the report."""
    started = False
    if seaweedfs(env, workdir, "status").returncode != 0:
        result = seaweedfs(env, workdir, "start")
        if result.returncode != 0:
            output = (result.stdout + result.stderr).strip()
            problem = "cannot start SeaweedFS (extra/qa/seaweedfs.sh start): " + \
                (output.splitlines()[-1] if output else
                 f"exit code {result.returncode}")
            # keep the script output and the SeaweedFS log with the report
            log_dir.mkdir(parents=True, exist_ok=True)
            details = log_dir / "seaweedfs-start.log"
            weed_log = Path(env.get("SEAWEEDFS_DIR",
                                    str(workdir / "seaweedfs"))) / "run" / "weed.log"
            text = output + "\n"
            try:
                text += f"\n--- {weed_log} ---\n" + weed_log.read_text(
                    encoding="utf-8", errors="replace")[-20000:]
            except OSError:
                pass
            details.write_text(text, encoding="utf-8")
            problem += f" (details: {details})"
            print(f"warning: {problem}", file=sys.stderr)
            return False, problem
        started = True
    for line in seaweedfs(env, workdir, "env").stdout.splitlines():
        match = re.match(r"export (PBT_S3_\w+)='([^']*)'", line.strip())
        if match:
            env[match.group(1)] = match.group(2)
    return started, ""


def main() -> int:
    workdir_default = os.environ.get("WORKDIR", str(REPO_DIR.parent))
    preset_default = os.environ.get("BUILD_PRESET", "release_gcc14")
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--workdir", default=workdir_default,
                        help="workspace directory (default: %(default)s)")
    parser.add_argument("--build-dir",
                        help="build directory (default: <workdir>/percona-binlog-"
                             "server-build-<variant>-pbt)")
    parser.add_argument("--preset", default=preset_default,
                        help="CMake preset the build used (default: %(default)s)")
    parser.add_argument("--sanitize", default=os.environ.get("PBT_SANITIZE", ""),
                        help="extra sanitizers the build used, as given to "
                             "build_property_tests.sh (selects the build dir)")
    parser.add_argument("--variant",
                        help="label for reports / junit suite names (default: "
                             "from pbt_build_info.json, else preset+sanitizers)")
    parser.add_argument("--filter", action="append", default=[],
                        help="regex on '<executable>::<property>' (repeatable)")
    parser.add_argument("--exclude", action="append", default=[],
                        help="regex on '<executable>::<property>' to skip")
    parser.add_argument("--label", default="property", help="CTest label")
    parser.add_argument("--jobs", "-j", type=int,
                        default=max(1, (os.cpu_count() or 2) // 2),
                        help="properties run in parallel (default: %(default)s)")
    parser.add_argument("--timeout", type=int, default=1800,
                        help="per-property timeout in seconds (default: %(default)s)")
    parser.add_argument("--cases-scale", type=float,
                        help="multiply every property's case count (PBT_CASES_SCALE)")
    parser.add_argument("--test-cases", type=int,
                        help="fixed case count for every property (PBT_TEST_CASES)")
    parser.add_argument("--seed", type=int, help="fixed seed (PBT_SEED)")
    parser.add_argument("--no-database", action="store_true",
                        help="do not use/update the Hegel example database")
    parser.add_argument("--report-multiple", action="store_true",
                        help="report all distinct failures of a property")
    parser.add_argument("--known-bugs", default=str(QA_DIR / "known_bugs.md"),
                        help="known bugs file (default: %(default)s)")
    parser.add_argument("--known-as", choices=["failure", "skipped"],
                        default="failure",
                        help="how known issues appear in junit.xml")
    parser.add_argument("--fail-on", choices=["new", "any", "never"],
                        default="new", help="exit code policy (default: %(default)s)")
    parser.add_argument("--output-dir",
                        help="report directory (default: "
                             "<workdir>/testruns/<timestamp>-<variant>)")
    parser.add_argument("--s3", choices=["auto", "off"], default="auto",
                        help="properties that need S3 (names containing "
                             "'S3'): 'auto' uses PBT_S3_* from the environment "
                             "or starts SeaweedFS with extra/qa/seaweedfs.sh "
                             "(and stops it afterwards); 'off' skips them "
                             "(default: %(default)s)")
    parser.add_argument("--list", action="store_true",
                        help="only list the properties that would run")
    args = parser.parse_args()

    workdir = Path(args.workdir).resolve()
    default_variant = variant_name(args.preset, args.sanitize)
    build_dir = Path(args.build_dir or
                     workdir / f"percona-binlog-server-build-{default_variant}-pbt"
                     ).resolve()
    build_info: Dict = {}
    try:
        build_info = json.loads(
            (build_dir / "pbt_build_info.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        pass
    variant = args.variant or build_info.get("variant") or default_variant
    preset = build_info.get("preset") or args.preset
    sanitizers = build_info.get("sanitizers", args.sanitize) or ""
    if not (build_dir / "CTestTestfile.cmake").exists():
        print(f"error: {build_dir} is not a configured build directory "
              f"(run extra/qa/build_property_tests.sh)", file=sys.stderr)
        return 2

    executables = discover_executables(build_dir, args.label)
    planned = []
    missing: List[Executable] = []
    for exe in executables:
        if not exe.command or not Path(exe.command[0]).exists():
            missing.append(exe)  # registered in CTest but not built
            continue
        for name in list_properties(exe):
            test_id = f"{exe.name}::{name}"
            if args.filter and not any(re.search(f, test_id) for f in args.filter):
                continue
            if any(re.search(f, test_id) for f in args.exclude):
                continue
            if args.s3 == "off" and "S3" in name:
                continue
            planned.append((exe, name))
    if args.list:
        for exe, name in planned:
            print(f"{exe.name}::{name}")
        return 0
    if not planned and not missing:
        print("error: no property tests found", file=sys.stderr)
        return 2

    run_id = datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + f"-{variant}"
    out_dir = Path(args.output_dir or workdir / "testruns" / run_id).resolve()
    if not args.output_dir:
        # the same variant started twice in the same second
        base_id, counter = run_id, 2
        while out_dir.exists():
            run_id = f"{base_id}-{counter}"
            out_dir = out_dir.parent / run_id
            counter += 1
    out_dir.mkdir(parents=True, exist_ok=False)
    log_dir = out_dir / "logs"

    env = dict(os.environ)
    knobs = {}
    if args.cases_scale is not None:
        knobs["PBT_CASES_SCALE"] = str(args.cases_scale)
    if args.test_cases is not None:
        knobs["PBT_TEST_CASES"] = str(args.test_cases)
    if args.seed is not None:
        knobs["PBT_SEED"] = str(args.seed)
    if args.report_multiple:
        knobs["PBT_REPORT_MULTIPLE"] = "1"
    knobs["PBT_DATABASE"] = "off" if args.no_database else \
        str(workdir / "testruns" / "hegel-db")
    for key in ["PBT_CASES_SCALE", "PBT_TEST_CASES", "PBT_SEED",
                "PBT_REPORT_MULTIPLE", "PBT_REPRODUCE", "PBT_DERANDOMIZE"]:
        env.pop(key, None)
    env.update(knobs)
    # sanitizer defaults (kept if already set by the caller)
    # UBSan builds are recoverable: keep running and report every distinct
    # location (the runner turns any report into a failure)
    env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1:halt_on_error=0")
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1:abort_on_error=0")
    env.setdefault("TSAN_OPTIONS", "halt_on_error=1:second_deadlock_stack=1")
    # the AWS SDK otherwise probes the EC2 instance metadata service for a
    # region / credentials every time an S3 client is created (every
    # storage construction), which can cost seconds per probe on hosts
    # where 169.254.169.254 does not answer
    env.setdefault("AWS_EC2_METADATA_DISABLED", "true")
    started_seaweedfs, s3_problem = False, ""
    if any("S3" in name for _, name in planned) and \
            "PBT_S3_ENDPOINT" not in env:
        started_seaweedfs, s3_problem = ensure_seaweedfs(env, workdir,
                                                          log_dir)

    # temporary files of the properties (storage tests create binlog
    # directories) stay inside WORKDIR: <run dir>/tmp, removed at the end
    tmp_dir = out_dir / "tmp"
    tmp_dir.mkdir()
    env["TMPDIR"] = str(tmp_dir)

    print(f"running {len(planned)} properties from {len(executables)} "
          f"executable(s), {args.jobs} in parallel -> {out_dir}", flush=True)
    started = time.monotonic()
    started_at = datetime.datetime.now().isoformat(timespec="seconds")
    results: List[Result] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run_property, exe, name, args, env, log_dir):
                   (exe, name) for exe, name in planned}
        for future in concurrent.futures.as_completed(futures):
            res = future.result()
            results.append(res)
            mark = "PASS" if res.status == "passed" else res.status.upper()
            print(f"  [{len(results):3}/{len(planned)}] {mark:6} "
                  f"{res.test_id} ({res.duration:.1f}s)", flush=True)
    order = {(e.name, n): i for i, (e, n) in enumerate(planned)}
    results.sort(key=lambda r: order[(r.executable, r.name)])
    for exe in missing:
        results.append(Result(exe.name, "<executable>", status="error",
                              kind="infra", message="executable is not built "
                              "(run extra/qa/build_property_tests.sh)"))

    # leftovers of killed / crashed properties
    shutil.rmtree(tmp_dir, ignore_errors=True)
    if started_seaweedfs:
        # started for this run only: stop it and drop the test objects
        seaweedfs(env, workdir, "wipe")

    bugs = load_known_bugs(Path(args.known_bugs))
    possibly_fixed = classify(results, bugs, variant)

    run_info = {
        "run_id": run_id,
        "started": started_at,
        "finished": datetime.datetime.now().isoformat(timespec="seconds"),
        "duration_s": time.monotonic() - started,
        "host": platform.node(),
        "variant": variant,
        "build_preset": preset,
        "sanitizers": sanitizers,
        "build_dir": str(build_dir),
        "known_bugs_file": args.known_bugs,
        "knobs": knobs,
        "s3_problem": s3_problem,
        "command_line": sys.argv,
        **git_info(),
    }
    (out_dir / "run_info.json").write_text(json.dumps(run_info, indent=2) + "\n")
    summary = build_summary(results, possibly_fixed, run_info)
    (out_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    write_junit(out_dir / "junit.xml", results, args.known_as,
                f"pbs-property-tests-{run_id}", variant)
    text = write_markdown(out_dir / "summary.md", summary, results,
                          possibly_fixed, build_dir)

    if not args.output_dir:
        for link_name in ["latest", f"latest-{variant}"]:
            latest = out_dir.parent / link_name
            try:
                if latest.is_symlink() or latest.exists():
                    latest.unlink()
                latest.symlink_to(out_dir.name)
            except OSError:
                pass

    t = summary["totals"]
    print()
    print(text.split("\n## ")[0].rstrip())
    print(f"\nreport: {out_dir}/summary.md\njunit:  {out_dir}/junit.xml")

    if args.fail_on == "never":
        return 0
    if args.fail_on == "any":
        return 1 if (t["failed"] or t["errors"]) else 0
    return 1 if (t["new_issues"] or t["regressions"] or t["errors"]) else 0


if __name__ == "__main__":
    sys.exit(main())
