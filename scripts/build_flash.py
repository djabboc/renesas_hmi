"""Build this RT-Thread Studio project and flash it through the configured DAP-LINK."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from configure_rw007_network import configure


PROJECT_ROOT = Path(__file__).resolve().parent.parent


def version_key(path: Path) -> tuple[int, ...]:
    for parent in path.parents:
        if re.search(r"\d", parent.name):
            return tuple(int(part) for part in re.findall(r"\d+", parent.name))
    return ()


def find_studio_tool(studio_root: Path, pattern: str, label: str) -> Path:
    matches = list(studio_root.glob(pattern))
    if not matches:
        raise FileNotFoundError(
            f"Cannot find {label} under {studio_root}. Pass --studio-root or set RTTHREAD_STUDIO."
        )
    return max(matches, key=version_key)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--studio-root",
        type=Path,
        default=Path(os.environ.get("RTTHREAD_STUDIO", r"C:\RT-ThreadStudio")),
        help="RT-Thread Studio installation directory",
    )
    parser.add_argument("--target", default="r7fa6m3ah", help="PyOCD target name")
    parser.add_argument("--build-only", action="store_true", help="Build without flashing")
    args = parser.parse_args()

    configure(PROJECT_ROOT)

    image = PROJECT_ROOT / "Debug" / "rtthread.hex"
    studio = args.studio_root / "eclipsec.exe"
    if not studio.is_file():
        raise FileNotFoundError(f"Cannot find eclipsec.exe under {args.studio_root}")
    pyocd = find_studio_tool(
        args.studio_root,
        "repo/Extract/Debugger_Support_Packages/RealThread/PyOCD/*/pyocd.exe",
        "PyOCD",
    )

    print(f"Building {PROJECT_ROOT.name} with RT-Thread Studio", flush=True)
    with tempfile.TemporaryDirectory(prefix="rttstudio-headless-") as workspace:
        process = subprocess.Popen(
            [
                str(studio),
                "-nosplash",
                "-data",
                workspace,
                "-application",
                "org.eclipse.cdt.managedbuilder.core.headlessbuild",
                "-import",
                str(PROJECT_ROOT),
                "-build",
                "project/Debug",
                "-printErrorMarkers",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
        build_errors = None
        assert process.stdout is not None
        for line in process.stdout:
            print(line, end="", flush=True)
            summary = re.search(r"Build Finished\.\s+(\d+) errors", line)
            if summary:
                build_errors = int(summary.group(1))
                if build_errors:
                    process.terminate()
        exit_code = process.wait()
        if exit_code or build_errors != 0:
            raise RuntimeError(f"Build not verified: exit={exit_code}, summary errors={build_errors}; refusing to flash")
    if not image.is_file():
        raise FileNotFoundError(f"Build did not produce {image}")
    print(f"Build complete: {image}", flush=True)

    if args.build_only:
        return 0

    print(f"Flashing {args.target} through DAP-LINK", flush=True)
    subprocess.run(
        [str(pyocd), "flash", "--target", args.target, "--frequency", "1000000", str(image)],
        check=True,
        cwd=pyocd.parent,
    )
    print("Flash complete; PyOCD resets the target after programming.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, subprocess.CalledProcessError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
