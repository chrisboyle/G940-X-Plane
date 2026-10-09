"""Create an installable ZIP containing both plugins for one platform."""
import argparse
from pathlib import Path
import sys
from zipfile import ZIP_DEFLATED, ZipFile

from tools.install import FILENAMES, PLUGINS


def package(build_dir, platform, output):
    filename = FILENAMES[platform]
    sources = [Path(build_dir) / plugin / "64" / filename for plugin in PLUGINS]
    for source in sources:
        if not source.is_file():
            raise ValueError(f"Plugin has not been built: {source}")
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(output, "w", compression=ZIP_DEFLATED) as archive:
        for plugin, source in zip(PLUGINS, sources):
            archive.write(source, f"Resources/plugins/{plugin}/64/{filename}")
        archive.write(Path(__file__).resolve().parents[1] / "LICENSE", "LICENSE")
        archive.writestr("INSTALL.txt", "Close X-Plane and back up existing G940 plugins.\n"
                          "Copy this archive's Resources folder into your X-Plane 11/12 folder,\n"
                          "merging folders and replacing only the matching .xpl files.\n"
                          "Start X-Plane and check Log.txt for g940FF and g940LEDs.\n"
                          "Linux requires writable evdev devices and a G940 LED sysfs driver.\n"
                          "See https://github.com/chrisboyle/G940-X-Plane for documentation.\n")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--platform", choices=FILENAMES, required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    try:
        print(package(args.build_dir, args.platform, args.output))
    except (OSError, ValueError) as error:
        print(f"Packaging failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
