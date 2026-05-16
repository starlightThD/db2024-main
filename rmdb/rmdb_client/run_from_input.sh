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

query_no=0
select_no=0
while IFS= read -r line || [[ -n "${line}" ]]; do
  line="${line%$'\r'}"

  # Skip empty lines and SQL comment lines.
  if [[ "${line}" =~ ^[[:space:]]*$ ]]; then
    continue
  fi
  if [[ "${line}" =~ ^[[:space:]]*-- ]]; then
    continue
  fi

  query_no=$((query_no + 1))
  normalized="${line#"${line%%[![:space:]]*}"}"
  lower="${normalized,,}"
  is_select=0
  if [[ "${lower}" =~ ^select[[:space:]] ]]; then
    is_select=1
    select_no=$((select_no + 1))
  fi

  start_ns="$(date +%s%N)"

  tmp_cmds="$(mktemp)"
  {
    printf '%s\n' "${line}"
    printf 'exit;\n'
  } > "${tmp_cmds}"

  # Run one statement per client session to get per-statement elapsed time.
  "${CLIENT_BIN}" "$@" < "${tmp_cmds}" >/dev/null

  end_ns="$(date +%s%N)"
  rm -f "${tmp_cmds}"

  elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
  if [[ "${is_select}" -eq 1 ]]; then
    printf '[S%03d] %d ms | %s\n' "${select_no}" "${elapsed_ms}" "${line}"
  fi
done < "${INPUT_FILE}"
