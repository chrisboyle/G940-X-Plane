from pathlib import Path
import tempfile
import unittest
from zipfile import ZipFile

from tools.install import FILENAMES, PLUGINS
from tools.package import package


class PackageTests(unittest.TestCase):
    def test_each_platform_extracts_into_xplane(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for plugin in PLUGINS:
                folder = root / "build" / plugin / "64"
                folder.mkdir(parents=True)
                for filename in FILENAMES.values():
                    (folder / filename).write_bytes(filename.encode())
            for platform, filename in FILENAMES.items():
                with self.subTest(platform=platform):
                    output = package(root / "build", platform, root / "dist" / f"{platform}.zip")
                    with ZipFile(output) as archive:
                        expected = {f"Resources/plugins/{plugin}/64/{filename}" for plugin in PLUGINS}
                        self.assertEqual(set(archive.namelist()), expected | {"LICENSE", "INSTALL.txt"})
                        self.assertIsNone(archive.testzip())
                        destination = root / "X-Plane 12" / platform
                        archive.extractall(destination)
                        for path in expected:
                            self.assertEqual((destination / path).read_bytes(), filename.encode())

    def test_incomplete_build_leaves_existing_archive_intact(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "build/g940FF/64"
            source.mkdir(parents=True)
            (source / "mac.xpl").write_bytes(b"only one plugin")
            output = root / "existing.zip"
            output.write_bytes(b"previous archive")
            with self.assertRaisesRegex(ValueError, "g940LEDs"):
                package(root / "build", "mac", output)
            self.assertEqual(output.read_bytes(), b"previous archive")
