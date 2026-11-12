import importlib.util
import io
from pathlib import Path
import tarfile
import unittest
import sys

sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location("vendor_novnc", Path(__file__).with_name("vendor-novnc.py"))
vendor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vendor)


def archive(entries):
    target = io.BytesIO()
    with tarfile.open(fileobj=target, mode="w:gz") as output:
        for name, kind in entries:
            info = tarfile.TarInfo(name)
            info.type = kind
            if kind == tarfile.REGTYPE:
                info.size = 1
                output.addfile(info, io.BytesIO(b"x"))
            else:
                info.linkname = "outside"
                output.addfile(info)
    return target.getvalue()


class VendorSafety(unittest.TestCase):
    def test_paths(self):
        for value in ["../bad", "/bad", "a//b", "a/./b", "a/../b", "C:/bad", "a\\b", ""]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                vendor.safe_relative(value)
        self.assertEqual(vendor.safe_relative("core/rfb.js"), "core/rfb.js")

    def test_no_links_devices_or_traversal(self):
        for kind in [tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.CHRTYPE, tarfile.FIFOTYPE]:
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                vendor.unpack(archive([("package/core/x.js", kind)]), "package")
        for name in ["package/../escape", "other/core/x", "/package/x", "package/a//x"]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                vendor.unpack(archive([(name, tarfile.REGTYPE)]), "package")

    def test_no_duplicate_paths(self):
        with self.assertRaises(ValueError):
            vendor.unpack(archive([("package/x", tarfile.REGTYPE), ("package/x", tarfile.REGTYPE)]), "package")

    def test_regular_file_bytes_only(self):
        self.assertEqual(vendor.unpack(archive([("package/core/x.js", tarfile.REGTYPE)]), "package"), {"core/x.js": b"x"})

    def test_destination_manifest_content(self):
        pin = __import__("json").loads(vendor.PIN_PATH.read_text(encoding="utf-8"))
        manifest = __import__("json").loads((vendor.ASSETS / "vendor/novnc/manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["gitCommit"], pin["gitCommit"])
        self.assertEqual(len(vendor.verify_rows(vendor.ASSETS / "vendor/novnc", manifest["files"])), 64)


if __name__ == "__main__":
    unittest.main()
