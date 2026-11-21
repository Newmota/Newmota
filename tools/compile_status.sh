#!/bin/sh
# Report the source file of one compiler invocation, then run the compiler.
# The status line is drawn by newmota.host.run_build; compiler output is
# printed in one piece afterwards so parallel jobs do not interleave it.
source_file=
for argument in "$@"; do
    case $argument in
        *.c | *.cc | *.cpp | *.S | *.s) source_file=$argument ;;
    esac
done
if [ -n "$source_file" ]; then
    printf '@@newmota-compile %s\n' "$source_file"
fi
output=$("$@" 2>&1)
status=$?
if [ -n "$output" ]; then
    printf '%s\n' "$output"
fi
exit "$status"
