#!/bin/sh
# mkvm.sh - convert build/os.img into VM-friendly disk formats.
#
#   sh tools/mkvm.sh [image] [outdir]
#
# Defaults: image = build/os.img, outdir = build.
#
# Produces, when the corresponding host tool is installed:
#   <outdir>/kestrel.vdi        VirtualBox   (VBoxManage convertfromraw)
#   <outdir>/kestrel.vmdk       VMware       (qemu-img convert -O vmdk)
#   <outdir>/kestrel-img.zip    raw image, compressed (zip)
#
# Missing tools are reported and skipped; this script never fails the
# build. Exit status is 0 unless the source image itself is absent.

set -u

IMG="${1:-build/os.img}"
OUTDIR="${2:-build}"

VDI="$OUTDIR/kestrel.vdi"
VMDK="$OUTDIR/kestrel.vmdk"
ZIP="$OUTDIR/kestrel-img.zip"

made=0
skipped=0

have()
{
    command -v "$1" >/dev/null 2>&1
}

note()
{
    echo "mkvm: $*"
}

if [ ! -f "$IMG" ]; then
    note "error: $IMG not found -- run 'make' first"
    exit 1
fi

mkdir -p "$OUTDIR" || exit 1

note "source image: $IMG"

# ---------------- VirtualBox (VDI) ----------------

if have VBoxManage; then
    rm -f "$VDI"
    if VBoxManage convertfromraw "$IMG" "$VDI" --format VDI >/dev/null 2>&1; then
        note "wrote $VDI (VirtualBox)"
        made=$((made + 1))
    else
        note "VBoxManage failed; skipping $VDI"
        skipped=$((skipped + 1))
    fi
else
    note "VBoxManage not found; skipping VirtualBox VDI"
    note "  install VirtualBox, or convert later with:"
    note "  VBoxManage convertfromraw $IMG $VDI --format VDI"
    skipped=$((skipped + 1))
fi

# ---------------- VMware (VMDK) ----------------

if have qemu-img; then
    rm -f "$VMDK"
    if qemu-img convert -O vmdk "$IMG" "$VMDK" >/dev/null 2>&1; then
        note "wrote $VMDK (VMware)"
        made=$((made + 1))
    else
        note "qemu-img failed; skipping $VMDK"
        skipped=$((skipped + 1))
    fi
else
    note "qemu-img not found; skipping VMware VMDK"
    note "  install qemu-utils, or convert later with:"
    note "  qemu-img convert -O vmdk $IMG $VMDK"
    skipped=$((skipped + 1))
fi

# ---------------- compressed raw image ----------------

if have zip; then
    rm -f "$ZIP"
    # -j: store the bare filename, not the build/ path.
    if zip -j -q "$ZIP" "$IMG"; then
        note "wrote $ZIP (compressed raw image)"
        made=$((made + 1))
    else
        note "zip failed; skipping $ZIP"
        skipped=$((skipped + 1))
    fi
else
    note "zip not found; skipping $ZIP"
    note "  install zip, or compress later with: zip -j $ZIP $IMG"
    skipped=$((skipped + 1))
fi

note "done: $made image(s) written, $skipped skipped"
exit 0
