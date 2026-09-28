#!/usr/bin/env bash
# Publishes a permanent, never-overwritten release for one signed bundle
# build (Droidtop/tracker#122). Shared by the unstable job (every push) and
# the publish job (a manual testing/stable promotion, which can build a
# commit the push job never saw): both call this so a bundle version that
# ever reaches a person always has a release that outlives the next push,
# unlike the moving unstable/testing/stable tags this repo already has,
# which stay exactly as they were -- rolling pointers, not history.
#
# Never deletes or replaces another tag. Re-running this exact workflow run
# (same run number) replaces only its own release, matching how the moving
# channels already handle a retry.
set -euo pipefail
id="$1"           # e.g. dev.enginehost.catsystem2.cst.v1
run_number="$2"
sha="$3"
ref_name="$4"
shift 4

slug="$(printf '%s' "$id" | sed 's/^dev\.enginehost\.//; s/\./-/g')"
tag="${slug}-build.${run_number}"
notes="Signed Enginehost bundle \`$id\`, build $run_number, built from \`$ref_name\` at \`$sha\`. This is permanent build history, never overwritten; install from the unstable, testing or stable release instead, whichever tracks what you want."

if gh release view "$tag" >/dev/null 2>&1; then
  gh release delete "$tag" --yes --cleanup-tag
fi
gh release create "$tag" "$@" --target "$sha" --title "$id (build $run_number)" --prerelease --notes "$notes"
