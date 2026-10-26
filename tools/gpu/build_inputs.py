"""GPU build inputs belong to the resolved target tree.

Independent source checkouts receive the target's selected patches. Existing
host compilers and package downloads can be shared with a reference checkout;
the target keeps its own modified sources, build directories and artifacts.
"""

from pathlib import Path
import shutil
import subprocess


def prepare_cache(core, reference):
    if reference is None or core.resolve() == reference.resolve():
        return

    cache = core / ".cache"
    seed = reference / ".cache"
    cache.mkdir(parents=True, exist_ok=True)

    for name in ("mesa-tools", "mesa-venv", "metal-rustup", "metal-cargo"):
        source = seed / name
        target = cache / name
        if source.is_dir() and not target.exists():
            target.symlink_to(source.resolve(), target_is_directory=True)

    for name in ("mesa-work", "glslang-source", "gpu-reference/metal2vulkan"):
        source = seed / name
        target = cache / name
        if not (source / ".git").is_dir() or target.exists():
            continue

        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source / ".git", target / ".git",
                        ignore=shutil.ignore_patterns("*.lock"))
        subprocess.run(["git", "-C", str(target), "config", "core.worktree", str(target)],
                       check=True)
        subprocess.run(["git", "-C", str(target), "reset", "--quiet", "--hard", "HEAD"],
                       check=True)


def prepare_metal_libraries(core, system_root):
    """The shader input directory carries original OS libraries and their paths.

    The plugin embeds the complete list before signing. The addon packager
    installs a QuartzCore copy beside the plugin so priming can open it without
    enumerating framework directories inside the render server's sandbox.
    """
    output = core / "build/metal-inputs"
    output.mkdir(parents=True, exist_ok=True)
    libraries = sorted(path for path in system_root.rglob("*.metallib") if path.is_file())
    if not libraries:
        raise SystemExit(f"No Metal libraries in the selected OS image: {system_root}")

    quartzcore = Path("System/Library/Frameworks/QuartzCore.framework/default.metallib")
    source = system_root / quartzcore
    if not source.is_file():
        raise SystemExit(f"Missing original QuartzCore Metal library: {source}")

    destination = output / "shaders" / quartzcore
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)

    paths = ["/" + path.relative_to(system_root).as_posix() for path in libraries]
    paths.append("/private/var/ngpu/addons/shaders/" + quartzcore.as_posix())
    contents = "\n".join(sorted(set(paths))) + "\n"
    (output / "NativeAdrenoMetal.primelist").write_text(contents)
    (core / "build/prime-list.txt").write_text(contents)
    print(f"Metal priming inputs: {len(libraries)} OS libraries and QuartzCore copy in {output}",
          flush=True)
