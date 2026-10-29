#!/usr/bin/env python3
"""The Metal build prepares the pinned translator and the local Rust toolchain.

The core plugin builder owns the Metal frontend, translated shader runtime and
signed bundle. This entry selects the A642L ABI and display integration options.
The device addon packager installs the resulting bundle and trust entries.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def run(command, environment, directory):
    subprocess.run([str(value) for value in command], env=environment,
                   cwd=directory, check=True)


def ios_toolchain_installed(rustup, environment, directory):
    result = subprocess.run([rustup, "toolchain", "list"], env=environment,
                            cwd=directory, check=True, capture_output=True, text=True)
    for line in result.stdout.splitlines():
        name = line.split()[0]
        if not name.startswith("1.98.0-"):
            continue

        toolchain = Path(environment["RUSTUP_HOME"]) / "toolchains" / name
        libraries = toolchain / "lib/rustlib/aarch64-apple-ios/lib"
        if ((toolchain / "bin/rustc").is_file()
                and (toolchain / "bin/cargo").is_file()
                and any(libraries.glob("libstd-*.rlib"))):
            return True

    return False


def prepare_translator(source, pin, environment, directory):
    """The translator checkout contains the selected revision and recorded patches.

    A revision change preserves the previous checkout beside it, including its
    expanded patches. The replacement reuses the available Git objects and
    checks out the new revision before the core builder applies its series.
    """
    if (source / ".git").exists():
        revision = subprocess.check_output(
            ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        if revision == pin["commit"]:
            return

        previous = source.with_name(f"{source.name}-{revision[:12]}-{time.time_ns()}")
        source.rename(previous)
        print(f"Preserved translator source: {previous}", flush=True)
        run(["git", "clone", "--no-checkout", "--no-hardlinks", previous, source],
            environment, directory)
        run(["git", "-C", source, "remote", "set-url", "origin", pin["repository"]],
            environment, directory)
    else:
        source.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "init", source], environment, directory)
        run(["git", "-C", source, "remote", "add", "origin", pin["repository"]],
            environment, directory)

    available = subprocess.run(
        ["git", "-C", str(source), "rev-parse", "--verify", pin["commit"] + "^{commit}"],
        env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if available.returncode:
        run(["git", "-C", source, "fetch", "--depth=1", "origin", pin["commit"]],
            environment, directory)

    run(["git", "-C", source, "checkout", "--detach", pin["commit"]],
        environment, directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=Path, required=True)
    parser.add_argument("--abi-directory", type=Path, required=True)
    args = parser.parse_args()
    core = args.core.resolve()
    cache = core / ".cache"
    source = cache / "gpu-reference/metal2vulkan"
    pin = json.loads((core / "scripts/data/gpu-metal-vulkan-inputs.json").read_text())["translator"]
    environment = dict(os.environ, UNIXNU_RECORD_HASHES="0",
                       RUSTUP_HOME=str(cache / "metal-rustup"),
                       CARGO_HOME=str(cache / "metal-cargo"))
    rustup = shutil.which("rustup")
    if not rustup:
        raise SystemExit("rustup is required to install the pinned iOS Rust toolchain")

    environment["PATH"] = os.pathsep.join([
        str(Path(rustup).parent), str(cache / "mesa-tools/bin"),
        environment.get("PATH", ""),
    ])
    prepare_translator(source, pin, environment, core)

    run([rustup, "set", "auto-self-update", "disable"], environment, core)
    if not ios_toolchain_installed(rustup, environment, core):
        run([rustup, "toolchain", "install", "1.98.0", "--profile", "minimal",
             "--target", "aarch64-apple-ios", "--no-self-update"], environment, core)
    run([sys.executable, core / "scripts/build-adreno-metal-plugin.py",
         "--gpu-abi-dir", args.abi_directory.resolve(),
         "--scanout-copy", "off", "--quartzcore-display-state", "off",
         "--turnip-replay", "off", "--turnip-debug", "", "--turnip-smoke-debug", "",
         "--shader-compiler", cache / "mesa-tools/bin/glslangValidator",
         "--skip-shader-validation"], environment, core)


if __name__ == "__main__":
    main()
