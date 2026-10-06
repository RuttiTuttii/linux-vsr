#!/usr/bin/env python3
# mp4 box surgery for dash segment rebuild
# parse ftyp/moov/moof/mdat layout, read tfdt base time and trun durations

import struct


# read box header at offset, return kind, size, header length
def read_box(data, off):
    # need at least size plus kind fields
    if off + 8 > len(data):
        return None
    # unpack big-endian size and fourcc kind
    size, kind = struct.unpack_from(">I4s", data, off)
    kind = kind.decode("latin1")
    # handle extended largesize form
    header = 8
    if size == 1:
        if off + 16 > len(data):
            return None
        (size,) = struct.unpack_from(">Q", data, off + 8)
        header = 16
    # handle zero size meaning to end of buffer
    if size == 0:
        size = len(data) - off
    # reject corrupt sizes
    if size < header or off + size > len(data):
        return None
    return (kind, size, header)


# walk top-level boxes of buffer
def walk_boxes(data):
    # iterate sibling boxes from buffer start
    off = 0
    while off + 8 <= len(data):
        box = read_box(data, off)
        if not box:
            break
        yield (box[0], off, box[1])
        off += box[1]


# split segment into init part and media part on first moof
def split_init_media(data):
    # scan top-level boxes for first moof marker
    for kind, off, size in walk_boxes(data):
        if kind == "moof":
            return (data[:off], data[off:])
    # no media part means whole buffer is init-like
    return (data, b"")


# find nested box by path from given payload range
def find_path(data, off, end, path):
    # descend one level per path element
    if not path:
        return (off, end)
    # scan children for matching kind
    cur = off
    while cur + 8 <= end:
        box = read_box(data, cur)
        if not box:
            return None
        kind, size, header = box
        if kind == path[0]:
            return find_path(data, cur + header, cur + size, path[1:])
        cur += size
    return None


# read tfdt base media decode time and timescale from init
def read_tfdt(media, timescale):
    # locate tfdt box inside moof and trafs
    for kind, off, size in walk_boxes(media):
        if kind != "moof":
            continue
        # search traf children for tfdt entry
        cur = off + 8
        end = off + size
        while cur + 8 <= end:
            box = read_box(media, cur)
            if not box:
                break
            skind, ssize, sheader = box
            if skind == "traf":
                found = find_path(media, cur + sheader, cur + ssize, ["tfdt"])
                if found:
                    foff, _ = found
                    # parse version and flags header
                    ver = media[foff]
                    if ver == 1:
                        (base,) = struct.unpack_from(">Q", media, foff + 4)
                    else:
                        (base,) = struct.unpack_from(">I", media, foff + 4)
                    return (base, timescale)
            cur += ssize
    return (None, timescale)


# read track timescale from init moov mdhd box
def read_timescale(init):
    # descend moov into first trak mdia mdhd chain
    found = find_path(init, 0, len(init), ["moov", "trak", "mdia", "mdhd"])
    if not found:
        return None
    # parse version to locate timescale field
    foff, _ = found
    ver = init[foff]
    if ver == 1:
        (scale,) = struct.unpack_from(">I", init, foff + 20)
    else:
        (scale,) = struct.unpack_from(">I", init, foff + 12)
    return scale if scale else None


# read track fragment default sample duration from tfhd box
def read_tfhd_default(media, traf_off, traf_end):
    # scan traf children for tfhd entry
    cur = traf_off
    while cur + 8 <= traf_end:
        box = read_box(media, cur)
        if not box:
            return None
        kind, size, header = box
        if kind == "tfhd":
            # parse flags for default duration presence
            flags = int.from_bytes(media[cur + header + 1:cur + header + 4], "big")
            if flags & 0x8:
                (dur,) = struct.unpack_from(">I", media, cur + header + 4)
                return dur
            return None
        cur += size
    return None


# sum trun sample durations for exact segment duration
def read_trun_duration(media, timescale):
    # locate trun boxes inside moof trafs
    total = 0
    found_any = False
    for kind, off, size in walk_boxes(media):
        if kind != "moof":
            continue
        cur = off + 8
        end = off + size
        while cur + 8 <= end:
            box = read_box(media, cur)
            if not box:
                break
            skind, ssize, sheader = box
            if skind == "traf":
                # read default duration fallback from tfhd
                default_dur = read_tfhd_default(media, cur + sheader, cur + ssize)
                res = find_path(media, cur + sheader, cur + ssize, ["trun"])
                if res:
                    roff, rend = res
                    # parse flags and sample count
                    flags = int.from_bytes(media[roff + 1:roff + 4], "big")
                    (count,) = struct.unpack_from(">I", media, roff + 4)
                    pos = roff + 8
                    # skip data offset field when present
                    if flags & 0x1:
                        pos += 4
                    # skip first sample flags when present
                    if flags & 0x4:
                        pos += 4
                    # walk per-sample entries
                    for _ in range(count):
                        # stop on truncated tables
                        if pos + 4 > rend:
                            break
                        dur = 0
                        # read present fields by flag bits
                        if flags & 0x100:
                            (dur,) = struct.unpack_from(">I", media, pos)
                            pos += 4
                        elif default_dur:
                            dur = default_dur
                        if flags & 0x200:
                            pos += 4
                        if flags & 0x400:
                            pos += 4
                        if flags & 0x800:
                            pos += 4
                        total += dur
                        found_any = True
            cur += ssize
    # convert timescale ticks to seconds
    if not found_any or not timescale:
        return None
    return total / float(timescale)
