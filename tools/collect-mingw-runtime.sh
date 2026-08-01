#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

queue=(
  lua.exe
  lua55.dll
  skynet.exe
  skynet.dll
  platform.dll
  luaclib/*.so
  cservice/*.so
)

index=0
while ((index < ${#queue[@]})); do
  file="${queue[index]}"
  index=$((index + 1))
  [[ -f "${file}" ]] || continue

  while read -r dependency; do
    [[ -n "${dependency}" ]] || continue
    name="$(basename "${dependency}")"
    if [[ ! -f "${name}" ]]; then
      cp -f "${dependency}" "${name}"
      queue+=("${name}")
    fi
  done < <(ldd "${file}" 2>/dev/null | awk '$3 ~ /^\/mingw64\/bin\/.*\.dll$/ {print $3}')
done
