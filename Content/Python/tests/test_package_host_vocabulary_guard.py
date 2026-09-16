# Copyright (c) 2026 Alexander Penkin. MIT License.

import pathlib
import subprocess
import tempfile
import unittest


class PackageHostVocabularyGuardTests(unittest.TestCase):
    def test_exact_previously_excluded_file_classes_are_rejected(self):
        plugin_root = pathlib.Path(__file__).resolve().parents[3]
        script = plugin_root / "scripts" / "package-fab.ps1"

        # The host-name rules live in scripts/package-fab.local.ps1, which is untracked on
        # purpose: naming the private host project's proper nouns in this public repo is the
        # leak the guard exists to stop. Without that file the scanner has no host rules, so
        # these fixtures are not blocked text and there is nothing to assert here.
        if not (plugin_root / "scripts" / "package-fab.local.ps1").exists():
            self.skipTest(
                "scripts/package-fab.local.ps1 is absent, so no host-vocabulary rules are"
                " defined; create it to exercise this guard"
            )

        project_theme = "Do" + "ta2"
        first_faction = "Radi" + "ant"
        second_faction = "Di" + "re"
        unit_plural = "Cre" + "eps"

        fixtures = {
            "docs/engine-research-2026-08-plugin-pass.md":
                f"green {first_faction} half, grey {second_faction} half\n",
            "docs/pwmodel-design.md": f"generated under {project_theme}/FX\n",
            "docs/pwmodel-format.md": f"a 21-bone {unit_plural[:-1].lower()}\n",
            "docs/floating-geometry-audit.md":
                f"do not depend on {project_theme} host content\n",
            "Source/PinWright/Private/Tests/Render/TestAnimationCaptureHandlers.cpp":
                f'TEXT("/Game/{project_theme}/{unit_plural}/Mesh")\n',
        }

        with tempfile.TemporaryDirectory(prefix="pinwright-vocabulary-guard-") as temp:
            root = pathlib.Path(temp)
            for relative, text in fixtures.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")

            completed = subprocess.run(
                [
                    "powershell.exe",
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(script),
                    "-HostVocabularyProbeRoot",
                    str(root),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

        output = completed.stdout + completed.stderr
        self.assertNotEqual(completed.returncode, 0, output)
        self.assertIn("host vocabulary", output.lower())
        for relative in fixtures:
            self.assertIn(relative.replace("/", "\\"), output)


if __name__ == "__main__":
    unittest.main()
