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

# Pre-flight check of the property tests: compiles every source of
# tests/property with g++-14 -fsyntax-only and the project's warning flags
# (-Werror), so that a test that would break the Jenkins build is caught in
# a minute instead of after the 3-hour run's build step. It does not link
# and does not need the project's Boost / AWS SDK build: system Boost
# headers are enough for a syntax check.
#
#   extra/qa/preflight_check.sh [--deps-dir DIR] [FILE...]
#
# FILE: sources relative to tests/property (default: all *.cpp there and
# common/fsync_trace.cpp). DIR (default: ~/.cache/pbs-preflight) receives
# hegel-cpp and reflect-cpp at the versions the build uses. On a Debian /
# Ubuntu host as root, missing packages (g++-14, Boost, MySQL client, OpenSSL,
# zlib headers) are installed with apt-get. Exit code 0: all files compile.

set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
deps_dir="${HOME}/.cache/pbs-preflight"
files=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --deps-dir) deps_dir="$2"; shift 2 ;;
    -h|--help) sed -n '17,32p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) files+=("$1"); shift ;;
  esac
done

hegel_version="v0.13.1"
reflect_version="v0.22.0"

need_packages=()
command -v g++-14 >/dev/null || need_packages+=(g++-14)
[[ -d /usr/include/boost ]] || need_packages+=(libboost-dev)
[[ -f /usr/include/mysql/mysql.h ]] || need_packages+=(libmysqlclient-dev)
[[ -f /usr/include/openssl/evp.h ]] || need_packages+=(libssl-dev)
[[ -f /usr/include/zlib.h ]] || need_packages+=(zlib1g-dev)
if [[ ${#need_packages[@]} -gt 0 ]]; then
  if [[ $(id -u) -ne 0 ]] || ! command -v apt-get >/dev/null; then
    echo "preflight: missing ${need_packages[*]} (install them, or run as root on Debian / Ubuntu)" >&2
    exit 2
  fi
  echo "preflight: installing ${need_packages[*]}"
  DEBIAN_FRONTEND=noninteractive apt-get -qq update >/dev/null
  DEBIAN_FRONTEND=noninteractive apt-get -qq install -y "${need_packages[@]}" >/dev/null
fi

mkdir -p "${deps_dir}"
fetch_dep() { # name url tag
  local dir="${deps_dir}/$1-$3"
  if [[ ! -d "${dir}" ]]; then
    git clone -q --depth 1 --branch "$3" "$2" "${dir}" 2>/dev/null \
      || { echo "preflight: cannot clone $2 $3" >&2; exit 2; }
  fi
  echo "${dir}"
}
hegel_dir="$(fetch_dep hegel-cpp https://github.com/hegeldev/hegel-cpp.git "${hegel_version}")"
reflect_dir="$(fetch_dep reflect-cpp https://github.com/getml/reflect-cpp.git "${reflect_version}")"

# the flags of cmake/compiler_warnings.cmake as the Jenkins build uses them
warnings=(-Werror -Wall -Wextra -Wextra-semi -Wshadow -Wnon-virtual-dtor -Wold-style-cast
  -Wcast-align -Wunused -Woverloaded-virtual -Wpedantic -Wconversion -Wsign-conversion
  -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
  -Wmisleading-indentation -Wduplicated-cond -Wduplicated-branches -Wlogical-op
  -Wuseless-cast -Wno-missing-field-initializers)

cd "${repo_dir}"
if [[ ${#files[@]} -eq 0 ]]; then
  while IFS= read -r f; do files+=("${f#tests/property/}"); done \
    < <(ls tests/property/*.cpp; echo tests/property/common/fsync_trace.cpp)
fi

failed=0
for f in "${files[@]}"; do
  out="$(mktemp)"
  if g++-14 -std=c++23 -fsyntax-only "${warnings[@]}" \
      -DPBS_BINLOG_SERVER_PATH='"/nonexistent/binlog_server"' \
      -DPBS_FSYNC_TRACE_PATH='"/nonexistent/libpbt_fsync_trace.so"' \
      -I src -I tests/property \
      -isystem "${hegel_dir}/include" -isystem "${reflect_dir}/include" \
      -isystem "${reflect_dir}/include/rfl/thirdparty" -isystem /usr/include/mysql \
      "tests/property/${f}" >"${out}" 2>&1; then
    echo "ok      ${f}"
  else
    echo "FAILED  ${f}"
    grep -E "error|warning" -A3 "${out}" | head -40
    failed=1
  fi
  rm -f "${out}"
done
exit ${failed}
