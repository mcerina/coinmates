#!/bin/sh
# usage: netplay/flatpak/flathub.sh <tag> <dir>
# Writes into <dir> the files of Coinmates' repository on Flathub (flathub/io.github.mcerina.Coinmates):
# our manifest, built from the release <tag> on GitHub instead of this checkout, and flathub.json.
# Flathub's bot (x-checker-data) then opens a pull request there at every new release.
set -e
tag=$1; dir=$2
[ -n "$tag" ] && [ -n "$dir" ] || { echo "usage: $0 <tag> <dir>" >&2; exit 1; }
cd "$(dirname "$0")/../.."
commit=$(git rev-parse "$tag^{commit}")
mkdir -p "$dir"
# the source: the tag on GitHub (the tag, so that version.sh says the release)
awk -v tag="$tag" -v commit="$commit" '
  /^      - type: git$/ { git = 1 }
  git && /^        path: / { next }
  git && /^        branch: / {
    print "        url: https://github.com/mcerina/coinmates.git"
    print "        tag: " tag
    print "        commit: " commit
    print "        x-checker-data:"
    print "          type: git"
    print "          tag-pattern: ^v([\\d.]+)$"
    git = 0; next
  }
  { print }' netplay/flatpak/io.github.mcerina.Coinmates.yml > "$dir/io.github.mcerina.Coinmates.yml"
# x86_64 only, until the same states on ARM Linux are checked (the Steam Deck is x86_64)
printf '{\n  "only-arches": ["x86_64"]\n}\n' > "$dir/flathub.json"
echo "$dir: io.github.mcerina.Coinmates.yml (tag $tag, commit $commit), flathub.json"
