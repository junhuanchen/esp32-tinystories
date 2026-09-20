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
        self.assertIn("tokenizer.active_vocab > (uint32_t)c->vocab", self.sketch)
        self.assertIn("tokenizer.active_vocab != (uint32_t)model.out_vocab", self.sketch)
        self.assertIn("VOCAB_N != model.out_vocab", self.sketch)

    def test_serial_prompt_is_encoded_before_generation(self):
        self.assertIn("static int read_prompt", self.sketch)
        self.assertIn("static bool skip_lf_after_cr = false", self.sketch)
        self.assertIn("if (c == '\\r')", self.sketch)
        self.assertIn("if (c == '\\n')", self.sketch)
        self.assertIn("bpe_encode_ascii(&tokenizer, prompt, prompt_ids", self.sketch)
        self.assertIn("generate(prompt_ids, n_prompt)", self.sketch)

    def test_generation_uses_an_english_sentence_boundary_near_its_limit(self):
        self.assertIn("static const int ENDING_WINDOW = 32", self.sketch)
        self.assertIn("static bool token_ends_sentence", self.sketch)
        self.assertIn("last == '.' || last == '!' || last == '?'", self.sketch)
        self.assertIn("decoded >= ending_from && token_ends_sentence(tok)", self.sketch)

    def test_generation_uses_reproducible_top_k_sampling(self):
        self.assertIn("static const bool USE_TOP_K_SAMPLING = true", self.sketch)
        self.assertIn("static const int SAMPLE_TOP_K = 16", self.sketch)
        self.assertIn("static const float SAMPLE_TEMPERATURE = 0.65f", self.sketch)
        self.assertIn("static const int NGRAM_WINDOW = 32", self.sketch)
        self.assertIn("static uint32_t sample_rng_state", self.sketch)
        self.assertIn("static int recent_trigram_blocks", self.sketch)
        self.assertIn("static bool is_blocked_token", self.sketch)
        self.assertIn("static int select_next_token(const int *recent", self.sketch)
        self.assertIn("float peak = values[0]", self.sketch)
        self.assertIn("values[i] - peak", self.sketch)
        self.assertIn("tok = select_next_token(recent, n_recent)", self.sketch)

    def test_generation_stops_at_the_tokenizer_eot(self):
        generator = (ROOT / "firmware" / "esp32_tinystories" / "tools" /
                     "generate_vocab.py").read_text(encoding="utf-8")
        self.assertIn('tok.token_to_id("<|endoftext|>")', generator)
        self.assertIn('f"#define VOCAB_EOT {eot}\\n"', generator)
        self.assertIn("if (tok == VOCAB_EOT) break", self.sketch)
        self.assertIn("invalid EOT token id", self.sketch)

    def test_boot_reports_runtime_limits_and_parallelism(self):
        self.assertIn("S=%d", self.sketch)
        self.assertIn("esp_clk_cpu_freq()", self.sketch)
        self.assertIn("int dual_core_active = 0", self.sketch)
        self.assertIn("dual-core=%s", self.sketch)

    def test_deploy_generates_the_asset_from_the_selected_tokenizer(self):
        self.assertIn("generate tokenizer encoder", self.deploy)
        self.assertIn("generate_tokenizer_header.py", self.deploy)
        self.assertIn('"$TOKENIZER" --out "$SKETCH/generated/tokenizer_encoder.h"',
                      self.deploy)


if __name__ == "__main__":
    unittest.main()
