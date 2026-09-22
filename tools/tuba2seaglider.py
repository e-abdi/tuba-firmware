#!/usr/bin/env python3
"""
tuba2seaglider.py — turn a Tuba console capture into Seaglider-style dive files.

Input is whatever the glider printed: an OpenLog SD capture, or a session log
from tools/tuba_console.py. Both contain the human-readable prose *and* the
machine-readable records the firmware interleaves (see include/dive_log.h).
This script pulls out the records and writes, per dive:

    pTTTDDDD.log    parameters, GPS fixes, phase transitions, motor moves
    pTTTDDDD.eng    the sampled data table, in engineering units

where TTT is the glider id (default 001) and DDDD the dive number, matching
the Seaglider p1230055.log / p1230055.eng convention that basestation tooling
expects.

Usage:
    ./tuba2seaglider.py capture.txt
    ./tuba2seaglider.py capture.txt -o /path/to/mission/dir --glider 7
    ./tuba2seaglider.py capture.txt --list        # just show what is in there

--- What does NOT carry over ---

Tuba is not a Seaglider, and pretending otherwise would corrupt anything
downstream that trusts the units:

  * Control columns are SECONDS OF MOTOR TRAVEL, not cm / degrees / cc. The
    real .eng columns pitchCtl, rollCtl and vbdCC are calibrated engineering
    units derived from position feedback; Tuba dead-reckons from run time and
    has no feedback yet. The columns are therefore named pitchCtl_s,
    rollCtl_s and vbdCtl_s, and a %units line in the .eng header says so.
    Do NOT feed them to a Seaglider flight model as-is.
  * There is no CT sail, so there is no conductivity, salinity or density —
    only the TSYS01 water temperature, as a waterT column.
  * elaps_t_0000 (seconds since 0000 UTC) is reconstructed from the $START
    record and the per-sample elapsed time. If the glider had no GPS fix, its
    clock came from the firmware build date and this column is approximate.
"""

import argparse
import os
import re
import sys
from collections import OrderedDict

# A structured record: '$' in column 0, a NAME, then comma-separated fields.
# The capture may carry a [hh:mm:ss] prefix from tuba_console.py, and lines
# can be prefixed by the menu's \r, so the anchor is tolerant.
RECORD_RE = re.compile(r"^(?:\[\d\d:\d\d:\d\d\]\s*)?\$([A-Z_0-9]+),?(.*)$")

# Records that carry structure rather than a parameter value.
STRUCTURAL = {
    "VERSION", "GLIDER", "MISSION", "DIVE", "SIMULATE", "START",
    "COLUMNS", "GPS1", "GPS2", "STATE", "GC", "D", "ERRORS", "FINISH",
}


class Dive:
    def __init__(self):
        self.version = None
        self.build_date = None
        self.glider = "tuba"
        self.mission = 0
        self.number = 0
        self.simulated = False
        self.start = None          # (month, day, year, hour, minute, second)
        self.columns = []
        self.params = OrderedDict()
        self.gps = []              # (which, fields) in capture order
        self.states = []           # raw field lists
        self.gc = []
        self.data = []             # list of field lists
        self.errors = None
        self.finish = None

    @property
    def aborted(self):
        """True when any $STATE reason is an abort rather than a normal end."""
        normal = {"CONTROL_FINISHED_OK", "TARGET_DEPTH_REACHED", "DIVE_TIMEOUT",
                  "SURFACE_DEPTH_REACHED", "RECOVERY_POSITION_SET"}
        for fields in self.states:
            if len(fields) >= 3 and fields[2] and fields[2] not in normal:
                return True
        return False

    @property
    def abort_reason(self):
        normal = {"CONTROL_FINISHED_OK", "TARGET_DEPTH_REACHED", "DIVE_TIMEOUT",
                  "SURFACE_DEPTH_REACHED", "RECOVERY_POSITION_SET"}
        for fields in self.states:
            if len(fields) >= 3 and fields[2] and fields[2] not in normal:
                return fields[2]
        return None

    @property
    def start_seconds_of_day(self):
        if not self.start or len(self.start) < 6:
            return None
        try:
            _, _, _, hh, mm, ss = (int(x) for x in self.start[:6])
        except ValueError:
            return None
        return hh * 3600 + mm * 60 + ss


def parse(lines):
    """Split a capture into Dive objects.

    A dive begins at $VERSION, which the firmware emits once per cycle as the
    first record of the header. Anything before the first $VERSION is menu
    noise and is dropped.
    """
    dives = []
    cur = None

    for raw in lines:
        m = RECORD_RE.match(raw.rstrip("\r\n"))
        if not m:
            continue
        name, rest = m.group(1), m.group(2)
        fields = [f.strip() for f in rest.split(",")] if rest else []

        if name == "VERSION":
            cur = Dive()
            dives.append(cur)
            if fields:
                cur.version = fields[0]
            if len(fields) > 1:
                cur.build_date = ",".join(fields[1:])
            continue

        if cur is None:
            continue    # record before any header — ignore

        if name == "GLIDER":
            cur.glider = fields[0] if fields else "tuba"
        elif name == "MISSION":
            cur.mission = int(fields[0]) if fields and fields[0].isdigit() else 0
        elif name == "DIVE":
            cur.number = int(fields[0]) if fields and fields[0].isdigit() else 0
        elif name == "SIMULATE":
            cur.simulated = bool(fields) and fields[0] == "1"
        elif name == "START":
            # "$START,mm dd yyyy hh mm ss" — space separated, one field.
            cur.start = fields[0].split() if fields else []
        elif name == "COLUMNS":
            cur.columns = fields
        elif name in ("GPS1", "GPS2"):
            cur.gps.append((name, fields))
        elif name == "STATE":
            cur.states.append(fields)
        elif name == "GC":
            cur.gc.append(fields)
        elif name == "D":
            cur.data.append(fields)
        elif name == "ERRORS":
            cur.errors = fields
        elif name == "FINISH":
            cur.finish = fields
        elif name not in STRUCTURAL:
            # Everything else in the header block is a parameter.
            cur.params[name] = ",".join(fields)

    return dives


def seaglider_start(start):
    """Seaglider's start: line is 'month day year-1900 hour min sec'."""
    if not start or len(start) < 6:
        return None
    try:
        mo, da, yr, hh, mi, se = (int(x) for x in start[:6])
    except ValueError:
        return None
    return f"{mo} {da} {yr - 1900} {hh} {mi} {se}"


def write_log(dive, path):
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(f"version: {dive.version or 'unknown'}\n")
        fh.write(f"glider: {dive.glider}\n")
        fh.write(f"mission: {dive.mission}\n")
        fh.write(f"dive: {dive.number}\n")
        start = seaglider_start(dive.start)
        if start:
            fh.write(f"start: {start}\n")
        if dive.simulated:
            fh.write("# SIMULATED DIVE — depth is synthetic, not measured\n")
        fh.write("data:\n")

        for key, val in dive.params.items():
            fh.write(f"${key},{val}\n")

        for which, fields in dive.gps:
            fh.write(f"${which}," + ",".join(fields) + "\n")

        # $STATE and $GC interleaved by elapsed time, as a Seaglider .log has
        # them, so the sequence reads in the order it actually happened.
        events = []
        for f in dive.states:
            events.append((int(f[0]) if f and f[0].isdigit() else 0, "STATE", f))
        for f in dive.gc:
            events.append((int(f[0]) if f and f[0].isdigit() else 0, "GC", f))
        events.sort(key=lambda e: e[0])
        for _, kind, f in events:
            fh.write(f"${kind}," + ",".join(f) + "\n")

        if dive.errors:
            fh.write("# $ERRORS: ms5837,bmp180,tsys01,hmc6343,gps (failed read attempts)\n")
            fh.write("$ERRORS," + ",".join(dive.errors) + "\n")
        if dive.finish:
            fh.write("$FINISH," + ",".join(dive.finish) + "\n")


# Column name -> unit, for the %units line. Names come from the firmware's
# $COLUMNS record; anything unknown is passed through as "?".
UNITS = {
    "elaps_t_0000": "seconds",
    "elaps_t": "seconds",
    "depth": "cm",
    "head": "degrees",
    "pitchAng": "degrees",
    "rollAng": "degrees",
    "pitchCtl_s": "seconds_of_travel",
    "rollCtl_s": "seconds_of_travel",
    "vbdCtl_s": "seconds_of_travel",
    "GC_phase": "code",
    "intP": "Pa",
    "waterT": "degC",
    "rec": "count",
}


def write_eng(dive, path):
    cols = list(dive.columns) if dive.columns else []
    if not cols:
        print(f"  warning: dive {dive.number} has no $COLUMNS record; "
              f"skipping .eng", file=sys.stderr)
        return False

    # The .eng adds elaps_t_0000 at the front and rec at the end, as the
    # basestation does.
    out_cols = ["elaps_t_0000"] + cols + ["rec"]
    sod = dive.start_seconds_of_day

    with open(path, "w", encoding="utf-8") as fh:
        fh.write(f"%version: {dive.version or 'unknown'}\n")
        fh.write(f"%glider: {dive.glider}\n")
        fh.write(f"%mission: {dive.mission}\n")
        fh.write(f"%dive: {dive.number}\n")
        start = seaglider_start(dive.start)
        if start:
            fh.write(f"%start: {start}\n")
        fh.write("%basestation_version: tuba2seaglider.py\n")
        if dive.simulated:
            fh.write("%simulated: 1\n")
        if dive.abort_reason:
            fh.write(f"%abort: {dive.abort_reason}\n")
        fh.write("%columns: " + ",".join(out_cols) + "\n")
        fh.write("%units: " + ",".join(UNITS.get(c, "?") for c in out_cols) + "\n")
        fh.write("%note: control columns are seconds of motor travel, "
                 "not cm/degrees/cc — Tuba has no position feedback\n")
        fh.write("%data:\n")

        for rec, row in enumerate(dive.data, start=1):
            if len(row) != len(cols):
                # A line the console interleaved output into. Skipping is
                # safer than emitting a misaligned row.
                continue
            try:
                elaps = int(row[0])
            except ValueError:
                continue
            t0000 = "" if sod is None else str((sod + elaps) % 86400)
            fh.write(" ".join([t0000] + row + [str(rec)]) + "\n")
    return True


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", help="OpenLog or tuba_console capture file")
    ap.add_argument("-o", "--outdir", default=".", help="output directory")
    ap.add_argument("--glider", type=int, default=1,
                    help="glider id used in the file names (default 1)")
    ap.add_argument("--list", action="store_true",
                    help="list the dives found and exit without writing")
    ap.add_argument("--skip-simulated", action="store_true",
                    help="ignore simulated dives (Seaglider keeps deck dives "
                         "out of the real mission directory for the same reason)")
    args = ap.parse_args()

    try:
        with open(args.capture, "r", encoding="utf-8", errors="replace") as fh:
            dives = parse(fh)
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if not dives:
        print("No dive records found. Does the capture contain '$VERSION' lines?",
              file=sys.stderr)
        return 1

    if args.skip_simulated:
        dives = [d for d in dives if not d.simulated]

    print(f"{len(dives)} dive(s) in {args.capture}")
    for d in dives:
        flags = []
        if d.simulated:
            flags.append("SIMULATED")
        if d.abort_reason:
            flags.append(f"ABORTED:{d.abort_reason}")
        suffix = ("  [" + ", ".join(flags) + "]") if flags else ""
        print(f"  mission {d.mission} dive {d.number}: "
              f"{len(d.data)} samples, {len(d.gc)} motor moves{suffix}")

    if args.list:
        return 0

    os.makedirs(args.outdir, exist_ok=True)
    written = 0
    for d in dives:
        stem = f"p{args.glider:03d}{d.number:04d}"
        log_path = os.path.join(args.outdir, stem + ".log")
        eng_path = os.path.join(args.outdir, stem + ".eng")
        write_log(d, log_path)
        if write_eng(d, eng_path):
            print(f"  wrote {log_path} and {eng_path}")
        else:
            print(f"  wrote {log_path} (no .eng — no data rows)")
        written += 1

    print(f"{written} dive(s) converted into {args.outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
