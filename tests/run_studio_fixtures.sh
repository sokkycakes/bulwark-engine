#!/bin/sh
# Runs mdlcheck on every *.mdl under $BULWARK_STUDIO_FIXTURES, with the
# model's numbered .seq files (name1.seq, name2.seq, ...) appended in order.
# Game content is never committed, so this is skipped when the variable is unset.
MDLCHECK="$1"
if [ -z "$BULWARK_STUDIO_FIXTURES" ]; then
	echo "BULWARK_STUDIO_FIXTURES not set, skipping"
	exit 0
fi
status=0
for mdl in $(find "$BULWARK_STUDIO_FIXTURES" -name '*.mdl' | sort); do
	base="${mdl%.mdl}"
	seqs=""
	n=1
	while [ -f "$base$n.seq" ]; do
		seqs="$seqs $base$n.seq"
		n=$((n + 1))
	done
	"$MDLCHECK" "$mdl" $seqs ${MDLCHECK_ARGS:-} || status=1
done
exit $status
