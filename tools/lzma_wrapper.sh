#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
#
# lzma_wrapper.sh - one LZMA command line for every build host.
#
# Airoha's BL2 (preloader) decompresses u-boot.bin.lzma and requires the
# uncompressed size in the 13-byte LZMA-Alone header to be the *real* value.
# Every LZMA SDK encoder writes it; xz writes -1 ("unknown size"), and BL2 then
# aborts the boot of an otherwise perfectly fine image with
#
#   ERROR:   Failed to decompress image (err=1)
#   ERROR:   BL2: Failure in post image load handling (1)
#   ERROR:   Stored BL31 + U-Boot FIP failed (1)
#
# This wrapper gives callers one command line over all of them, and normalises
# the header itself so a stream can never leave it with -1:
#
#   lzma_wrapper.sh e <in> <out>     compress <in> into <out>
#   lzma_wrapper.sh d <in> <out>     decompress <in> into <out>
#   lzma_wrapper.sh -c <in>          compress <in> to stdout
#   lzma_wrapper.sh -d -c <in>       decompress <in> to stdout
#   lzma_wrapper.sh --version        encoder that would be used
#
# `-` means stdin/stdout, and `-z` is accepted for gzip-style compatibility
# (`-dc` / `-zc` are understood as well).
#
# Backends, in the order LZMA_WRAPPER_BACKEND=auto tries them:
#
#   1. a system LZMA SDK encoder - /usr/bin/lzmp, /usr/bin/lzma or whatever
#      `lzma` resolves to, so the images of a host that has the Debian/Ubuntu
#      'lzma' package stay byte-for-byte what they always were.  Two command
#      line flavours are understood and probed for: the POSIX filter form
#      "-z -c <file>" (SDK 9.x lzmp) and the classic form "e <in> <out>"
#      (SDK 4.x LzmaAlone).
#   2. the in-tree legacy encoder built from tools/lzma432 (SDK 4.32, see the
#      lzma-sdk rule in tools/build_airoha/Makefile) - same SDK family as the
#      vendor blobs, no host package needed.
#   3. xz with the LZMA-Alone container.
#
# Backends 1 and 2 produce a sized stream without an end marker, exactly like
# the vendor images.  The xz fallback produces "real size + xz's end marker":
# BL2's LZMA SDK decoder stops after the declared size and ignores the marker
# (checked byte-exact against LZMA SDK 9.22), while liblzma is stricter and
# calls such a stream corrupt - so never use `xz -d` to verify an image built
# with the xz fallback.  The SDK encoders can only read regular files (never a
# pipe, and `lzma`'s decoder insists on a .lzma suffix), so the input is always
# spooled through the temporary workspace.
#
# The script may be symlinked as `lzma` (with its directory early in PATH) to
# give vendor scripts that assume the SDK command line the same behaviour; it
# never invokes itself.
#
# Environment:
#   LZMA_WRAPPER_BACKEND=auto|sdk|tree|xz  pick the encoder (default: auto)
#   LZMA_SDK=<path>         force this binary as the SDK encoder
#   LZMA_TREE_SDK=<path>    in-tree SDK encoder (default: <dir>/build_airoha/lzma-sdk)
#   LZMA_WRAPPER_VERBOSE=1  report the chosen backend on stderr

set -eu

prog=$(basename "$0")

die() { echo "$prog: $*" >&2; exit 1; }

note() {
	[ "${LZMA_WRAPPER_VERBOSE:-0}" = 1 ] || return 0
	echo "$prog: $*" >&2
}

self=$(readlink -f "$0" 2>/dev/null || echo "$0")
tree_sdk=${LZMA_TREE_SDK:-$(dirname "$self")/build_airoha/lzma-sdk}
work=

cleanup() {
	[ -n "$work" ] && rm -rf "$work"
	return 0
}
trap cleanup EXIT HUP INT TERM

usage() {
	cat <<EOF
Usage: $prog e <in> <out>     compress <in> into <out>
       $prog d <in> <out>     decompress <in> into <out>
       $prog -c <in>          compress <in> to stdout
       $prog -d -c <in>       decompress <in> to stdout
       $prog --version        encoder that would be used

'-' means stdin/stdout, '-z' is accepted for gzip-style compatibility.
Environment: LZMA_WRAPPER_BACKEND=auto|sdk|tree|xz, LZMA_SDK=<path>,
             LZMA_TREE_SDK=<path>, LZMA_WRAPPER_VERBOSE=1
EOF
}

# ---------------------------------------------------------------------------
# Patch the LZMA-Alone header (bytes 5..12) with the real uncompressed size.
# ---------------------------------------------------------------------------
fix_lzma_header() {
	python3 - "$1" "$2" <<'PY'
import os, struct, sys

src, dst = sys.argv[1], sys.argv[2]
size = os.path.getsize(src)
data = bytearray(open(dst, 'rb').read())
# sanity-check the LZMA-Alone header: props byte < 225 (lc/lp/pb) and a
# plausible dictionary size.  Catches a backend that emitted another container
# (e.g. .xz) instead of complaining about it later, at boot time.
if (len(data) < 13 or data[0] >= 225 or
        not 4096 <= struct.unpack_from('<I', data, 1)[0] <= 1 << 32):
    sys.stderr.write('not an LZMA-Alone stream (%d bytes, props=0x%02x)\n' %
                     (len(data), data[0] if data else 0))
    sys.exit(1)
if struct.unpack_from('<Q', data, 5)[0] != size:
    struct.pack_into('<Q', data, 5, size)
    open(dst, 'wb').write(data)
PY
}

# ---------------------------------------------------------------------------
# Backend detection
# ---------------------------------------------------------------------------
# An LZMA SDK build announces itself in --version ("LZMA SDK 9.22", "LZMA 4.32
# ... Igor Pavlov", ...); xz's lzma does not.
is_sdk_build() {
	case "$("$1" --version 2>&1 || true)" in
	*"LZMA SDK"* | *"Igor Pavlov"*) return 0 ;;
	esac
	return 1
}

# Which command line shape does this binary understand?
#   filter  - POSIX filter form, "-z -c <regular file>" (SDK 9.x lzmp, xz)
#   classic - "e <in> <out>"                            (SDK 4.x LzmaAlone)
# The probes use this script itself as the (tiny) input file.
probe_form() {
	bin=$1
	shift
	"$bin" "$@" -z -c "$self" >/dev/null 2>&1 && { printf '%s' filter; return 0; }

	probe=${TMPDIR:-/tmp}/lzma_wrapper.probe.$$
	rm -f "$probe"
	if "$bin" "$@" e "$self" "$probe" >/dev/null 2>&1 && [ -s "$probe" ]; then
		rm -f "$probe"
		printf '%s' classic
		return 0
	fi
	rm -f "$probe"
	return 1
}

# Try one candidate command line; on success it becomes the backend.
use_candidate() {
	bin=$1
	shift
	[ -n "$bin" ] || return 1
	command -v "$bin" >/dev/null 2>&1 || return 1
	[ "$(readlink -f "$bin" 2>/dev/null || echo "$bin")" = "$self" ] && return 1
	form=$(probe_form "$bin" "$@") || return 1
	BACKEND=$bin
	BACKEND_OPTS=$*
	BACKEND_MODE=$form
	return 0
}

# The system SDK encoder, PATH first so that a deliberately installed one wins
# over /usr/bin.
find_system_sdk() {
	for cand in "$(command -v lzma 2>/dev/null || true)" /usr/bin/lzmp /usr/bin/lzma; do
		[ -n "$cand" ] || continue
		is_sdk_build "$cand" || continue
		use_candidate "$cand" && return 0
	done
	return 1
}

pick_backend() {
	if [ -n "${LZMA_SDK:-}" ]; then
		use_candidate "$LZMA_SDK" ||
			die "LZMA_SDK=$LZMA_SDK is not a usable LZMA encoder"
		return 0
	fi

	case "${LZMA_WRAPPER_BACKEND:-auto}" in
	sdk | system)
		find_system_sdk ||
			die "no system LZMA SDK encoder found: install the 'lzma' package" ;;
	tree)
		use_candidate "$tree_sdk" ||
			die "in-tree LZMA SDK encoder not usable: $tree_sdk (run 'make lzma-sdk')" ;;
	xz)
		use_candidate xz --format=lzma || die "xz not found" ;;
	*)
		find_system_sdk && return 0
		use_candidate "$tree_sdk" && { note "using the in-tree encoder"; return 0; }
		use_candidate xz --format=lzma && return 0
		die "no LZMA encoder found: install xz-utils or the legacy 'lzma' package, or build the in-tree one" ;;
	esac
}

# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------
mode=e
to_stdout=no
src=
dst=

operand() {
	if [ -z "$src" ]; then
		src=$1
	elif [ -z "$dst" ]; then
		dst=$1
	else
		die "too many file arguments: $1"
	fi
}

case "${1:-}" in
e) mode=e; shift ;;
d) mode=d; shift ;;
esac

for arg in "$@"; do
	case "$arg" in
	--version | -V) mode=version ;;
	--help | -h) mode=help ;;
	-- | '') ;;
	-z) ;;
	-c) to_stdout=yes ;;
	-d) mode=d ;;
	-) operand "$arg" ;;
	--*) die "unsupported option: $arg" ;;
	-*)
		# combined short options: -dc, -zc, -cd, ...
		for c in $(printf '%s' "${arg#-}" | sed 's/./& /g'); do
			case "$c" in
			z) ;;
			c) to_stdout=yes ;;
			d) mode=d ;;
			*) die "unsupported option: -$c" ;;
			esac
		done ;;
	*) operand "$arg" ;;
	esac
done

if [ "$mode" = help ]; then
	usage
	exit 0
fi

BACKEND=
BACKEND_OPTS=
BACKEND_MODE=
pick_backend

if [ "$mode" = version ]; then
	# shellcheck disable=SC2086
	ver=$( { "$BACKEND" $BACKEND_OPTS --version 2>&1 || true; } |
		grep -m 1 -E 'LZMA|Igor Pavlov|XZ Utils|liblzma' || true)
	printf 'backend = %s%s [%s CLI]%s\n' \
		"$BACKEND" "${BACKEND_OPTS:+ $BACKEND_OPTS}" "$BACKEND_MODE" \
		"${ver:+ $ver}"
	exit 0
fi

[ -n "$src" ] || die "no input file given (see --help)"
if [ "$to_stdout" = yes ]; then
	dst=-
fi
[ -n "$dst" ] || die "no output file given (see --help)"

# ---------------------------------------------------------------------------
# Convert
# ---------------------------------------------------------------------------
# The header carries the real size, so the input has to be a seekable regular
# file of known length before anything is compressed.
work=$(mktemp -d "${TMPDIR:-/tmp}/lzma_wrapper.XXXXXX") ||
	die "cannot create a temporary workspace"
in_file=$work/in.bin
if [ "$mode" = d ]; then
	in_file=$work/in.lzma	# the SDK 9.x encoder decides by suffix
fi

if [ "$src" = - ]; then
	cat > "$in_file" || die "cannot read stdin"
else
	cp -f "$src" "$in_file" 2>/dev/null || die "cannot read input file: $src"
fi

out_file=$work/out
note "$mode $src -> $dst using $BACKEND $BACKEND_OPTS ($BACKEND_MODE)"

if [ "$BACKEND_MODE" = classic ]; then
	# LZMA SDK 4.x CLI: lzma e|d <in> <out>
	# shellcheck disable=SC2086
	"$BACKEND" $BACKEND_OPTS "$mode" "$in_file" "$out_file" ||
		die "conversion failed: $src"
elif [ "$mode" = d ]; then
	# shellcheck disable=SC2086
	"$BACKEND" $BACKEND_OPTS -d -c "$in_file" > "$out_file" ||
		die "decompression failed: $src"
else
	# shellcheck disable=SC2086
	"$BACKEND" $BACKEND_OPTS -z -c "$in_file" > "$out_file" ||
		die "compression failed: $src"
fi

if [ "$mode" = e ]; then
	fix_lzma_header "$in_file" "$out_file" ||
		die "cannot patch the LZMA-Alone header of $src (backend: $BACKEND)"
fi

if [ "$dst" = - ]; then
	cat "$out_file"
else
	cat "$out_file" > "$dst" || die "cannot write output file: $dst"
fi
