#!/usr/bin/env python3
"""Prepare pinned Mesa and host build tools for the Darwin Turnip build.

The UniXNU checkout supplies the Mesa revision, sparse directories, patch order
and Python requirements. glslang 15.1.0 and bison 3.8.2 build in its .cache when
the matching tools are absent. Downloads, source checkouts, logs and installed
host tools stay in that checkout. No tests or file confirmation hashes run.

Example:
  python3 tools/gpu/bootstrap-turnip.py --core ../UniXNU
  UNIXNU_RECORD_HASHES=0 ../UniXNU/.cache/mesa-venv/bin/python \
    ../UniXNU/scripts/build-turnip-darwin.py --target ios \
    --abi-directory drivers/Adreno6xx

--build also invokes that existing library builder after preparing its inputs.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request

from build_inputs import prepare_cache


GLSLANG_REVISION = "1062752a891c95b2bfeed9e356562d88f9df84ac"
GLSLANG_REPOSITORY = "https://chromium.googlesource.com/external/github.com/KhronosGroup/glslang"
GLSLANG_VERSION = "15.1.0"
BISON_VERSION = "3.8.2"


class Bootstrap:
    """Bootstrap prepares the source and host tools consumed by the core builder.

    Each command runs in a named workspace directory and saves its output in
    .cache/turnip-bootstrap. Existing Mesa edits must match the core's recorded
    patch sequence before they are reused. The core library builder owns the
    cross file, Mesa configuration, compilation and final iOS dylib.
    """

    def __init__(self, core, jobs):
        self.core = core
        self.jobs = jobs
        self.cache = core / ".cache"
        self.logs = self.cache / "turnip-bootstrap"
        self.tools = self.cache / "mesa-tools"
        self.venv = self.cache / "mesa-venv"
        self.cache.mkdir(parents=True, exist_ok=True)
        self.logs.mkdir(parents=True, exist_ok=True)
        (self.tools / "bin").mkdir(parents=True, exist_ok=True)
        self.environment = dict(os.environ, UNIXNU_RECORD_HASHES="0")
        self.environment["PATH"] = os.pathsep.join([
            str(self.tools / "bin"), str(self.venv / "bin"),
            self.environment.get("PATH", ""),
        ])
        self.pins = json.loads((core / "scripts/data/adreno-compiler-inputs.json").read_text())

    def run(self, name, command, cwd=None):
        command = [str(value) for value in command]
        directory = Path(cwd) if cwd else self.core
        log = self.logs / (name + ".log")
        (self.logs / (name + ".command.json")).write_text(json.dumps({
            "command": command,
            "cwd": str(directory),
        }, indent=2) + "\n")
        print(f"Turnip bootstrap: {name}", flush=True)
        with log.open("w") as output:
            result = subprocess.run(command, cwd=directory, env=self.environment,
                                    stdout=output, stderr=subprocess.STDOUT)
        if result.returncode:
            tail = "\n".join(log.read_text(errors="replace").splitlines()[-20:])
            raise SystemExit(f"{name} failed; see {log}\n{tail}")

    def read(self, command, cwd=None):
        return subprocess.check_output([str(value) for value in command],
                                       cwd=cwd or self.core, env=self.environment,
                                       text=True).strip()

    def checkout(self, name, path, repository, revision, sparse=None):
        if not (path / ".git").exists():
            if path.exists() and any(path.iterdir()):
                raise SystemExit(f"Source directory already contains files: {path}")

            path.mkdir(parents=True, exist_ok=True)
            self.run(name + "-init", ["git", "init", "-q", path])
            self.run(name + "-remote", ["git", "-C", path, "remote", "add", "origin", repository])

        current = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"],
                                 capture_output=True, text=True)
        if current.returncode == 0:
            if current.stdout.strip() != revision:
                raise SystemExit(f"Existing {name} checkout uses another revision: {path}")

            return

        self.run(name + "-fetch", ["git", "-C", path, "fetch", "--depth=1",
                                    "--filter=blob:none", "origin", revision])
        if sparse:
            self.run(name + "-sparse-init", ["git", "-C", path, "sparse-checkout", "init", "--cone"])
            self.run(name + "-sparse-set", ["git", "-C", path, "sparse-checkout", "set", *sparse])

        self.run(name + "-checkout", ["git", "-C", path, "checkout", "--detach", revision])

    def mesa(self):
        source = self.cache / "mesa-work"
        mesa = self.pins["mesa"]
        self.checkout("mesa", source, mesa["mirror"], mesa["commit"], mesa["sparse_directories"])
        changes = self.read(["git", "-C", source, "status", "--porcelain"])
        if changes:
            sys.path.insert(0, str(self.core / "scripts"))
            from mesa_patch_identity import validate_recorded_patches
            # An existing target can contain an earlier complete prefix of the
            # selected patches. Confirm that source before applying the suffix;
            # any edits outside those recorded states remain a build error.
            patches = self.pins["patches"]
            failure = None
            for applied in range(len(patches), -1, -1):
                recorded = dict(self.pins, patches=patches[:applied])
                try:
                    validate_recorded_patches(self.core, source, recorded)
                    break
                except SystemExit as error:
                    if failure is None:
                        failure = error
            else:
                raise failure

            for index in range(applied, len(patches)):
                self.run(f"mesa-patch-{index + 1:02}",
                         ["git", "-C", source, "apply", self.core / patches[index]])
        else:
            for index, patch in enumerate(self.pins["patches"]):
                self.run(f"mesa-patch-{index + 1:02}", ["git", "-C", source, "apply", self.core / patch])

    def python(self):
        python = self.venv / "bin/python"
        if not python.is_file():
            self.run("python-venv", [sys.executable, "-m", "venv", self.venv])

        self.run("python-packages", [python, "-m", "pip", "--disable-pip-version-check", "install",
                                      "-r", self.core / "scripts/requirements-adreno-compiler.txt"])

    @staticmethod
    def matching_bison(path):
        if not path or not Path(path).is_file():
            return False

        result = subprocess.run([str(path), "--version"], capture_output=True, text=True)
        return result.returncode == 0 and bool(re.match(
            r"^bison \(GNU Bison\) " + re.escape(BISON_VERSION) + r"(?:\s|$)", result.stdout))

    def bison(self):
        installed = self.tools / "bin/bison"
        if self.matching_bison(installed):
            return

        candidates = [shutil.which("bison"),
                      "/opt/homebrew/opt/bison/bin/bison",
                      "/usr/local/opt/bison/bin/bison"]
        for candidate in candidates:
            if self.matching_bison(candidate):
                if installed.exists() or installed.is_symlink():
                    installed.unlink()

                installed.symlink_to(Path(candidate).resolve())
                return

        pin = self.pins["bison"]
        if pin["version"] != BISON_VERSION:
            raise SystemExit("The core bison pin differs from this bootstrap's supported version")

        archives = self.cache / "turnip-source-archives"
        archives.mkdir(exist_ok=True)
        archive = archives / f"bison-{BISON_VERSION}.tar.xz"
        if not archive.is_file():
            partial = archives / (archive.name + ".part")
            print(f"Turnip bootstrap: download {pin['url']}", flush=True)
            request = urllib.request.Request(pin["url"], headers={"User-Agent": "UniXNU-Turnip-bootstrap"})
            with urllib.request.urlopen(request, timeout=120) as response, partial.open("wb") as output:
                shutil.copyfileobj(response, output)
            partial.replace(archive)

        source = self.cache / f"bison-{BISON_VERSION}"
        if not (source / "configure").is_file():
            self.run("bison-extract", ["tar", "-xf", archive, "-C", self.cache])

        build = self.cache / "bison-build"
        build.mkdir(exist_ok=True)
        self.run("bison-configure", [source / "configure", f"--prefix={self.tools}", "--disable-nls"], cwd=build)
        self.run("bison-build", ["make", f"-j{self.jobs}"], cwd=build)
        self.run("bison-install", ["make", "install"], cwd=build)

    def glslang(self):
        installed = self.tools / "bin/glslangValidator"
        if installed.is_file():
            version = self.read([installed, "--version"])
            if re.search(r"Glslang Version: \S*:" + re.escape(GLSLANG_VERSION) + r"(?:\s|$)", version):
                return

        source = self.cache / "glslang-source"
        self.checkout("glslang", source, GLSLANG_REPOSITORY, GLSLANG_REVISION)
        build = self.cache / "glslang-build"
        self.run("glslang-configure", [
            "cmake", "-S", source, "-B", build, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={self.tools}",
            "-DENABLE_OPT=OFF", "-DGLSLANG_TESTS=OFF", "-DENABLE_SPVREMAPPER=OFF",
            "-DENABLE_GLSLANG_BINARIES=ON",
        ])
        self.run("glslang-build", ["cmake", "--build", build, "--target", "install", f"-j{self.jobs}"])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--core", type=Path, required=True)
    parser.add_argument("--reference-core", type=Path)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--abi-directory", type=Path)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.build and args.abi_directory is None:
        parser.error("--build requires --abi-directory")

    missing = [tool for tool in ("git", "cmake", "ninja", "make", "tar", "xcrun")
               if shutil.which(tool) is None]
    if missing:
        raise SystemExit("Required host build tools: " + ", ".join(missing))

    core = args.core.expanduser().resolve()
    reference = args.reference_core.expanduser().resolve() if args.reference_core else None
    prepare_cache(core, reference)
    bootstrap = Bootstrap(core, args.jobs)
    bootstrap.mesa()
    bootstrap.python()
    bootstrap.bison()
    bootstrap.glslang()
    (bootstrap.logs / "inputs.json").write_text(json.dumps({
        "mesa_revision": bootstrap.pins["mesa"]["commit"],
        "mesa_patches": bootstrap.pins["patches"],
        "python_requirements": str(bootstrap.core / "scripts/requirements-adreno-compiler.txt"),
        "glslang_version": GLSLANG_VERSION,
        "glslang_revision": GLSLANG_REVISION,
        "bison_version": BISON_VERSION,
        "record_hashes": False,
    }, indent=2) + "\n")

    if args.build:
        bootstrap.run("turnip-ios", [bootstrap.venv / "bin/python",
                                     bootstrap.core / "scripts/build-turnip-darwin.py",
                                     "--target", "ios", "--abi-directory", args.abi_directory.resolve()])

    print(f"Turnip build inputs ready: {bootstrap.core}; logs: {bootstrap.logs}")


if __name__ == "__main__":
    main()
