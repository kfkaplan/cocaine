#!/bin/sh
# Builds a Mac OS X system root for cocaine from a Mac OS X 10.5 install DVD.
#
# The system root is the /System, /usr, /bin and /Library of a real Mac OS X:
# the dynamic linker and system libraries old programs load. Nothing is
# installed or booted. The installer packages on the disc are just unpacked.
#
# usage: import-sysroot.sh <install-dvd image, or its mounted volume> [destination]
#
# The destination defaults to sysroots/10.5 inside the project, which git
# ignores: Apple's files stay out of the repository.
set -eu

src=${1:?usage: import-sysroot.sh <install-dvd image or mounted volume> [destination]}
here=$(cd "$(dirname "$0")/.." && pwd)
dest=${2:-$here/sysroots/10.5}

# The core system, the rest of the frameworks and applications, the Unix
# layer, and the fonts. Printer drivers, translations and the like stay out.
packages="BaseSystem Essentials BSD AdditionalEssentials AdditionalFonts"

mount=
cleanup() {
  if [ -n "$mount" ]; then
    hdiutil detach "$mount" -quiet || true
    rmdir "$mount" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if [ -d "$src" ]; then
  volume=$src
else
  mount=$(mktemp -d)
  hdiutil attach -readonly -nobrowse -noautoopen -noverify -mountpoint "$mount" "$src" >/dev/null
  volume=$mount
fi

pkgdir=$volume/System/Installation/Packages
if [ ! -d "$pkgdir" ]; then
  echo "import-sysroot: $src is not a Mac OS X install disc (no System/Installation/Packages)" >&2
  exit 1
fi

mkdir -p "$dest"
log=$dest/.import.log
: >"$log"

for name in $packages; do
  pkg=$pkgdir/$name.pkg
  if [ ! -e "$pkg" ]; then
    echo "skipping $name: not on this disc"
    continue
  fi
  echo "unpacking $name..."
  # A 10.5 package is a xar archive whose "Payload" is a compressed cpio
  # archive of the files. bsdtar reads both, and refuses entries that try to
  # escape the destination. Device nodes can't be created without root and
  # aren't wanted; those errors land in the log.
  if [ -d "$pkg" ]; then
    # Older bundle-style package.
    gunzip -c "$pkg/Contents/Archive.pax.gz" | bsdtar -x -o -C "$dest" -f - 2>>"$log" || true
  else
    bsdtar -xOf "$pkg" Payload | bsdtar -x -o -C "$dest" -f - 2>>"$log" || true
  fi
done

# Make everything readable and writable by its new owner, so cocaine can read
# it and you can delete it.
chmod -R u+rwX "$dest" 2>/dev/null || true

if [ ! -f "$dest/usr/lib/dyld" ]; then
  echo "import-sysroot: unpacking failed, there is no usr/lib/dyld in $dest (see $log)" >&2
  exit 1
fi
version=$(/usr/libexec/PlistBuddy -c "Print :ProductVersion" \
  "$dest/System/Library/CoreServices/SystemVersion.plist" 2>/dev/null || echo unknown)
echo "done: Mac OS X $version system root in $dest ($(du -sh "$dest" | cut -f1))"
