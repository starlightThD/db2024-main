#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INPUT_FILE="${SCRIPT_DIR}/input.txt"
CLIENT_BIN="${SCRIPT_DIR}/build/rmdb_client"

if [[ ! -f "${INPUT_FILE}" ]]; then
  echo "input file not found: ${INPUT_FILE}" >&2
  exit 1
fi

if [[ ! -x "${CLIENT_BIN}" ]]; then
  echo "client binary not found or not executable: ${CLIENT_BIN}" >&2
  echo "please build rmdb_client first" >&2
  exit 1
fi

tmp_cmds="$(mktemp)"
trap 'rm -f "${tmp_cmds}"' EXIT

has_exit=0
while IFS= read -r line || [[ -n "${line}" ]]; do
  # Strip trailing CR for CRLF input files.
  line="${line%$'\r'}"
  printf '%s\n' "${line}" >> "${tmp_cmds}"
  if [[ "${line}" =~ ^[[:space:]]*(exit|exit;|bye|bye;)[[:space:]]*$ ]]; then
    has_exit=1
  fi
done < "${INPUT_FILE}"

if [[ "${has_exit}" -eq 0 ]]; then
  echo "exit;" >> "${tmp_cmds}"
fi

"${CLIENT_BIN}" "$@" < "${tmp_cmds}"
