#!/usr/bin/env python3
"""UniXNU ADT step for raphael (Redmi K20 Pro / Mi 9T Pro, SM8150).

raphael is the same SM8150 silicon as guacamole (OnePlus 7 Pro); only the
board compatible string and the panel resolution differ (1080x2340). The
UniXNU payload driver passes the generic ADT flags (chip-id, owned-memory-size,
root-match UUID, CPU registers, PSCI method, trust cache, ...). The raphael
hardware profile in mkadt.py owns those values; accept the flags and generate
the D22AP/raphael tree with the requested DRAM size.
"""
import argparse
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--output", required=True)
    parser.add_argument("--dram-size", default="0xc0000000")
    parser.add_argument("--owned-memory-size", type=lambda value: int(value, 0))
    parser.add_argument("--base-d22ap", default=None,
                        help="path to base-d22ap.adt (iPhone X artifact)")
    known, _unknown = parser.parse_known_args()
    tool = Path(__file__).resolve().parent / "mkadt.py"
    command = [sys.executable, str(tool), known.output, "--device", "raphael",
               "--dram-size", known.dram_size]
    if known.owned_memory_size is not None:
        command += ["--owned-memory-size", str(known.owned_memory_size)]
    if known.base_d22ap is not None:
        command = [f"UNIXNU_D22AP_ADT={known.base_d22ap}", *command]
        # run through a shell so the env assignment takes effect
        import os
        env = dict(os.environ)
        env["UNIXNU_D22AP_ADT"] = known.base_d22ap
        subprocess.run(command[1:], check=True, env=env)
        return
    subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
