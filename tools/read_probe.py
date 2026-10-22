"""Read a --probe health line file and say whether the audio path was well behaved.

Phase 22 asks for "music plays, correct pitch and tempo, no crackling, no dropouts, no desync
over several minutes". Those are ear judgments. This checks their measurable shadows, so the
phase can be verified by someone, or something, with no ears:

  pitch and tempo   the device rate must equal the rate the game asked for. SDL is opened with
                    allowed_changes = 0, so a mismatch is a pitch error of exactly that ratio.
                    The sample count per second is checked against the rate for the same reason:
                    it catches the whole family of "off by a factor" bugs, of which this project
                    has already shipped one.

  dropouts          a RUN of empty queue polls, never a single one. See below.

  crackling         roughness, sum|x[n]-x[n-1]| over sum|x[n]|. Anything musical is smooth at the
                    sample level and sits well under 1. Whatever happens to be in memory is not,
                    and lands near 1.5 and up. This is the statistic that would have caught the
                    byte-count bug in Phase 20 in one second rather than by ear.

THE SINGLE EMPTY POLL IS NOT A DROPOUT, and reading it as one was this tool's first answer. It
failed a run that measured 1.4% of polls at zero with 38ms of audio buffered every second, which
is not a starved device. SDL's queue count drops frames the moment its audio thread pulls a chunk
into the device buffer, so a poll landing just after a pull reads zero while the device is fed for
the next 32ms. What distinguishes starvation is CONSECUTIVE zeros: the device holds one chunk, so
a run means nothing arrived to replace it.

It reports, then exits non-zero if anything failed, so it can gate a script.

    python tools/read_probe.py build-cmake/probe.txt
"""

import io
import sys

# Consecutive empty polls. Polls run at about one per VI, so a run of 3 is roughly 50ms with
# nothing queued behind a device buffer that holds 32ms. That is a real gap. One or two is the
# pull-timing artifact described above.
MAX_ZERO_RUN = 2

# Roughness above this is not audio. Set well clear of real music, which measures far lower.
MAX_MEAN_ROUGHNESS = 1.0

# The sample count per second should match the device rate times two channels. Allow a little
# slack for a second that straddles a pause, and none for a factor of two.
RATE_TOLERANCE = 0.25

COLUMNS = ("line", "vis", "hz", "rate_ok", "buffers", "samples",
           "qmin", "qmean", "qmax", "polls", "under", "zero_run", "rough")


def read(path):
    rows = []
    for line in io.open(path, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f = line.split()
        if len(f) != len(COLUMNS):
            continue
        row = dict(zip(COLUMNS, [int(x) for x in f[:-1]] + [float(f[-1])]))
        rows.append(row)
    return rows


def main(path):
    rows = read(path)
    if not rows:
        print("FAIL  no usable rows in", path)
        print("      (a file written by an older build has a different column count)")
        return 1

    # Judge only the stretch where audio was actually flowing. The logo sequence runs before the
    # device is open, and scoring silence as a pass would be the easiest way to pass wrongly.
    live = [r for r in rows if r["buffers"] > 0]
    print("seconds recorded      :", len(rows))
    print("seconds with audio    :", len(live))
    if not live:
        print("FAIL  no audio was ever queued")
        return 1

    failures = []

    rates = sorted({r["hz"] for r in live})
    mismatches = [r for r in live if not r["rate_ok"]]
    print("device rate (Hz)      :", rates)
    print("rate matched request  :", "yes" if not mismatches else "NO, %d seconds" % len(mismatches))
    if mismatches:
        failures.append("the device rate did not match the rate the game asked for")

    expected = live[0]["hz"] * 2
    counts = [r["samples"] for r in live]
    mean_samples = sum(counts) / len(counts)
    ratio = mean_samples / expected if expected else 0.0
    print("samples per second    : mean %d, expected %d, ratio %.3f"
          % (mean_samples, expected, ratio))
    if abs(ratio - 1.0) > RATE_TOLERANCE:
        failures.append("the sample rate through the queue is %.2fx what the device expects" % ratio)

    vis = [r["vis"] for r in live]
    print("VIs per second        : min %d, max %d" % (min(vis), max(vis)))

    hz = live[0]["hz"]
    qmean = [r["qmean"] for r in live]
    print("queue depth (frames)  : mean of means %d, high water %d  (%.0f ms buffered)"
          % (sum(qmean) / len(qmean), max(r["qmax"] for r in live),
             1000.0 * (sum(qmean) / len(qmean)) / hz))

    worst_run = max(r["zero_run"] for r in live)
    runs = [r for r in live if r["zero_run"] > MAX_ZERO_RUN]
    print("empty polls           : %d of %d (%.2f%%), longest run %d"
          % (sum(r["under"] for r in live), sum(r["polls"] for r in live),
             100.0 * sum(r["under"] for r in live) / sum(r["polls"] for r in live),
             worst_run))
    if runs:
        w = max(runs, key=lambda r: r["zero_run"])
        print("   worst second       : line %d, a run of %d" % (w["line"], w["zero_run"]))
        failures.append("the queue was empty for a run of %d polls, which is a real gap" % worst_run)

    rough = [r["rough"] for r in live]
    mean_rough = sum(rough) / len(rough)
    print("roughness             : min %.3f, mean %.3f, max %.3f"
          % (min(rough), mean_rough, max(rough)))
    if mean_rough > MAX_MEAN_ROUGHNESS:
        failures.append("roughness %.2f: this is noise, not audio" % mean_rough)

    print()
    if failures:
        for f in failures:
            print("FAIL ", f)
        return 1
    print("PASS  rate correct, no sustained gaps, and the stream is audio rather than noise")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "build-cmake/probe.txt"))
