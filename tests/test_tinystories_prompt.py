"""Contract checks for TinyStories' USB-serial prompt path.

Arduino headers are unavailable to host Python tests, so these checks pin the
device-facing integration points that make a typed prompt use the tokenizer
shipped with the same deployed model.
"""

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT / "firmware" / "esp32_tinystories" / "esp32_tinystories.ino"
DEPLOY = ROOT / "scripts" / "deploy.sh"


class TinyStoriesPromptContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sketch = SKETCH.read_text(encoding="utf-8")
        cls.deploy = DEPLOY.read_text(encoding="utf-8")

    def test_uses_a_generated_encoder_asset(self):
        self.assertIn('#include "generated/tokenizer_encoder.h"', self.sketch)
        self.assertIn("bpe_tokenizer_load(TOKENIZER_ENCODER_ASSET", self.sketch)
        self.assertIn("tokenizer.active_vocab != (uint32_t)c->vocab", self.sketch)

    def test_serial_prompt_is_encoded_before_generation(self):
        self.assertIn("static int read_prompt", self.sketch)
        self.assertIn("bpe_encode_ascii(&tokenizer, prompt, prompt_ids", self.sketch)
        self.assertIn("generate(prompt_ids, n_prompt)", self.sketch)

    def test_deploy_generates_the_asset_from_the_selected_tokenizer(self):
        self.assertIn("generate tokenizer encoder", self.deploy)
        self.assertIn("generate_tokenizer_header.py", self.deploy)
        self.assertIn('"$TOKENIZER" --out "$SKETCH/generated/tokenizer_encoder.h"',
                      self.deploy)


if __name__ == "__main__":
    unittest.main()
