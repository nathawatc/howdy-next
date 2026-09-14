#!/usr/bin/env bash
set -euo pipefail

fail() {
	echo "Nix CI image is stale or incomplete: $*" >&2
	exit 1
}

metadata=/etc/howdy-nix-ci/dependencies
[[ -r $metadata ]] || fail "Missing dependency metadata."

options=(--offline --no-update-lock-file)
expected=$(nix eval "${options[@]}" --raw \
	.#packages.x86_64-linux.ci-dependencies.outPath) || fail "Cannot evaluate dependencies offline."
[[ $expected == "$(<"$metadata")" ]] || fail "Dependency set changed."

inputs=$(nix derivation show "${options[@]}" .#packages.x86_64-linux.howdy-next |
	jq -er '[.derivations[].inputs.drvs | to_entries[] |
		"/nix/store/" + .key + "^" + (.value.outputs | join(","))] |
		unique | .[]') || fail "Cannot inspect package dependencies."
printf '%s\n' "$inputs" | nix build "${options[@]}" --no-link \
	--max-jobs 0 --stdin || fail "A required build input is not prebuilt."

echo "Prebuilt dependencies verified."
