#!/bin/sh
# The build version, which every player's build must share: the release (the tag v1.2.3 of the
# commit, as 1.2.3), or else the commit, plus "-dirty" when the tracked files have local changes.
cd "$(dirname "$0")/.." || exit 1
v=$(git rev-parse --short=10 HEAD) || exit 1
[ -n "$(git status --porcelain --untracked-files=no)" ] && v="$v-dirty"
case $v in
  *-dirty) ;;
  *) tag=$(git tag --points-at HEAD 'v[0-9]*' | head -1); [ -n "$tag" ] && v=${tag#v} ;;
esac
echo "$v"
