"""Static checks for the minimal ESP-IDF migration.

ESP-IDF is not installed in this test environment, so these tests verify the
build graph and partition contract without attempting a cross-compilation.
"""

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
IDF = ROOT / "idf"
BUILD = ROOT / "scripts" / "build_idf.ps1"
BUILD_LINUX = ROOT / "scripts" / "build_idf.sh"
WORKSPACE = ROOT / "esp32-ai-idf.code-workspace"


class IdfProjectContract(unittest.TestCase):
    def test_has_standard_project_entry_point(self):
        text = (IDF / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("project(esp32_tinystories)", text)

    def test_model_partition_fits_the_published_binary(self):
        text = (IDF / "partitions.csv").read_text(encoding="utf-8")
        self.assertIn("model,     data, 0x40,    0x110000,  0xEE0000", text)

    def test_sdkconfig_selects_the_custom_partition_table(self):
        text = (IDF / "sdkconfig.defaults").read_text(encoding="utf-8")
        self.assertIn("CONFIG_PARTITION_TABLE_CUSTOM=y", text)
        self.assertIn('CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"', text)

    def test_build_generates_headers_and_flashes_model(self):
        main = (IDF / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
        project = (IDF / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("if(NOT CMAKE_SCRIPT_MODE_FILE)", main)
        self.assertIn("generate_vocab.py", main)
        self.assertIn("generate_tokenizer_header.py", main)
        self.assertIn("--allow-in-vocab-special-added-tokens", main)
        self.assertIn('esptool_py_flash_to_partition(flash "model"', project)

    def test_entrypoint_uses_usb_serial_jtag_not_arduino(self):
        text = (IDF / "main" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("usb_serial_jtag_driver_install", text)
        self.assertIn('extern "C" void app_main(void)', text)
        self.assertNotIn("#include <Arduino.h>", text)

    def test_build_script_makes_flashing_explicit(self):
        text = BUILD.read_text(encoding="utf-8")
        self.assertIn("[switch]$Flash", text)
        self.assertIn("[switch]$Clean", text)
        self.assertIn("& idf.py fullclean", text)
        self.assertIn("if (-not (Get-Command idf.py -ErrorAction SilentlyContinue))", text)
        self.assertIn("-Flash requires -Port COMx", text)
        self.assertIn("& idf.py build", text)
        self.assertIn("& idf.py -p $Port flash", text)

    def test_linux_build_script_makes_flashing_explicit(self):
        text = BUILD_LINUX.read_text(encoding="utf-8")
        self.assertIn("FLASH=0", text)
        self.assertIn("CLEAN=0", text)
        self.assertIn("idf.py fullclean", text)
        self.assertIn("idf.py build", text)
        self.assertIn('idf.py -p "$PORT" flash', text)
        self.assertIn("--flash requires --port", text)

    def test_workspace_exposes_idf_as_a_project_folder(self):
        text = WORKSPACE.read_text(encoding="utf-8")
        self.assertIn('"path": "idf"', text)
        self.assertIn('"IDF_TARGET": "esp32s3"', text)


if __name__ == "__main__":
    unittest.main()
