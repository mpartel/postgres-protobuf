#!/bin/bash
set -euo pipefail

cd "$(dirname "${0}")"

# Derive the supported Postgres versions from the per-version test workflows.
versions=()
for workflow in .github/workflows/tests-pg*.yml; do
    [[ -e "${workflow}" ]] || continue
    version="${workflow#.github/workflows/tests-pg}"
    version="${version%.yml}"
    versions+=("${version}")
done

if [[ "${#versions[@]}" -eq 0 ]]; then
    echo "No tests-pg*.yml workflows found" >&2
    exit 1
fi

IFS=$'\n' versions=($(sort -n <<<"${versions[*]}")); unset IFS

echo "Testing Postgres versions: ${versions[*]}"

for version in "${versions[@]}"; do
    echo "=== Postgres ${version} ==="
    env USE_DOCKER=1 POSTGRES_VERSION="${version}" ./build-and-test.sh
done

echo "All versions passed: ${versions[*]}"
