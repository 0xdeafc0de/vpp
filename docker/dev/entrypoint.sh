#!/usr/bin/env bash
set -euo pipefail

repo_root=/workspace/vpp
build_root=/cache/vpp-build-root
repo_build_root="${repo_root}/build-root"

mkdir -p "${build_root}" /run/vpp /root/.cache/ccache

cp "${repo_build_root}/Makefile" "${build_root}/Makefile"
cp "${repo_build_root}/platforms.mk" "${build_root}/platforms.mk"
cp "${repo_build_root}/copyimg" "${build_root}/copyimg"
cp "${repo_build_root}/build-config.mk.README" "${build_root}/build-config.mk.README"
mkdir -p "${build_root}/scripts"
cp "${repo_build_root}/scripts/set-rpath" "${build_root}/scripts/set-rpath"

printf 'SOURCE_PATH = %s\n' "${repo_root}" > "${build_root}/build-config.mk"

if [ -d "${repo_root}/src" ] && ! git -C "${repo_root}" describe --long --match 'v*' >/dev/null 2>&1; then
  printf '%s\n' "${VPP_GIT_DESCRIBE_FALLBACK:-v25.02-0-gcontainer}" > "${repo_root}/src/.version"
fi

exec "$@"
