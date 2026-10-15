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

# Local S3-compatible endpoint (SeaweedFS 'weed mini') for the S3 storage
# property tests. Binds to 127.0.0.1 only; data lives in
# $SEAWEEDFS_DIR/data (inside WORKDIR).
#
#   extra/qa/seaweedfs.sh start   # start (no-op if already running)
#   extra/qa/seaweedfs.sh stop    # stop
#   extra/qa/seaweedfs.sh status  # exit 0 if the S3 endpoint answers
#   extra/qa/seaweedfs.sh env     # print PBT_S3_* variables to 'eval'
#   extra/qa/seaweedfs.sh uri [path]
#                                 # print a storage.uri for main_config.json
#   extra/qa/seaweedfs.sh wipe    # stop and delete all stored data
#
# Ports / credentials / bucket: SEAWEEDFS_* in extra/qa/pbt_env.sh.

set -euo pipefail
# shellcheck source=extra/qa/pbt_env.sh
source "$(dirname "${BASH_SOURCE[0]}")/pbt_env.sh"

weed="${SEAWEEDFS_DIR}/bin/weed"
data_dir="${SEAWEEDFS_DIR}/data"
run_dir="${SEAWEEDFS_DIR}/run"
pid_file="${run_dir}/weed.pid"
log_file="${run_dir}/weed.log"
s3_config="${run_dir}/s3_identities.json"
endpoint="http://127.0.0.1:${SEAWEEDFS_S3_PORT}"

is_running() {
  [[ -f "${pid_file}" ]] && kill -0 "$(cat "${pid_file}")" 2>/dev/null
}

s3_answers() {
  # any HTTP answer (403 for an anonymous request) means S3 is up
  local code
  code="$(curl --silent --output /dev/null --max-time 2 --write-out '%{http_code}' "${endpoint}/" || true)"
  [[ "${code}" != "000" ]]
}

bucket_ready() {
  local code
  code="$(curl --silent --output /dev/null --max-time 5 --write-out '%{http_code}' \
    --aws-sigv4 "aws:amz:us-east-1:s3" \
    --user "${SEAWEEDFS_ACCESS_KEY}:${SEAWEEDFS_SECRET_KEY}" \
    "${endpoint}/${SEAWEEDFS_BUCKET}/" || true)"
  [[ "${code}" == "200" ]]
}

port_in_use() {
  (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
}

# stops a SeaweedFS that did not come up and exits with the reason
fail_start() {
  tail -20 "${log_file}" >&2 || true
  stop
  pbt_die "$1 (log: ${log_file})"
}

start() {
  [[ -x "${weed}" ]] || pbt_die "${weed} not found; run extra/qa/setup_pbt_env.sh"
  if is_running; then
    pbt_log "SeaweedFS already running (pid $(cat "${pid_file}"))"
    return 0
  fi
  local port
  for port in "${SEAWEEDFS_S3_PORT}" "${SEAWEEDFS_MASTER_PORT}" \
              "${SEAWEEDFS_VOLUME_PORT}" "${SEAWEEDFS_FILER_PORT}"; do
    if port_in_use "${port}"; then
      pbt_die "port ${port} on 127.0.0.1 is already in use (another SeaweedFS / service?); set SEAWEEDFS_S3_PORT / _MASTER_PORT / _VOLUME_PORT / _FILER_PORT"
    fi
  done
  # gRPC ports are the HTTP ports + 10000
  for port in "${SEAWEEDFS_MASTER_PORT}" "${SEAWEEDFS_VOLUME_PORT}" \
              "${SEAWEEDFS_FILER_PORT}"; do
    if port_in_use "$((port + 10000))"; then
      pbt_die "gRPC port $((port + 10000)) (= ${port} + 10000) on 127.0.0.1 is already in use; set SEAWEEDFS_*_PORT"
    fi
  done
  pbt_require_in_workdir "${data_dir}"
  mkdir -p "${data_dir}" "${run_dir}"
  cat > "${s3_config}" <<EOF
{
  "identities": [
    {
      "name": "pbs_qa",
      "credentials": [
        {"accessKey": "${SEAWEEDFS_ACCESS_KEY}", "secretKey": "${SEAWEEDFS_SECRET_KEY}"}
      ],
      "actions": ["Admin", "Read", "Write", "List", "Tagging"]
    }
  ]
}
EOF
  pbt_log "starting SeaweedFS on ${endpoint} (log: ${log_file})"
  # setsid: keep running after the calling shell exits
  setsid "${weed}" mini \
    -dir="${data_dir}" \
    -ip=127.0.0.1 -ip.bind=127.0.0.1 \
    -master.port="${SEAWEEDFS_MASTER_PORT}" \
    -volume.port="${SEAWEEDFS_VOLUME_PORT}" \
    -filer.port="${SEAWEEDFS_FILER_PORT}" \
    -s3.port="${SEAWEEDFS_S3_PORT}" \
    -s3.config="${s3_config}" \
    -bucket="${SEAWEEDFS_BUCKET}" \
    -admin.ui=false -webdav=false \
    >"${log_file}" 2>&1 < /dev/null &
  echo $! > "${pid_file}"
  local s3_up=0
  for _ in $(seq 1 "${SEAWEEDFS_START_TIMEOUT}"); do
    if s3_answers; then
      s3_up=1
      if bucket_ready; then
        pbt_log "SeaweedFS ready, bucket '${SEAWEEDFS_BUCKET}'"
        return 0
      fi
    fi
    is_running || fail_start "SeaweedFS exited"
    sleep 1
  done
  if [[ ${s3_up} -eq 1 ]]; then
    fail_start "SeaweedFS S3 answers on ${endpoint} but bucket '${SEAWEEDFS_BUCKET}' is not accessible with the configured keys in ${SEAWEEDFS_START_TIMEOUT}s (curl $(curl --version | head -1 | cut -d' ' -f2) needs >= 7.75 for --aws-sigv4)"
  fi
  fail_start "SeaweedFS S3 endpoint ${endpoint} did not answer in ${SEAWEEDFS_START_TIMEOUT}s"
}

stop() {
  if is_running; then
    local pid
    pid="$(cat "${pid_file}")"
    pbt_log "stopping SeaweedFS (pid ${pid})"
    kill "${pid}" 2>/dev/null || true
    for _ in $(seq 1 30); do
      kill -0 "${pid}" 2>/dev/null || break
      sleep 1
    done
    kill -9 "${pid}" 2>/dev/null || true
  fi
  rm -f "${pid_file}"
}

case "${1:-}" in
  start) start ;;
  stop) stop ;;
  status)
    if is_running && s3_answers; then
      echo "running (pid $(cat "${pid_file}")) ${endpoint}"
    else
      echo "not running"; exit 1
    fi ;;
  env)
    cat <<EOF
export PBT_S3_ENDPOINT='${endpoint}'
export PBT_S3_ACCESS_KEY='${SEAWEEDFS_ACCESS_KEY}'
export PBT_S3_SECRET_KEY='${SEAWEEDFS_SECRET_KEY}'
export PBT_S3_BUCKET='${SEAWEEDFS_BUCKET}'
EOF
    ;;
  uri)
    # README.md: http[s]://[key:secret@]host[:port]/bucket/path
    echo "http://${SEAWEEDFS_ACCESS_KEY}:${SEAWEEDFS_SECRET_KEY}@127.0.0.1:${SEAWEEDFS_S3_PORT}/${SEAWEEDFS_BUCKET}/${2:-pbs}"
    ;;
  wipe)
    stop
    pbt_require_in_workdir "${data_dir}"
    rm -rf "${data_dir}"
    pbt_log "SeaweedFS data removed" ;;
  *) sed -n '17,30p' "$0"; exit 2 ;;
esac
