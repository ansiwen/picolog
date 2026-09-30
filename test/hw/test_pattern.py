"""Unit tests for the pattern generator/analyzer (no hardware needed)."""

import pytest

import pattern as p


def test_line_format():
    ln = p.line(1234)
    assert ln.startswith(b"PL00001234 ")
    assert ln.endswith(b"\r\n")
    assert len(ln) == p.LINE_LEN
    items = p.parse(ln)
    assert [(i.kind, i.seq) for i in items] == [("line", 1234)]


def test_contiguous_ok():
    rep = p.analyze(p.lines(10, 100))
    assert rep.errors == []
    assert rep.seqs == list(range(10, 110))


def test_leading_and_trailing_fragments_ok():
    data = p.lines(0, 20)
    rep = p.analyze(data[17:-5])
    assert rep.errors == []
    assert rep.seqs == list(range(1, 19))


def test_gap_detected():
    data = p.lines(0, 5) + p.lines(6, 5)
    rep = p.analyze(data)
    assert rep.errors == ["gap: line 6 after 4"]


def test_duplicate_detected():
    data = p.lines(0, 5) + p.lines(3, 5)
    rep = p.analyze(data)
    assert any("duplicate" in e for e in rep.errors)


def test_corruption_detected():
    data = bytearray(p.lines(0, 5))
    data[p.LINE_LEN * 2 + 20] ^= 0x01  # flip a payload bit in line 2
    rep = p.analyze(bytes(data))
    assert 2 not in rep.seqs
    assert rep.errors  # corrupt line shows up as unexpected text + gap


def test_marker_between_lines_strict_vs_lenient():
    data = p.lines(0, 3) + b"\r\n[picolog: BREAK]\r\n" + p.lines(3, 3)
    assert p.analyze(data).errors == ["unexpected marker: b'BREAK'"]
    rep = p.analyze(data, strict=False)
    assert rep.errors == []
    assert rep.markers == [b"BREAK"]


def test_marker_splitting_a_line_lenient():
    data = p.lines(0, 6)
    cut = p.LINE_LEN * 3 + 25  # middle of line 3
    data = data[:cut] + b"\r\n[picolog: framing error]\r\n" + data[cut:]
    rep = p.analyze(data, strict=False)
    assert rep.errors == []
    assert rep.seqs == [0, 1, 2, 4, 5]


def test_gap_after_dropped_marker_lenient():
    data = p.lines(0, 3) + b"\r\n[picolog: 999 bytes dropped]\r\n" + p.line(50)[7:] + p.lines(51, 3)
    rep = p.analyze(data, strict=False)
    assert rep.errors == []
    assert rep.markers_matching(b"dropped")


def test_unexplained_gap_lenient_still_error():
    data = p.lines(0, 3) + b"\r\n[picolog: BREAK]\r\n" + p.lines(10, 3)
    rep = p.analyze(data, strict=False)
    assert rep.errors == ["gap: line 10 after 2"]


def test_split_replay():
    hist = p.lines(0, 10)[30:]
    live = p.lines(10, 5)
    header = b"=== picolog replay: %d bytes, uptime 0d 00:01:02 ===\r\n" % len(hist)
    r = p.split_replay(header + hist + p.END_MARKER + live)
    assert r.size == len(hist)
    assert r.history == hist
    assert r.live == live
    # Seam check: removing the end marker yields a seamless stream.
    assert p.analyze(r.history + r.live).errors == []


def test_split_replay_rejects_garbage():
    with pytest.raises(ValueError):
        p.split_replay(b"hello\r\nworld")
