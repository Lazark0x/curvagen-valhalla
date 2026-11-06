#!/usr/bin/env bash
# The round-trip test run that gates an engine image: builds the round-trip unit and
# gurka suites from this checkout and runs them. Run it from the repository root on the
# exact commit about to be built, with submodules populated
# (git submodule update --init --recursive). Needs docker.
#
#   scripts/curvagen-roundtrip-tests.sh
#
# DEPS_IMAGE   a ready image of Dockerfile.local's `deps` stage (built when unset)
# BUILD_DIR    the cmake build directory under the checkout, kept between runs so a
#              rerun is incremental (default build-roundtrip-tests)
# JOBS         compile parallelism (default 3)
# CONTAINER_NAME  the build container's name
#
# Exit 0 only when every suite passed. The last line is the summary to quote in the
# release note.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
BUILD_DIR="${BUILD_DIR:-build-roundtrip-tests}"
JOBS="${JOBS:-3}"
UNIT=(roundtrip_gates roundtrip_score curvature_metric)
GURKA=(gurka_motorcycle_roundtrip gurka_roundtrip_audit gurka_roundtrip_distinctness
  gurka_roundtrip_request_faults gurka_loops gurka_motorcycle_curvature
  gurka_curvature_metric_stamp)

if [ -z "${DEPS_IMAGE:-}" ]; then
  DEPS_IMAGE="curvagen-valhalla-deps:$(git hash-object scripts/install-linux-deps.sh | cut -c1-12)"
  if ! docker image inspect "$DEPS_IMAGE" >/dev/null 2>&1; then
    # No extended attributes: the daemon cannot set macOS ones (com.apple.provenance).
    COPYFILE_DISABLE=1 tar --no-xattrs -c Dockerfile.local scripts/install-linux-deps.sh |
      docker build -f Dockerfile.local --target deps -t "$DEPS_IMAGE" -
  fi
fi

sha="$(git rev-parse --short=9 HEAD)"
git diff --quiet HEAD -- . ':!third_party' || sha="$sha+dirty"

targets=""
for t in "${UNIT[@]}" "${GURKA[@]}"; do targets="$targets --target $t"; done
runs=""
for t in "${UNIT[@]}"; do runs="$runs test/$t"; done
for t in "${GURKA[@]}"; do runs="$runs test/gurka/$t"; done

docker run --rm --name "${CONTAINER_NAME:-curvagen-roundtrip-tests-$$}" \
  -v "$PWD":"$PWD" -w "$PWD" "$DEPS_IMAGE" bash -c "
set -uo pipefail
cmake -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DENABLE_TESTS=On \
  -DENABLE_SINGLE_FILES_WERROR=Off >/dev/null || exit 1
cmake --build $BUILD_DIR -j$JOBS $targets || exit 1
cd $BUILD_DIR
failed=0; summary=''
for t in $runs; do
  n=\$(basename \$t)
  if LOCPATH=$PWD/locales ./\$t --gtest_brief=1 > \$n.log 2>&1; then r=pass; else r=FAIL; failed=1; tail -40 \$n.log; fi
  summary=\"\$summary \$n=\$r(\$(grep -oE '[0-9]+ tests?' \$n.log | head -1 | cut -d' ' -f1))\"
done
echo \"roundtrip tests @ $sha:\$summary\"
exit \$failed
"
