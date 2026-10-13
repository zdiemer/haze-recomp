"""Stage only release inputs, generate a per-user MSI, and hash deliverables."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import uuid
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TOOLING = ROOT
WIX = "http://wixtoolset.org/schemas/v4/wxs"
UPGRADE_CODE = "6F7B431E-615D-4B96-AD16-8CE45E3B467B"


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def inventory(root: Path) -> dict[str, str]:
    result = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
            raise ValueError(f"Links are not package inputs: {path}")
        if path.is_file():
            result[path.relative_to(root).as_posix()] = digest(path)
    return result


def version() -> str:
    value = (ROOT / "VERSION").read_text().strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", value):
        raise ValueError("VERSION must be major.minor.patch (MSI uses three numeric fields)")
    if any(int(n) > limit for n, limit in zip(value.split('.'), (255, 255, 65535))):
        raise ValueError("VERSION exceeds Windows Installer limits")
    return value


def wix_source(stage: Path, output: Path, release_version: str) -> None:
    ET.register_namespace("", WIX)
    def node(parent, kind, **attrs):
        return ET.SubElement(parent, f"{{{WIX}}}{kind}", attrs)
    tree = ET.Element(f"{{{WIX}}}Wix")
    package = node(tree, "Package", Name="Haze Recomp", Manufacturer="Haze Recomp contributors",
                   Version=release_version, UpgradeCode=UPGRADE_CODE, Scope="perUser",
                   InstallerVersion="500", Compressed="yes")
    node(package, "MajorUpgrade", DowngradeErrorMessage="A newer Haze Recomp is already installed.")
    node(package, "MediaTemplate", EmbedCab="yes")
    local = node(package, "StandardDirectory", Id="LocalAppDataFolder")
    programs = node(local, "Directory", Id="HazePrograms", Name="Programs")
    install = node(programs, "Directory", Id="INSTALLFOLDER", Name="Haze Recomp")
    feature = node(package, "Feature", Id="Main", Title="Haze Recomp", Level="1")
    directories = {".": install}
    for relative in inventory(stage):
        # MSI owns defaults; the bootstrapper creates user settings only when
        # absent. Permanent MSI components retain machine-wide location state
        # and can skip settings after uninstall/reinstall to another directory.
        installed_relative = 'defaults/' + relative if relative.startswith('config/') else relative
        path = Path(installed_relative)
        parent = Path()
        for part in path.parts[:-1]:
            previous = parent.as_posix()
            parent /= part
            key = parent.as_posix()
            if key not in directories:
                ident = "D" + hashlib.sha256(key.encode()).hexdigest()[:24]
                directories[key] = node(directories[previous], "Directory", Id=ident, Name=part)
        ident = hashlib.sha256(installed_relative.encode()).hexdigest()[:24]
        attrs = {"Id": "C" + ident, "Guid": str(uuid.uuid5(uuid.UUID(UPGRADE_CODE), installed_relative))}
        component = node(directories[parent.as_posix()], "Component", **attrs)
        node(component, "File", Id="F" + ident, Source=str((stage / relative).resolve()), Name=path.name)
        node(component, "RegistryValue", Root="HKCU", Key="Software\\HazeRecomp\\Components",
             Name=ident, Value=release_version, Type="string", KeyPath="yes")
        node(feature, "ComponentRef", Id="C" + ident)
    menu = node(package, "StandardDirectory", Id="ProgramMenuFolder")
    component = node(menu, "Component", Id="StartMenu", Guid="*")
    node(component, "Shortcut", Id="Launch", Name="Haze Recomp", Target="[INSTALLFOLDER]HazeLauncher.exe", WorkingDirectory="INSTALLFOLDER")
    node(component, "RegistryValue", Root="HKCU", Key="Software\\HazeRecomp", Name="Installed", Value="1", Type="integer", KeyPath="yes")
    node(feature, "ComponentRef", Id="StartMenu")
    ET.indent(tree)
    ET.ElementTree(tree).write(output, encoding="utf-8", xml_declaration=True)


def stage_package(runtime: Path, tools: Path, licenses: Path, notes: Path, output: Path) -> Path:
    release_version = version()
    if output.exists():
        raise ValueError("Use a new output directory; existing releases are immutable")
    runtime_files = inventory(runtime)
    for name in ("HazeRecomp.exe", "HazeLauncher.exe", "build-info.json"):
        if name not in runtime_files:
            raise ValueError(f"Missing runtime input: {name}")
    if any('/' in p or (p not in ("HazeRecomp.exe", "HazeLauncher.exe", "build-info.json") and not p.lower().endswith('.dll')) for p in runtime_files):
        raise ValueError("Runtime staging contains unexpected files")
    provenance = json.loads((runtime / "build-info.json").read_text())
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    if provenance.get("commit") != commit or provenance.get("version") != release_version:
        raise ValueError("Runtime provenance does not match this checkout/version")
    if provenance.get("files") != {k: v for k, v in runtime_files.items() if k != "build-info.json"}:
        raise ValueError("Runtime inputs changed since dependency staging")
    lock = json.loads((ROOT / "config/release-tools.json").read_text())
    recorded = json.loads((tools / "files.json").read_text())
    for name in ('7zip',):
        if inventory(tools / name) != recorded[name]:
            raise ValueError(f"Bundled {name} is modified; prepare a pristine tools directory")
    if json.loads((tools / "release-tools.json").read_text()) != lock:
        raise ValueError("Bundled tools do not match the release lock")
    # Explicitly supplied redistribution notices and corresponding source offer
    # are a release input; never infer licenses from a developer's install tree.
    for name in ("THIRD-PARTY-NOTICES.txt", "SOURCE-OFFER.txt"):
        if not (licenses / name).is_file() or (licenses / name).stat().st_size == 0:
            raise ValueError(f"Missing redistribution input: {name}")
    inventory(licenses)
    if not notes.read_text(encoding="utf-8").strip():
        raise ValueError("Release notes are required")
    stage = output / f"HazeRecomp-{release_version}-windows-x64"
    stage.mkdir(parents=True)
    for name in runtime_files:
        shutil.copy2(runtime / name, stage / name)
    shutil.copytree(licenses, stage / "licenses")
    shutil.copytree(ROOT / "assets/input-prompts", stage / "assets/input-prompts")
    (stage / "config").mkdir()
    for name in ("input.ini", "graphics.ini"):
        shutil.copy2(ROOT / "config" / name, stage / "config" / name)
    for name in ("LICENSE", "VERSION"):
        shutil.copy2(ROOT / name, stage / name)
    shutil.copy2(notes, stage / "RELEASE-NOTES.txt")
    shutil.copy2(TOOLING / "docs/installing.md", stage / "INSTALLING.txt")
    (stage / "manifest.json").write_text(json.dumps({"version": release_version, "commit": commit,
        "files": inventory(stage)}, indent=2) + "\n", encoding="utf-8")
    return stage


def verify(stage: Path) -> None:
    manifest = json.loads((stage / "manifest.json").read_text())
    actual = inventory(stage)
    del actual["manifest.json"]
    if actual != manifest["files"]:
        raise ValueError("Package contents do not match the manifest")


def archive(stage: Path, output: Path) -> None:
    verify(stage)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as stream:
        for name in inventory(stage):
            info = zipfile.ZipInfo(f"{stage.name}/{name}", date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            stream.writestr(info, (stage / name).read_bytes())


def main() -> None:
    global ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", type=Path)
    parser.add_argument("--project-root", type=Path, default=ROOT)
    parser.add_argument("--runtime", type=Path)
    parser.add_argument("--tools", type=Path)
    parser.add_argument("--licenses", type=Path)
    parser.add_argument("--notes", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--wix", type=Path)
    args = parser.parse_args()
    ROOT = args.project_root.resolve()
    if args.verify:
        verify(args.verify)
        return
    if not all((args.runtime, args.tools, args.licenses, args.notes, args.output, args.wix)):
        parser.error("Packaging requires --runtime, --tools, --licenses, --notes, --output and --wix")
    stage = stage_package(args.runtime, args.tools, args.licenses, args.notes, args.output)
    wxs = args.output / "installer.wxs"
    wix_source(stage, wxs, version())
    msi = args.output / f"{stage.name}.msi"
    subprocess.run([str(args.wix.resolve()), "build", "-arch", "x64", str(wxs), "-o", str(msi)], check=True)
    zipped = args.output / f"{stage.name}.zip"
    archive(stage, zipped)
    from installer import build_installer
    setup = build_installer(msi, args.tools, args.output, version())
    (args.output / "SHA256SUMS.txt").write_text("".join(f"{digest(p)}  {p.name}\n" for p in (setup, msi, zipped)), encoding="ascii")
    print(f"Release candidate: {args.output}. Complete the acceptance gates before publishing.")


if __name__ == "__main__":
    main()
