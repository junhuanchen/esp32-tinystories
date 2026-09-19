"""Static safety checks for the Windows ESP32-S3 deployment entry point.

The script intentionally is not executed here: doing so requires model assets
and would write to a board. These assertions preserve the deployment contract
that compilation occurs before either flash command.
"""

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "deploy.ps1"
FETCH = ROOT / "scripts" / "fetch_model.ps1"


class WindowsDeployScript(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = SCRIPT.read_text(encoding="utf-8")

    def test_uses_the_n16r8_memory_configuration(self):
        self.assertIn("FlashSize=16M", self.text)
        self.assertIn("PSRAM=opi", self.text)
        self.assertIn("'0x110000'", self.text)

    def test_requires_an_explicit_windows_port(self):
        self.assertIn("[Parameter(Mandatory)]\n  [string]$Port", self.text)

    def test_compile_precedes_both_device_writes(self):
        compile_at = self.text.index("Invoke-Step \"compile $Sketch\"")
        model_at = self.text.index("Invoke-Step 'flash model'")
        firmware_at = self.text.index("Invoke-Step 'upload firmware'")
        self.assertLess(compile_at, model_at)
        self.assertLess(model_at, firmware_at)

    def test_tinystories_generates_its_matching_decode_header(self):
        self.assertIn("firmware/esp32_tinystories", self.text)
        self.assertIn("generate_vocab.py", self.text)
        self.assertIn("generate_tokenizer_header.py", self.text)


class WindowsFetchScript(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = FETCH.read_text(encoding="utf-8")

    def test_tinystories_assets_are_pinned(self):
        self.assertIn("slvDev/esp32-ai-tinystories", self.text)
        self.assertIn("1d8326c05c383ccfa615f5455575802817cb453dbc7ab28875d41a9dbb45477e",
                      self.text)
        self.assertIn("14912348", self.text)

    def test_verification_precedes_install(self):
        self.assertLess(self.text.index("Get-FileHash"),
                        self.text.index("[IO.Directory]::Move($Incoming, $Destination)"))
        self.assertIn("metadata.json disagrees", self.text)

    def test_download_url_delimits_the_filename_variable(self):
        self.assertIn("/${Name}?download=true", self.text)


if __name__ == "__main__":
    unittest.main()
