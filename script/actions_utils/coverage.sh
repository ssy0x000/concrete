CURR_DIR=`dirname $0`

# Run diff-coverage
poetry run diff-cover coverage.xml --fail-under 100 \
--html-report coverage.html \
--compare-branch origin/"$1" | tee diff-coverage.txt

# Get exit code without closing the script
TEST_EXIT_CODE="$?"

# Format diff-coverage.txt for PR comment
poetry run python script/actions_utils/coverage_report_format.py \
--diff-cover-exit-code "$TEST_EXIT_CODE" \
--diff-cover-output diff-coverage.txt

# Set exit code if test failed
if [[ "$TEST_EXIT_CODE" != "0" ]]; then
    exit "$TEST_EXIT_CODE"
fi
