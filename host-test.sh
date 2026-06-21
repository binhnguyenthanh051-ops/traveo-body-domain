#!/usr/bin/env bash
# Host-side build + test runner for Windows (MSYS2/UCRT64).
# Usage:
#   bash host-test.sh              # build + run all tests
#   bash host-test.sh messages     # messages tests only
#   bash host-test.sh scheduler    # scheduler tests only
#   bash host-test.sh clean        # remove build artifacts

set -euo pipefail

export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
CC=gcc
CFLAGS="-std=c17 -Wall -Wextra -Werror -O1 -g -pipe"
BUILD=build

UNITY_SRC="vendor/unity/src/unity.c"
UNITY_INC="-Ivendor/unity/src"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

mkdir -p "$BUILD"

build_messages() {
    $CC $CFLAGS $UNITY_INC -Ishared/messages/include \
        $UNITY_SRC shared/messages/src/body_msgs.c \
        shared/messages/tests/test_body_msgs.c \
        -o "$BUILD/test_messages"
}

build_scheduler() {
    $CC $CFLAGS $UNITY_INC -Ischeduler/include \
        $UNITY_SRC scheduler/src/sched.c \
        scheduler/tests/test_sched.c scheduler/tests/sched_port_fake.c \
        -o "$BUILD/test_scheduler"
}

run_messages() {
    build_messages
    echo "== messages =="
    "$BUILD/test_messages"
}

run_scheduler() {
    build_scheduler
    echo "== scheduler =="
    "$BUILD/test_scheduler"
}

case "${1:-all}" in
    messages)   run_messages ;;
    scheduler)  run_scheduler ;;
    clean)      rm -rf "$BUILD" ;;
    all)
        run_messages
        run_scheduler
        ;;
    *)
        echo "Usage: $0 {all|messages|scheduler|clean}" >&2
        exit 1
        ;;
esac
