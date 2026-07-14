#!/usr/bin/env bash
# release.sh — cut a new chowdy release and publish to GitHub + AUR in one go.
#
#   bash tools/release.sh 0.1.3
#
# Collapses the whole release ceremony into one command:
#   1. bump pkgver in packaging/arch/PKGBUILD (pkgrel -> 1), regen .SRCINFO
#   2. commit + push main, create + push tag vX.Y.Z
#   3. download the GitHub release tarball, compute its sha256
#   4. write the sha into PKGBUILD, regen .SRCINFO, verify, commit + push main
#   5. copy PKGBUILD + chowdy.install + .SRCINFO into the AUR repo, commit, push
#
# The AUR push itself is trivial — the rest is the version-pinned + checksummed
# release overhead. This just does it for you so it's a single command.
#
# AUR repo location defaults to ~/projects/aur-chowdy; override with
#   CHOWDY_AUR_REPO=/path bash tools/release.sh 0.1.3

set -euo pipefail

ver="${1:-}"
[[ -n "$ver" ]] || { echo "usage: bash tools/release.sh X.Y.Z" >&2; exit 2; }
[[ "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "version must look like 0.1.3" >&2; exit 2; }

REPO="$(cd "$(dirname "$0")/.." && pwd)"
AUR_REPO="${CHOWDY_AUR_REPO:-$HOME/projects/aur-chowdy}"
ARCH="$REPO/packaging/arch"
PKGBUILD="$ARCH/PKGBUILD"
tag="v$ver"
tarball="https://github.com/q-artem/chowdy/archive/$tag.tar.gz"

cd "$REPO"

# Refuse to release with a dirty tree (untracked files like package-lock.json
# are fine — only staged/unstaged tracked changes block).
if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "working tree has uncommitted changes — commit or stash first" >&2
    exit 1
fi
if [[ ! -d "$AUR_REPO/.git" ]]; then
    echo "AUR repo not found at $AUR_REPO" >&2
    echo "  clone it: git clone ssh://aur@aur.archlinux.org/chowdy.git $AUR_REPO" >&2
    exit 1
fi

echo "==> 1/5  bump pkgver -> $ver"
sed -i "s/^pkgver=.*/pkgver=$ver/; s/^pkgrel=.*/pkgrel=1/" "$PKGBUILD"
( cd "$ARCH" && makepkg --printsrcinfo > .SRCINFO )
git add "$PKGBUILD" "$ARCH/.SRCINFO"
git commit -m "packaging: bump to $ver"

echo "==> 2/5  push main + tag $tag"
git push origin main
git tag -a "$tag" -m "release $ver"
git push origin "$tag"

echo "==> 3/5  sha256 of release tarball"
sha="$(curl -fsSL "$tarball" | sha256sum | cut -d' ' -f1)"
[[ ${#sha} -eq 64 ]] || { echo "bad sha256: '$sha'" >&2; exit 1; }
echo "    $sha"

echo "==> 4/5  write sha, verify, commit + push"
sed -i "s/^sha256sums=.*/sha256sums=('$sha')/" "$PKGBUILD"
( cd "$ARCH" && makepkg --printsrcinfo > .SRCINFO )
( cd "$ARCH" && makepkg --verifysource && rm -f "chowdy-$ver.tar.gz" )
git add "$PKGBUILD" "$ARCH/.SRCINFO"
git commit -m "packaging: real sha256 for $ver"
git push origin main

echo "==> 5/5  publish to AUR ($AUR_REPO)"
cp "$PKGBUILD" "$ARCH/chowdy.install" "$ARCH/.SRCINFO" "$AUR_REPO/"
( cd "$AUR_REPO" && git add PKGBUILD chowdy.install .SRCINFO \
    && git commit -m "chowdy $ver" && git push origin master )

echo
echo "done — chowdy $ver is live on GitHub + AUR."
