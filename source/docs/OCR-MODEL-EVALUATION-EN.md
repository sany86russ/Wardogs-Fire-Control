# OCR model evaluation

Evaluation date: **12 October 2026**.

The main recognizer already uses **Paddle PP-OCRv6 Small** through the project's C++ `RapidOcr` class and ONNX Runtime CPU. The RapidOCR name does not identify a different neural network. The application runs neither the Python RapidOCR package nor Python PaddleOCR; project code locates text lines and verifies coordinate provenance.

SHA256 of the current model:

```text
6f327246b50388f3c176ae304bd95767ea6dc0c9ae92153ef8cbe210b3c14884
```

This matches the model in the [official RapidAI catalogue at the checked revision](https://github.com/RapidAI/RapidOCR/blob/0700743fc8bfb3943cd8e40e84547f14c7d15dfc/python/rapidocr/default_models.yaml). Each model supplies its own character dictionary; the dictionary must not be replaced independently of its weights.

## Measurements

Four models read the same **15 saved crops**: coordinate lines, individual map fields, MIL labels, dark compass labels, and three crops without a complete coordinate pair. There were **60 sequential console launches**, five recognition calls per launch, at Idle priority. All returned exit code 0. No game, application GUI, screen capture or input hook was started.

| Model | Median p50, ms | Initialization, ms | Private memory, MiB | Correct value /12 | Correct in both passes /12 |
|---|---:|---:|---:|---:|---:|
| PP-OCRv6 Small, current | 46.48 | 111.88 | 56.86 | 11 | 10 |
| PP-OCRv6 Tiny | 9.08 | 59.04 | 28.76 | 9 | 9 |
| PP-OCRv6 Medium | 2336.73 | 199.91 | 109.98 | 11 | 11 |
| English PP-OCRv5 mobile | 42.53 | 122.90 | 42.75 | 10 | 9 |

None of the models produced a complete coordinate pair on the three negative crops. This is a bounded refusal check, not an estimate of the in-game error rate.

Timing comes from the existing `ocr_benchmark`: p50 includes the first of five retained-session calls. Initialization uses a new process and recognizer; the OS file cache was not cleared. Results represent one computer and a single-thread CPU configuration. The tool's printed p95 for five calls was not treated as a reliable 95th percentile.

Correctness counts compare exact values on 12 positive crops. Individual map fields are read before production component localization in this experiment. These counts **do not estimate whole-application accuracy, hit percentage or OCR accuracy on arbitrary frames**.

## Decision

Keep **PP-OCRv6 Small for coordinates, map and chat**:

- Tiny is substantially faster but corrupts individual map fields and leading digits.
- Medium improves two-pass agreement on one difficult field, but about 2.3 seconds per line is unsuitable for continuous HUD reading in the tested configuration.
- English mobile is slightly faster and improves some chat lines, but loses leading digits in a map field and a repeated MIL reading.
- No replacement fixes the difficult dark compass label. Clearer, correctly isolated labels are read by all models.

Migrating to Python PaddleOCR would not itself upgrade the weights already in use. The evidence does not justify replacing the entire recognizer without validating the production workflow.

The next priority is precise HUD glyph localization and validation of label-to-tick correspondence. Recognized text and CTC scores do not supply individual glyph coordinates, prove launch direction or replace geometry checks. Tiny can be investigated separately for numeric HUD labels with unchanged safeguards and Small fallback on refusal; these measurements do not yet establish that path's readiness.

## Verified primary sources

- [PaddleOCR v3.7.0](https://github.com/PaddlePaddle/PaddleOCR/releases/tag/v3.7.0), 11 June 2026; checked revision `dab3fe35379033fdcb2d0e9572fac0b36c9a9ebf`.
- [RapidOCR v3.10.0](https://github.com/RapidAI/RapidOCR/releases/tag/v3.10.0), 8 October 2026; checked revision `0700743fc8bfb3943cd8e40e84547f14c7d15dfc`. Its catalogue still references the verified PP-OCRv6 weights from v3.9.2.
- [PaddleOCR recognition models](https://www.paddleocr.ai/latest/en/version3.x/module_usage/text_recognition.html). Public v6 and v5 metrics use different evaluation sets and are not directly comparable.
- [ONNX Runtime quantization](https://onnxruntime.ai/docs/performance/model-optimizations/quantization.html) and [OpenVINO Execution Provider](https://onnxruntime.ai/docs/execution-providers/OpenVINO-ExecutionProvider.html): possible future experiments; application speedup and accuracy preservation remain unproved.

Paddle models use Apache 2.0; ONNX Runtime uses MIT. Component licenses are included in the distribution. This evaluation added no application dependencies.
