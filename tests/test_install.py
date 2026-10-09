import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("installer", Path(__file__).resolve().parents[1] / "tools/install.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.home = Path(self.temporary.name).resolve()

    def simulator(self, name):
        path = self.home / name
        (path / "Resources/plugins").mkdir(parents=True)
        return path

    def hint(self, version, contents):
        path = self.home / f"Library/Preferences/x-plane_install_{version}.txt"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents)
        return path

    def test_multiline_and_spaces(self):
        sim = self.simulator("X-Plane 12")
        self.hint(12, f"/missing/location\n{sim}\n\n{sim}/\n")
        self.assertEqual(installer.find_installation("mac", home=self.home), sim)

    def test_xplane_11_fallback(self):
        sim = self.simulator("X-Plane 11")
        self.hint(11, f"{sim}\n")
        self.assertEqual(installer.find_installation("mac", home=self.home), sim)

    def test_multiple_installations_require_explicit_path(self):
        first, second = self.simulator("one"), self.simulator("two")
        self.hint(12, f"{first}\n{second}\n")
        with self.assertRaisesRegex(ValueError, "Multiple"):
            installer.find_installation("mac", home=self.home)
        self.assertEqual(installer.find_installation("mac", explicit=second), second)

    def test_missing_hint_and_invalid_path_fail(self):
        with self.assertRaises(ValueError):
            installer.find_installation("mac", home=self.home)
        with self.assertRaises(ValueError):
            installer.find_installation("mac", explicit=self.home)

    def test_installs_only_selected_platform_and_backs_up(self):
        sim = self.simulator("X-Plane 12")
        build = self.home / "build"
        for plugin in installer.PLUGINS:
            folder = build / plugin / "64"
            folder.mkdir(parents=True)
            (folder / "mac.xpl").write_bytes(b"new mac plugin")
            target = sim / "Resources/plugins" / plugin / "64"
            target.mkdir(parents=True)
            (target / "mac.xpl").write_bytes(b"old mac plugin")
            (target / "lin.xpl").write_bytes(b"linux plugin")
        installer.install(build, "mac", sim)
        for plugin in installer.PLUGINS:
            target = sim / "Resources/plugins" / plugin / "64"
            self.assertEqual((target / "mac.xpl").read_bytes(), b"new mac plugin")
            self.assertEqual((target / "lin.xpl").read_bytes(), b"linux plugin")
            backups = list(target.glob("mac.xpl.backup-*"))
            self.assertEqual(len(backups), 1)
            self.assertEqual(backups[0].read_bytes(), b"old mac plugin")

    def test_missing_build_fails_before_installing(self):
        sim = self.simulator("X-Plane 12")
        with self.assertRaises(ValueError):
            installer.install(self.home / "missing", "mac", sim)
        self.assertFalse((sim / "Resources/plugins/g940FF").exists())
