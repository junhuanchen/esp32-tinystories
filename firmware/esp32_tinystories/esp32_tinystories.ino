// PLE TinyLM inference on the ESP32-S3.
//
// The 28.9M-param model (14.9MB, 4-bit) lives in a flash 'model' partition,
// memory-mapped. Placement follows reads-per-token rather than what happens to
// fit:
//
//   flash   PLE table + token embedding   one row of the 25.2M-parameter
//                                         table per token
//   PSRAM   staged int8 core + head, KV   read once per position
//   SRAM    scratch + norm vectors        touched many times per token
//
// The logits array stays in PSRAM: 25,353 floats is 99 KiB, and the argmax
// reads it once per token.
//
// Same llm.h that is verified against PyTorch on the host; only the platform
// hooks differ here.

#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include <math.h>

// int8 activations, required by the staged int8 kernel. Not bit-exact against
// the fp32 golden; verify.c must be built without this flag. Validation CE cost
// (runtime/host_verify/ppl.c, 32,768 predictions): 2.4793 -> 2.4796, ppl 11.93 / 11.94.
#define LLM_INT8_ACT 1
#define LLM_PROFILE 1
#define LLM_PROFILE_NOW() esp_timer_get_time()
#include "../../runtime/llm.h"
#include "../../runtime/bpe_tokenizer.h"
#include "generated/vocab.h"
#include "generated/tokenizer_encoder.h"

// This board's built-in AMOLED uses a different driver; leave the optional
// external I2C OLED disabled and stream generation over USB serial.
#define USE_DISPLAY 0
#if USE_DISPLAY
#include "display.h"
#endif

// Leave a small tail of the context for a natural English sentence ending.
// This remains a hard upper limit: a prompt plus its continuation never exceeds
// the model's context window.
static const int N_GENERATE = 508;
static const int ENDING_WINDOW = 32;

// Greedy decoding is repeatable but easily falls into local repetition loops on
// a small model. Keep it available as a baseline, while the default samples
// only among the 16 most likely next tokens. The fixed seed makes comparisons
// between firmware builds reproducible after each boot.
static const bool USE_TOP_K_SAMPLING = true;
static const int SAMPLE_TOP_K = 16;
static const float SAMPLE_TEMPERATURE = 0.65f;
static uint32_t sample_rng_state = 0x6d2b79f5u;
static const int NGRAM_WINDOW = 32;

Model model;
Scratch s;
BpeTokenizer tokenizer;

// ---- allocation ------------------------------------------------------------
// Allocations are strict because memory placement is part of the runtime
// configuration. Allocation failure stops initialization.
static size_t psram_used = 0, sram_used = 0;

// The two LLM_Q8_MAX_INPUT int8 activation buffers (matvec_q8, matvec_par),
// which are static and so absent from the totals above. Together these report
// the managed hot set, not total SRAM usage; the free-SRAM figure printed at
// boot is the overall diagnostic.
#define STATIC_SRAM_BYTES (2 * LLM_Q8_MAX_INPUT)

static void *ps(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (p) psram_used += n;
  return p;
}
static void *ps_or_die(size_t n, const char *what) {
  void *p = ps(n);
  if (!p) {
    Serial.printf("FATAL: required PSRAM allocation failed: %s (%u bytes)\n",
                  what, (unsigned)n);
    while (1) delay(1000);
  }
  return p;
}
static void *sram_or_die(size_t n, const char *what) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) {
    Serial.printf("FATAL: required SRAM allocation failed: %s (%u bytes)\n",
                  what, (unsigned)n);
    while (1) delay(1000);
  }
  sram_used += n;
  return p;
}

// ---- dual-core int8 matvec -------------------------------------------------
// Serves both hooks. Below ~128 rows the task notify round trip costs more than
// the split saves, so small tensors run single-core.
static TaskHandle_t worker_h, main_h;
static const QT *job_t;
static const int8_t *job_xq;
static float job_xs;
static float *job_y;
static int job_split;

static void worker_main(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    matvec_i8_range(job_t, job_xq, job_xs, job_y, 0, job_split);
    xTaskNotifyGive(main_h);
  }
}

static void matvec_par(const QT *t, const float *x, float *y) {
  static int8_t xq[LLM_Q8_MAX_INPUT];
  float xs;
  if (t->w8 == NULL || t->rows < 128) { MATVEC(t, x, y); return; }
  quantize_act(x, t->cols, xq, &xs);   // once; both cores read the result
  job_t = t; job_xq = xq; job_xs = xs; job_y = y; job_split = t->rows / 2;
  xTaskNotifyGive(worker_h);
  matvec_i8_range(t, xq, xs, y, job_split, t->rows);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// Copy RMSNorm weights from mapped flash to internal SRAM.
static void copy_norms_to_sram() {
  Cfg *c = &model.c;
  int D = c->dim, L = c->n_layers, P = c->ple_dim;
  const float **vecs[3 * 32 + 2];
  int sizes[3 * 32 + 2], n_vec = 0;
  vecs[n_vec] = &model.ple_proj_norm; sizes[n_vec++] = P;
  for (int l = 0; l < L; l++) {
    vecs[n_vec] = &model.attn_norm[l]; sizes[n_vec++] = D;
    vecs[n_vec] = &model.ffn_norm[l];  sizes[n_vec++] = D;
    vecs[n_vec] = &model.ple_norm[l];  sizes[n_vec++] = D;
  }
  vecs[n_vec] = &model.out_norm; sizes[n_vec++] = D;
  for (int i = 0; i < n_vec; i++) {
    size_t bytes = (size_t)sizes[i] * sizeof(float);
    void *dst = sram_or_die(bytes, "norm vector");
    memcpy(dst, *vecs[i], bytes);
    *vecs[i] = (const float *)dst;
  }
  Serial.printf("norms  -> SRAM   %d vectors\n", n_vec);
}

static void alloc_scratch() {
  Cfg *c = &model.c;
  int D = c->dim, L = c->n_layers, P = c->ple_dim, F = c->ffn, S = c->seq_len;
  // hot working set -> internal SRAM
  s.x     = (float *)sram_or_die(D * 4, "x");
  s.h     = (float *)sram_or_die((F > D ? F : D) * 4, "h");
  s.qkv   = (float *)sram_or_die(3 * D * 4, "qkv");
  s.att   = (float *)sram_or_die(D * 4, "att");
  s.g1    = (float *)sram_or_die(F * 4, "g1");
  s.g2    = (float *)sram_or_die((P > F ? P : F) * 4, "g2");
  s.ple   = (float *)sram_or_die(L * P * 4, "ple");
  s.tmpP  = (float *)sram_or_die(L * P * 4, "tmpP");
  s.trow  = (float *)sram_or_die(L * P * 4, "trow");
  s.scores = (float *)sram_or_die(S * 4, "scores");
  // logits: out_vocab floats, 99 KiB here, read once per token. Left in PSRAM
  // rather than spend a fifth of internal SRAM on it.
  s.logits = (float *)ps_or_die((size_t)model.out_vocab * 4, "logits");
  // KV cache: 1.1MB, read once per position rather than per matvec.
  s.kcache = (float *)ps_or_die((size_t)L * S * D * 4, "kcache");
  s.vcache = (float *)ps_or_die((size_t)L * S * D * 4, "vcache");
}

static void blink(uint8_t g) {
#ifdef RGB_BUILTIN
  rgbLedWrite(RGB_BUILTIN, 0, g, g / 3);
#endif
}

// Emit one token to every active output (serial always; panel when enabled).
static void emit(int tok) {
  if (tok >= VOCAB_N) return;
  const unsigned char *bytes = VOCAB_BLOB + VOCAB_OFF[tok];
  int len = VOCAB_OFF[tok + 1] - VOCAB_OFF[tok];
  // Non-blocking: when no host is draining the USB-CDC buffer (running as a
  // standalone gadget on the display), skip the write instead of stalling the
  // whole generation once the TX buffer fills.
  if ((int)Serial.availableForWrite() >= len) Serial.write(bytes, len);
#if USE_DISPLAY
  display_puts(bytes, len);
#endif
}

static uint32_t sample_random_u32() {
  uint32_t x = sample_rng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  sample_rng_state = x;
  return x;
}

// Select from the top logits without sorting the full vocabulary. The output
// head already scans every class; this bounded insertion pass avoids a large
// allocation or a full sort in PSRAM.
// Reject only a candidate that recreates an exact recent token trigram. Unlike
// a token-wide penalty, frequent words remain available in new phrases.
static int recent_trigram_blocks(const int *recent, int n_recent, int *blocked) {
  if (n_recent < 3) return 0;
  int first = recent[n_recent - 2];
  int second = recent[n_recent - 1];
  int n_blocked = 0;
  for (int i = 0; i + 2 < n_recent; i++) {
    if (recent[i] != first || recent[i + 1] != second) continue;
    int tok = recent[i + 2];
    bool seen = false;
    for (int j = 0; j < n_blocked; j++)
      if (blocked[j] == tok) { seen = true; break; }
    if (!seen) blocked[n_blocked++] = tok;
  }
  return n_blocked;
}

static bool is_blocked_token(int tok, const int *blocked, int n_blocked) {
  for (int i = 0; i < n_blocked; i++)
    if (blocked[i] == tok) return true;
  return false;
}

static void remember_token(int tok, int *recent, int *n_recent) {
  if (*n_recent < NGRAM_WINDOW) {
    recent[(*n_recent)++] = tok;
    return;
  }
  memmove(recent, recent + 1, (NGRAM_WINDOW - 1) * sizeof(recent[0]));
  recent[NGRAM_WINDOW - 1] = tok;
}

static int select_next_token(const int *recent, int n_recent) {
  int blocked[NGRAM_WINDOW];
  int n_blocked = recent_trigram_blocks(recent, n_recent, blocked);
  if (!USE_TOP_K_SAMPLING) {
    int best = -1;
    for (int v = 0; v < model.out_vocab; v++)
      if (!is_blocked_token(v, blocked, n_blocked) &&
          (best < 0 || s.logits[v] > s.logits[best]))
        best = v;
    return best >= 0 ? best : 0;
  }

  float values[SAMPLE_TOP_K];
  int ids[SAMPLE_TOP_K];
  for (int i = 0; i < SAMPLE_TOP_K; i++) {
    values[i] = -INFINITY;
    ids[i] = 0;
  }
  for (int v = 0; v < model.out_vocab; v++) {
    if (is_blocked_token(v, blocked, n_blocked)) continue;
    float value = s.logits[v];
    if (value <= values[SAMPLE_TOP_K - 1]) continue;
    int i = SAMPLE_TOP_K - 1;
    while (i > 0 && value > values[i - 1]) {
      values[i] = values[i - 1];
      ids[i] = ids[i - 1];
      --i;
    }
    values[i] = value;
    ids[i] = v;
  }

  float total = 0.0f;
  float peak = values[0];
  for (int i = 0; i < SAMPLE_TOP_K; i++) {
    values[i] = expf((values[i] - peak) / SAMPLE_TEMPERATURE);
    total += values[i];
  }
  float target = ((sample_random_u32() >> 8) * (1.0f / 16777216.0f)) * total;
  for (int i = 0; i < SAMPLE_TOP_K - 1; i++) {
    if (target < values[i]) return ids[i];
    target -= values[i];
  }
  return ids[SAMPLE_TOP_K - 1];
}

// The TinyStories model writes raw English UTF-8 bytes. Near the output limit,
// stop after a token whose final byte closes a sentence instead of always
// exhausting the context in the middle of one.
static bool token_ends_sentence(int tok) {
  if (tok < 0 || tok >= VOCAB_N) return false;
  int end = VOCAB_OFF[tok + 1];
  if (end <= VOCAB_OFF[tok]) return false;
  unsigned char last = VOCAB_BLOB[end - 1];
  return last == '.' || last == '!' || last == '?';
}

static void discard_prompt_line() {
  for (;;) {
    while (!Serial.available()) delay(10);
    int c = Serial.read();
    if (c == '\n') return;
  }
}

// Read one printable-ASCII prompt from USB CDC. The on-device tokenizer
// intentionally rejects non-ASCII instead of silently encoding it differently
// from the tokenizer used to train this English-language model.
static int read_prompt(char *out, int cap) {
  // Windows serial monitors commonly submit CR, whereas terminals usually
  // submit LF. Remember a completed CR so its optional LF partner is not
  // interpreted as an empty prompt on the next call.
  static bool skip_lf_after_cr = false;
  int n = 0;
  for (;;) {
    while (!Serial.available()) delay(10);
    int c = Serial.read();
    if (skip_lf_after_cr && c == '\n') {
      skip_lf_after_cr = false;
      continue;
    }
    skip_lf_after_cr = false;
    if (c == '\r') {
      skip_lf_after_cr = true;
      Serial.println(); out[n] = '\0'; return n;
    }
    if (c == '\n') { Serial.println(); out[n] = '\0'; return n; }
    if (c < 0x20 || c >= 0x7f) {
      Serial.println("\ninput must be printable ASCII");
      discard_prompt_line();
      return -1;
    }
    if (n + 1 >= cap) {
      Serial.println("\nprompt is too long");
      discard_prompt_line();
      return -1;
    }
    out[n++] = (char)c;
    Serial.write((uint8_t)c);
  }
}

static void generate(const uint16_t *prompt_ids, int n_prompt) {
  if (n_prompt >= model.c.seq_len) {
    Serial.printf("prompt has %d tokens; limit is %d\n", n_prompt,
                  model.c.seq_len - 1);
    return;
  }

  Serial.print(">>> ");
  int pos = 0, tok = 0;
  int recent[NGRAM_WINDOW];
  int n_recent = 0;
  int64_t decode_us = 0;
  int decoded = 0;

  for (int i = 0; i < n_prompt; i++) {
    tok = prompt_ids[i];
    emit(tok);
    llm_forward(&model, tok, pos++, &s);
    remember_token(tok, recent, &n_recent);
  }

  llm_profile_reset(&s);
  int64_t t_start = esp_timer_get_time();
  int max_generate = N_GENERATE;
  int room = model.c.seq_len - pos;
  if (max_generate > room) max_generate = room;
  int ending_from = max_generate - ENDING_WINDOW;
  if (ending_from < 0) ending_from = 0;
  for (int step = 0; step < max_generate; step++) {
    tok = select_next_token(recent, n_recent);
    // The training stream places this special token between stories. Its decode
    // span is empty, so stop here instead of silently starting a new story.
    if (tok == VOCAB_EOT) break;
    emit(tok);
    blink((step & 1) ? 40 : 8);

    int64_t d0 = esp_timer_get_time();
    llm_forward(&model, tok, pos++, &s);
    remember_token(tok, recent, &n_recent);
    decode_us += esp_timer_get_time() - d0;
    decoded++;
    if (decoded >= ending_from && token_ends_sentence(tok)) break;
    if ((step & 7) == 0) delay(0);
  }
  int64_t total_us = esp_timer_get_time() - t_start;

  Serial.printf("\n\n--- %d tokens in %.2f s ---\n", decoded, total_us / 1e6);
  Serial.printf("throughput: %.2f tok/s   (%.1f ms/token)\n",
                decoded * 1e6 / total_us, decode_us / 1000.0f / decoded);
  if (s.profile.calls) {
    float n = (float)s.profile.calls * 1000.f;
    Serial.printf("profile ms/token: input %.1f | attn %.1f | ffn %.1f | ple %.1f | head %.1f\n",
                  s.profile.input_us / n, s.profile.attn_us / n,
                  s.profile.ffn_us / n, s.profile.ple_us / n,
                  s.profile.head_us / n);
  }
  blink(0);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== ESP32-S3 PLE TinyLM ===");

  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "model");
  if (!part) { Serial.println("model partition not found"); return; }
  const void *base;
  esp_partition_mmap_handle_t h;
  esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                     ESP_PARTITION_MMAP_DATA, &base, &h);
  if (err != ESP_OK) { Serial.printf("mmap failed: %d\n", err); return; }

  if (llm_load((const uint8_t *)base, &model)) { Serial.println("bad model magic"); return; }
  Cfg *c = &model.c;
  Serial.printf("model: Vin=%d Vout=%d D=%d L=%d H=%d F=%d P=%d S=%d  (mapped %.1f MB)\n",
                c->vocab, model.out_vocab, c->dim, c->n_layers, c->n_heads,
                c->ffn, c->ple_dim, c->seq_len, part->size / 1e6);

#if USE_DISPLAY
  display_begin();
#endif

  if (bpe_tokenizer_load(TOKENIZER_ENCODER_ASSET,
                         TOKENIZER_ENCODER_ASSET_SIZE, &tokenizer)) {
    Serial.println("bad tokenizer encoder asset");
    return;
  }
  // Vin is an aligned input-embedding capacity and can be larger than the
  // trained tokenizer. Every tokenizer id must fit Vin, while the encoder and
  // decoder must exactly cover the logits the model emits.
  if (tokenizer.active_vocab > (uint32_t)c->vocab ||
      tokenizer.active_vocab != (uint32_t)model.out_vocab) {
    Serial.printf("FATAL: tokenizer/model mismatch: encoder %u, Vin %d, Vout %d\n",
                  (unsigned)tokenizer.active_vocab, c->vocab, model.out_vocab);
    return;
  }

  // vocab.h carries the decode table. It must cover every emitted logit.
  if (VOCAB_N != model.out_vocab) {
    Serial.printf("FATAL: tokenizer/model mismatch: vocab.h %d, model %d\n",
                  VOCAB_N, model.out_vocab);
    return;
  }
  if (VOCAB_EOT < 0 || VOCAB_EOT >= model.out_vocab) {
    Serial.printf("FATAL: invalid EOT token id %d\n", VOCAB_EOT);
    return;
  }

  alloc_scratch();
  copy_norms_to_sram();
  Serial.printf("hot set-> SRAM   %u B dynamic + %u B static = %u B managed\n",
                (unsigned)sram_used, (unsigned)STATIC_SRAM_BYTES,
                (unsigned)(sram_used + STATIC_SRAM_BYTES));

  // Stage every per-position tensor to int8 in PSRAM.
  int want = llm_core_stage_count(&model);
  int staged = llm_stage_core_int8_alloc(&model, ps);
  if (staged != want) {
    Serial.printf("FATAL: staged %d/%d core tensors\n", staged, want);
    while (1) delay(1000);
  }
  // The tied head is read per token, not per position, so the core helper does
  // not walk it. Stage it too: it is 85%% of the dense MACs.
  {
    void *b = ps_or_die(llm_stage_int8_bytes(&model.out_head), "staged head");
    llm_stage_int8(&model.out_head, b);
    ++staged;
  }
  Serial.printf("weights-> PSRAM  %d tensors int8, %.2f MB allocated\n",
                staged, psram_used / 1048576.0);

  main_h = xTaskGetCurrentTaskHandle();
  int dual_core_active = 0;
  if (xTaskCreatePinnedToCore(worker_main, "mv", 4096, NULL, 2, &worker_h, 0) == pdPASS) {
    // After the worker exists: matvec_par notifies worker_h.
    model.layer_matvec = matvec_par;
    model.head_matvec  = matvec_par;
    dual_core_active = 1;
  } else {
    Serial.println("dual-core worker failed; running single core");
  }
  Serial.printf("runtime: cpu=%u MHz | seq=%d | dual-core=%s\n",
                (unsigned)(esp_clk_cpu_freq() / 1000000), c->seq_len,
                dual_core_active ? "on" : "off");

  // FNV-1a over the mapped image. scripts/deploy.sh prints the same value for
  // the file it flashed; the two must agree.
  {
    const uint8_t *img = (const uint8_t *)base;
    uint32_t fp = 2166136261u;
    for (size_t i = 0; i < model.image_bytes; i++) { fp ^= img[i]; fp *= 16777619u; }
    Serial.printf("build: bytes=%u fp=%08x sram=%uB psram=%.2fMB\n",
                  (unsigned)model.image_bytes, fp,
                  (unsigned)(sram_used + STATIC_SRAM_BYTES),
                  psram_used / 1048576.0);
  }
  Serial.printf("free: sram %.0f KB | psram %.2f MB\n\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0,
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0);

  Serial.println("type an English prompt and press Enter");
}

void loop() {
  char prompt[BTK_MAX_INPUT_BYTES + 1];
  uint16_t prompt_ids[BTK_MAX_INPUT_BYTES];
  Serial.print("prompt> ");
  int bytes = read_prompt(prompt, sizeof(prompt));
  if (bytes <= 0) {
    if (bytes == 0) Serial.println("prompt must not be empty");
    return;
  }
  int n_prompt = bpe_encode_ascii(&tokenizer, prompt, prompt_ids,
                                  BTK_MAX_INPUT_BYTES);
  if (n_prompt < 0) {
    Serial.printf("tokenizer rejected prompt: %d\n", n_prompt);
    return;
  }
  generate(prompt_ids, n_prompt);
}
