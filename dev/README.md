# 中文故事生成改进计划

> 本文件只记录后续计划。当前不改变已发布英文 TinyStories 模型的训练数据、权重或推理行为。

## 当前状态

- 硬件目标为 ESP32-S3 N16R8（16 MB Flash、8 MB PSRAM）。
- 当前部署的是英文 TinyStories 续写模型，不是指令或聊天模型。
- 固件可通过 USB 串口接收英文 ASCII 开场白，并用 ByteLevel-BPE 编码后续写。
- 单次生成受 512-token 上下文窗口限制；固件在接近最大可用生成长度的最后 32 token 内，遇到英文 `.`, `!` 或 `?` 时提前停止，否则在上限停止。
- 当前目标是先验证英文续写质量；中文输入、中文模型与风格控制不在当前实现范围内。

## 目标

在不超出 ESP32-S3 的 Flash、PSRAM 和生成延迟预算的前提下，逐步实现可控制风格、长度和结尾质量的中文故事续写。

## 分阶段路线

### 阶段 1：英文续写基线验证

目的：确认串口 prompt、最大长度与句末收尾在真实设备上的行为。

1. 使用短、明确的英文开场测试，例如 `A little rabbit wanted to make a new friend.`。
2. 记录 prompt token 数、输出 token 数、是否在 `. ! ?` 处停止、总耗时和 token/s。
3. 分别测试短 prompt 与接近 512-token 上下文限制的 prompt。
4. 若结尾仍常被截断，调整 `ENDING_WINDOW`；每次只调整一个值并重新记录结果。

验收条件：常见短 prompt 不会在词或句子中间因长度限制截断；生成速度与当前约 8--10 token/s 基线没有明显回退。

涉及文件：

- `firmware/esp32_tinystories/esp32_tinystories.ino`
- `scripts/benchmark_device.py`
- `tests/test_tinystories_prompt.py`

### 阶段 2：长度档位

目的：以固件长度预算控制短、中、长故事，不依赖模型理解精确词数指令。

设计：

- `short`、`medium`、`long` 映射到三个 token 目标值。
- 每个档位都保留句末收尾窗口和上下文硬上限。
- 串口输入采用一个简单的档位前缀或菜单选择；不引入聊天协议。

建议初始值（需在实机上校准）：

| 档位 | 目标输出 token | 英文大致长度 |
|---|---:|---:|
| short | 80 | 约 50 词 |
| medium | 160 | 约 100 词 |
| long | 300 | 约 200 词 |

验收条件：档位之间的输出长度明显可区分，且仍优先落在句末；任何 prompt 与输出之和不超过 512 token。

### 阶段 3：训练中文故事模型

目的：替换英文语料、tokenizer 和权重，使模型真正学习中文故事，而不是将中文当作未知字节序列。

1. 确定有明确授权的 UTF-8 中文故事语料，并保留每篇故事的边界。
2. 将 `research/tinystories/prepare.py` 参数化，支持本地中文语料和独立的数据输出目录；不得覆盖英文 TinyStories 数据。
3. 训练独立的中文 ByteLevel-BPE tokenizer。优先从 16k vocabulary 开始；质量不足时再评估 32k。
4. 使用现有 `research/tinystories/train.py`、`export.py` 训练并导出中文 PLE 模型。
5. 导出的 `model.bin`、`tokenizer.json`、golden 文件必须成组保存并进行 tokenizer SHA-256 校验。

验收条件：电脑端 `sample.py` 对中文 prompt 能生成可解码中文；C host golden gate 和设备部署检查均通过。

风险：中文词表变大或输出头变宽会增加 Flash 占用、PSRAM logits 缓冲和每 token 延迟。16 MB Flash 是硬约束。

### 阶段 4：中文输出与收尾

目的：让设备正确输出中文 UTF-8，并按中文句末自然停下。

1. 修改 `firmware/esp32_tinystories/tools/generate_vocab.py`，从 ByteLevel-BPE 的原始 token 字节构建解码表，不能逐 token 调用 `decode([id])` 后再拼接。
2. 生成头文件时导出 `<|endoftext|>` 的 token ID。
3. 在固件中优先检测 `<|endoftext|>`；达到长度目标后再检测 `。`、`！`、`？` 的 UTF-8 字节序列作为兜底。
4. 保持串口 UTF-8 输出；现有 6x8 ASCII OLED/TFT 字体不作为中文显示方案。

验收条件：串口输出不出现乱码或替换字符；故事优先在 EOS 或中文句末结束；达不到上述条件时才使用硬上限。

涉及文件：

- `firmware/esp32_tinystories/tools/generate_vocab.py`
- `firmware/esp32_tinystories/esp32_tinystories.ino`
- 新增针对 UTF-8 分词和词表导出的 Python 测试。

### 阶段 5：中文主题与风格控制

目的：使输入的主题或风格成为训练分布的一部分，而非期待小型续写模型可靠执行自然语言命令。

训练样本格式建议：

```text
<|style:童话|><|length:short|>
标题：小猫和星星
故事：从前……
<|endoftext|>
```

首版只使用有限且稳定的标签：

- 风格：`童话`、`科幻`、`悬疑`
- 长度：`short`、`medium`、`long`

固件先支持预设主题/风格选择；任意中文串口输入需要单独实现与 Hugging Face tokenizer 一致的 Unicode ByteLevel-BPE 编码器，并应作为独立任务。

验收条件：相同主题下切换标签能够产生可观察的风格/长度差异；未知标签被固件拒绝或回退到默认标签。

## 约束与原则

- 模型、tokenizer、解码词表必须来自同一训练/导出批次，禁止混用。
- 优先在主机上运行 tokenizer、golden 和导出测试，再烧录设备。
- 每次只改变一个变量：词表规模、训练语料、采样方式或收尾窗口不能同时变更。
- 不以 OLED 显示正确性作为中文能力验收；中文首期以 USB 串口为准。
- 当前模型使用贪心 argmax。温度、top-k 随机采样属于质量多样性优化，应在中文基础链路稳定后再评估。

