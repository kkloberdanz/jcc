# skip_stage REASON: say why this suite cannot run here and exit 77.
# make turns any failing recipe into exit 2, so the code alone does not
# survive `make X`: when tests/verify.sh sets VERIFY_SKIP, a marker file
# is left there too, and that is what makes the stage a SKIP, not a FAIL.
skip_stage() {
    echo "$1"
    [ -z "${VERIFY_SKIP:-}" ] || : >"$VERIFY_SKIP"
    exit 77
}
