"""Require the committed SDK pin to be published on the fork's haze branch."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

SDK_PATH = "external/ps3recomp"
SDK_URL = "https://github.com/zdiemer/ps3recomp.git"
HAZE_REF = "refs/remotes/origin/haze"


def git(root: Path, *args: str) -> str:
    result = subprocess.run(["git", "-C", str(root), *args],
                            capture_output=True, text=True)
    if result.returncode:
        raise ValueError(result.stderr.strip() or f"git {' '.join(args)} failed")
    return result.stdout.strip()


def check(root: Path, fetch: bool = False) -> str:
    for key, expected in (("url", SDK_URL), ("branch", "haze")):
        actual = git(root, "config", "--blob", "HEAD:.gitmodules", "--get",
                     f"submodule.{SDK_PATH}.{key}")
        if actual != expected:
            raise ValueError(f"SDK {key} must be {expected!r}, got {actual!r}")
    entry = git(root, "ls-tree", "HEAD", "--", SDK_PATH).split()
    if len(entry) != 4 or entry[:2] != ["160000", "commit"]:
        raise ValueError(f"{SDK_PATH} must be a committed submodule")
    pin = entry[2]
    sdk = root / SDK_PATH
    # An uninitialized directory otherwise lets git discover the parent repo.
    if not (sdk / ".git").exists():
        raise ValueError("SDK is uninitialized; run git submodule update --init --recursive")
    if git(sdk, "rev-parse", "HEAD") != pin:
        raise ValueError("SDK checkout must match the committed gitlink")
    if git(sdk, "status", "--porcelain", "--untracked-files=normal"):
        raise ValueError("SDK has local modifications")
    if fetch:
        options = ["--no-tags"]
        if git(sdk, "rev-parse", "--is-shallow-repository") == "true":
            options.append("--unshallow")
        # Fetch the canonical repository explicitly, independent of local aliases.
        git(sdk, "fetch", *options, SDK_URL, f"+refs/heads/haze:{HAZE_REF}")
    try:
        tip = git(sdk, "rev-parse", "--verify", f"{HAZE_REF}^{{commit}}")
    except ValueError as error:
        raise ValueError("Missing fetched haze history; rerun with --fetch") from error
    result = subprocess.run(["git", "-C", str(sdk), "merge-base", "--is-ancestor",
                             pin, tip], capture_output=True, text=True)
    if result.returncode == 1:
        raise ValueError(f"SDK pin {pin} is not on published haze ({tip}); "
                         "integrate/publish it to haze first (use --fetch to refresh)")
    if result.returncode:
        raise ValueError(result.stderr.strip() or "Cannot verify SDK ancestry")
    return pin


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--fetch", action="store_true", help="refresh canonical haze history")
    args = parser.parse_args()
    try:
        pin = check(args.root.resolve(), args.fetch)
    except ValueError as error:
        print(f"SDK pin check failed: {error}", file=sys.stderr)
        return 1
    print(f"SDK pin {pin} is reachable from zdiemer/ps3recomp:haze")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
