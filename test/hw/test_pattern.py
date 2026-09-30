"""Tests for the pattern module itself (no hardware needed)."""

import pattern
from pattern import analyze, make_line, make_lines, split_replay


def test_line_shape():
    line = make_line(7)
    assert len(line) == pattern.LINE_LEN == 62
    assert line.startswith(b"PL00000007 ") and line.endswith(b"\r\n")
    assert make_line(7) == line and make_line(8) != line


def test_strict_clean_stream():
    a = analyze(make_lines(100, 50))
    # everything before the first line terminator is ignored (it may be a partial
    # line), so the stream starts at the second line
    assert a.ok and a.seqs == list(range(101, 150))
    assert a.first == 101 and a.last == 149 and not a.gaps and not a.dups


def test_cut_in_the_middle_of_a_line():
    data = make_lines(0, 10)
    a = analyze(data[17:])
    assert a.ok and a.seqs == list(range(1, 10))


def test_detects_gap_and_dup_and_corruption():
    good = make_lines(0, 10)
    lines = good.split(b"\r\n")
    gap = b"\r\n".join(lines[:4] + lines[5:])
    a = analyze(gap)
    assert not a.ok and a.gaps == [(3, 1)]

    dup = b"\r\n".join(lines[:4] + [lines[3]] + lines[4:])
    a = analyze(dup)
    assert not a.ok and a.dups == [3]

    bad = bytearray(good)
    bad[62 * 4 + 20] ^= 0x01
    a = analyze(bytes(bad))
    assert not a.ok and len(a.bad_lines) == 1


def test_markers_strict_vs_lenient():
    good = make_lines(0, 10)
    marker = b"\r\n[picolog: BREAK]\r\n"
    data = good[:62 * 4] + marker + good[62 * 4:]
    assert not analyze(data, strict=True).ok
    a = analyze(data, strict=False)
    assert a.ok and a.markers == ["BREAK"]


def test_marker_splitting_a_line_joins_up():
    good = make_lines(0, 10)
    marker = b"\r\n[picolog: framing error x3]\r\n"
    cut = 62 * 4 + 30
    a = analyze(good[:cut] + marker + good[cut:], strict=False)
    assert a.ok and a.seqs == list(range(1, 10))
    assert a.markers == ["framing error x3"]


def test_gap_only_tolerated_after_loss_marker():
    good = make_lines(0, 20).split(b"\r\n")
    gapped = b"\r\n".join(good[:6] + good[12:])
    assert not analyze(gapped, strict=False).ok
    marker = b"\r\n[picolog: 4096 bytes dropped]\r\n"
    a = analyze(b"\r\n".join(good[:6]) + b"\r\n" + marker + b"\r\n".join(good[12:]), strict=False)
    assert a.ok and a.gaps and a.dropped_bytes() == 4096
    # a harmless marker does not excuse a gap
    marker2 = b"\r\n[picolog: something else]\r\n"
    a = analyze(b"\r\n".join(good[:6]) + b"\r\n" + marker2 + b"\r\n".join(good[12:]), strict=False)
    assert not a.ok


def test_split_replay():
    hdr = b"=== picolog replay: 124 bytes, uptime 1d 02:03:04 ===\r\n"
    hist = make_lines(5, 2)
    live = make_lines(7, 3)
    r = split_replay(hdr + hist + pattern.REPLAY_END + live)
    assert r.complete and r.header_bytes == 124 and r.uptime_s == 86400 + 2 * 3600 + 3 * 60 + 4
    assert r.history == hist and r.live == live
    r = split_replay(hdr + hist[:20])
    assert not r.complete and r.history == hist[:20]
    try:
        split_replay(b"garbage")
    except ValueError:
        pass
    else:
        raise AssertionError("expected ValueError")


def test_cut_at():
    old = make_lines(900, 5)
    new = make_lines(10, 6)
    assert analyze(pattern.cut_at(old + new, 10)).seqs == list(range(10, 16))
    assert analyze(pattern.cut_at(new, 10)).seqs == list(range(10, 16))
    try:
        pattern.cut_at(old, 10)
    except ValueError:
        pass
    else:
        raise AssertionError("expected ValueError")
