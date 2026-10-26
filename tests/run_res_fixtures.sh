#!/bin/sh
# Runs rescheck on every *.res under $BULWARK_RES_FIXTURES. Files the parser
# has to repair pass, and are listed; a file it rejects fails the test.
# Game content is never committed, so this is skipped when the variable is unset.
RESCHECK="$1"
if [ -z "$BULWARK_RES_FIXTURES" ]; then
	echo "BULWARK_RES_FIXTURES not set, skipping"
	exit 0
fi
find "$BULWARK_RES_FIXTURES" -name '*.res' -print0 | sort -z | xargs -0 "$RESCHECK" --quiet ${RESCHECK_ARGS:-}
