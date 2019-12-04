#!/usr/bin/env bash
set -vex

# configure
# '--wrap-mode nofallback' prevents meson from downloading
# stuff from the internet or using subprojects.
meson \
  --default-library shared \
  --libdir lib \
  --unity "${ENABLED_UNITY_BUILD:-off}" \
  --wrap-mode nofallback \
  --prefix "${PREFIX_ARG:-/usr/local}" \
  -Dtests="${ENABLED_TESTS:-false}" \
  -Dtests-internal="${ENABLED_INTERNAL_TESTS:-false}" \
  "${CURRENT_BUILD_DIR:-build}" .
