#!/usr/bin/env python3
"""
airoha_verify_lzma.py - Build gate for the LZMA-Alone streams Airoha's BL2 loads

BL2 (the preloader) decompresses u-boot.bin.lzma and needs the uncompressed size
in the 13-byte LZMA-Alone header to be the *real* value.  With -1 ("unknown
size") it aborts the boot of an otherwise perfectly fine image with

  ERROR:   Failed to decompress image (err=1)
  ERROR:   BL2: Failure in post image load handling (1)
  ERROR:   Stored BL31 + U-Boot FIP failed (1)

This tool is the final gate of tools/build_airoha: it checks the header, decodes
the payload and, with --expect, compares the result byte for byte with the file
the stream was made from.

The payload is decoded with liblzma's *raw* mode on purpose: that is what BL2's
LZMA SDK decoder does (decode as much as the declared size asks for), and unlike
the container-aware mode it does not reject the "real size + end marker" stream
that the xz fallback of tools/lzma_wrapper.sh produces.  Raw mode may decode a
few bytes past the declared size (the encoder's range coder flush), so the
result is truncated to the declared size before it is compared.

Usage:
  python3 airoha_verify_lzma.py <image.lzma> --expect <original.bin>
  python3 airoha_verify_lzma.py <image.lzma>     # header + decode only
  python3 airoha_verify_lzma.py <image.lzma> --expect <original.bin> --repair
        # write the real uncompressed size into the header first (build mode)

Exit status: 0 = this is a stream BL2 can boot, 1 = it is not.
"""

import argparse
import hashlib
import lzma
import os
import struct
import sys

UNKNOWN_SIZE = 0xFFFFFFFFFFFFFFFF
# Ceiling for a decode of an unknown/oversized stream, so a corrupt header
# cannot make this tool allocate forever.
MAX_DECODE = 512 * 1024 * 1024
RULE = '=' * 60


def header(data):
    """Return (props, dict_size, uncompressed_size) of the LZMA-Alone header."""
    return (data[0],
            struct.unpack_from('<I', data, 1)[0],
            struct.unpack_from('<Q', data, 5)[0])


def filters_of(data):
    """LZMA1 filter list as described by the stream's properties byte."""
    props, dict_size, _ = header(data)
    return [{
        'id': lzma.FILTER_LZMA1,
        'dict_size': dict_size,
        'lc': props % 9,
        'lp': (props // 9) % 5,
        'pb': props // 45,
    }]


def decode_raw(data, limit):
    """Decode the payload the way BL2's LZMA SDK decoder does.

    Returns (decoded, saw_end_marker).  The result may be a little longer than
    the declared uncompressed size; the caller truncates it.
    """
    dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW,
                                filters=filters_of(data))
    out = dec.decompress(data[13:], max_length=limit)
    while len(out) < limit and not dec.eof and not dec.needs_input:
        more = dec.decompress(b'', max_length=limit - len(out))
        if not more:
            break
        out += more
    return out, dec.eof


def md5_of(blob):
    return hashlib.md5(blob).hexdigest()


def report(args, data, decoded_md5, decoded_len, errors, repaired=None):
    props, dict_size, declared = header(data)
    print(RULE)
    print('  LZMA-ALONE STREAM VALIDATION')
    print(RULE)
    print('  %-18s %s (%d bytes)' % ('File:', args.image, len(data)))
    print('  %-18s props=0x%02X, dictionary=%d bytes, uncompressed=%s' %
          ('Header:', props, dict_size,
           'unknown (-1)' if declared == UNKNOWN_SIZE else declared))
    if repaired is not None:
        print('  %-18s %d bytes (header repaired)' % ('Uncompressed size:', repaired))
    if decoded_len is not None:
        print('  %-18s %d bytes, md5 %s' % ('Decoded:', decoded_len, decoded_md5))
    if args.expect:
        print('  %-18s %s (%d bytes, md5 %s)' %
              ('Expected:', args.expect, os.path.getsize(args.expect),
               md5_of(open(args.expect, 'rb').read())))
    print('')
    print('  %-18s %s' % ('Result:', 'FAIL' if errors else 'PASS'))
    for err in errors:
        print('  ERROR:            %s' % err)
    print(RULE)


def main():
    ap = argparse.ArgumentParser(
        description='Verify that an LZMA-Alone stream is bootable by Airoha BL2.')
    ap.add_argument('image', help='LZMA-Alone stream to check (e.g. u-boot.bin.lzma)')
    ap.add_argument('--expect', metavar='FILE',
                    help='uncompressed file the stream was made from')
    ap.add_argument('--repair', action='store_true',
                    help='write the real uncompressed size into the header first')
    ap.add_argument('--quiet', action='store_true',
                    help='print the result line only')
    args = ap.parse_args()

    errors = []
    try:
        data = open(args.image, 'rb').read()
    except OSError as exc:
        sys.exit('ERROR: cannot read %s: %s' % (args.image, exc))

    expected = None
    if args.expect:
        try:
            expected = open(args.expect, 'rb').read()
        except OSError as exc:
            sys.exit('ERROR: cannot read %s: %s' % (args.expect, exc))

    if len(data) < 14:
        report(args, data + b'\x00' * (13 - len(data)), None, None,
               ['file is too short to be an LZMA-Alone stream'])
        return 1

    props, dict_size, declared = header(data)
    if props >= 225:
        errors.append('properties byte 0x%02X is not a valid LZMA setting' % props)
    if not 4096 <= dict_size <= 1 << 32:
        errors.append('dictionary size %d is out of range' % dict_size)

    # The declared size has to be the real one; BL2 cannot work with -1.
    real_size = len(expected) if expected is not None else None
    if real_size is None and declared == UNKNOWN_SIZE and args.repair:
        out, eof = decode_raw(data, MAX_DECODE)
        real_size = len(out) if eof else None
    patched = False
    if args.repair and real_size is not None and declared != real_size:
        buf = bytearray(data)
        struct.pack_into('<Q', buf, 5, real_size)
        open(args.image, 'wb').write(buf)
        data = bytes(buf)
        props, dict_size, declared = header(data)
        patched = True

    if declared == UNKNOWN_SIZE:
        errors.append('header declares -1 (unknown size); BL2 rejects such an '
                      'image - rerun with --repair to fix it')
    elif declared == 0:
        errors.append('header declares an uncompressed size of 0')

    decoded = decoded_len = decoded_md5 = None
    if not errors or declared != UNKNOWN_SIZE:
        limit = min(MAX_DECODE, declared + 4096) if declared not in (0, UNKNOWN_SIZE) else MAX_DECODE
        try:
            out, eof = decode_raw(data, limit)
        except lzma.LZMAError as exc:
            errors.append('payload is not a valid LZMA1 stream: %s' % exc)
        else:
            if declared != UNKNOWN_SIZE and len(out) < declared:
                errors.append('stream is truncated: it decodes to %d bytes, the '
                              'header declares %d' % (len(out), declared))
            elif declared != UNKNOWN_SIZE:
                # BL2 stops at the declared size; anything the range coder
                # flushed after it is irrelevant.
                out = out[:declared]
                decoded = out
                decoded_len = len(out)
                decoded_md5 = md5_of(out)
                if expected is not None:
                    if declared != len(expected):
                        errors.append('declared size %d does not match %s (%d bytes)' %
                                      (declared, args.expect, len(expected)))
                    elif decoded_md5 != md5_of(expected):
                        errors.append('decoded payload (md5 %s) does not match %s '
                                      '(md5 %s)' % (decoded_md5, args.expect,
                                                    md5_of(expected)))

    if args.quiet:
        print('%s: %s' % (args.image, 'FAIL' if errors else 'PASS'))
    else:
        report(args, data, decoded_md5, decoded_len, errors,
               repaired=real_size if patched else None)
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
