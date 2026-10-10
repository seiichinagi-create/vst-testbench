#!/bin/bash
# Rebuild (and, through each repo's COPY_PLUGIN_AFTER_BUILD, redeploy) plugin repos, a few at a time.
#
#   JOBS=3 tools/rebuild_plugins.sh LOGDIR REPO [REPO...]     # REPO = a folder under C:\dev that has a build\ folder
#
# Prints one line per repo as it finishes:  <repo> OK|FAILED <seconds>   (the full log is LOGDIR/<repo>.log).
# Close anything that has the installed VST3s loaded (REAPER, the VST TestBench) first: a loaded DLL cannot be replaced.
LOGDIR="$1"; shift
JOBS="${JOBS:-3}"
mkdir -p "$LOGDIR"

build_one() {
    local repo="$1" start
    start=$(date +%s)
    if cmake --build "/c/dev/$repo/build" --config Release > "$LOGDIR/$repo.log" 2>&1; then
        echo "$repo OK $(( $(date +%s) - start ))"
    else
        echo "$repo FAILED $(( $(date +%s) - start ))"
    fi
}

running=0
for repo in "$@"; do
    build_one "$repo" &
    running=$((running + 1))
    if [ "$running" -ge "$JOBS" ]; then
        wait -n
        running=$((running - 1))
    fi
done
wait
echo "ALL DONE"
