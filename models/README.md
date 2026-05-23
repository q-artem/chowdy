# Models

This directory holds the ONNX models used by `chowdy`. The `.onnx` files are
intentionally excluded from version control (see `.gitignore` rule
`models/*.onnx`); each developer downloads them locally with the commands
below.

Both files were extracted from the official InsightFace `buffalo_s` package
(`https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_s.zip`).
The unused models from the package (`genderage.onnx`, `2d106det.onnx`,
`1k3d68.onnx`) were discarded.

## Files

| File | Role | Size | Source | License | SHA-256 |
| --- | --- | --- | --- | --- | --- |
| `scrfd_500m_bnkps.onnx` | Face detector (SCRFD-500MF with 5-point landmarks). Original name inside `buffalo_s.zip` is `det_500m.onnx`; renamed for clarity. | 2 524 817 B (~2.5 MB) | `buffalo_s.zip` v0.7 from `deepinsight/insightface` GitHub release | InsightFace non-commercial research license (see upstream repo). | `5e4447f50245bbd7966bd6c0fa52938c61474a04ec7def48753668a9d8b4ea3a` |
| `w600k_mbf.onnx` | Face embedder, 512-d output (MobileFaceNet @ WebFace600K). **Fallback** for EdgeFace-XS-gamma-06: see note below. | 13 616 099 B (~13 MB) | `buffalo_s.zip` v0.7 from `deepinsight/insightface` GitHub release | InsightFace non-commercial research license (see upstream repo). | `9cc6e4a75f0e2bf0b1aed94578f144d15175f357bdc05e815e5c4a02b319eb4f` |

## Re-download (one-liners)

Both files live inside the same archive, so the simplest way is to fetch the
zip once and extract the two needed entries:

```sh
# Both models in one shot (downloads ~122 MB, keeps ~16 MB on disk):
curl -L -o /tmp/buffalo_s.zip \
  https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_s.zip \
  && unzip -j -o /tmp/buffalo_s.zip det_500m.onnx w600k_mbf.onnx -d models/ \
  && mv models/det_500m.onnx models/scrfd_500m_bnkps.onnx \
  && rm /tmp/buffalo_s.zip
```

Individual fetches (same archive, but kept as separate commands so they can be
copy-pasted independently):

```sh
# scrfd_500m_bnkps.onnx (extract det_500m.onnx from buffalo_s.zip)
curl -L -o /tmp/buffalo_s.zip \
  https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_s.zip \
  && unzip -j -o /tmp/buffalo_s.zip det_500m.onnx -d models/ \
  && mv models/det_500m.onnx models/scrfd_500m_bnkps.onnx \
  && rm /tmp/buffalo_s.zip
```

```sh
# w600k_mbf.onnx (extract from the same buffalo_s.zip)
curl -L -o /tmp/buffalo_s.zip \
  https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_s.zip \
  && unzip -j -o /tmp/buffalo_s.zip w600k_mbf.onnx -d models/ \
  && rm /tmp/buffalo_s.zip
```

Verify integrity after download:

```sh
sha256sum -c <<'EOF'
5e4447f50245bbd7966bd6c0fa52938c61474a04ec7def48753668a9d8b4ea3a  models/scrfd_500m_bnkps.onnx
9cc6e4a75f0e2bf0b1aed94578f144d15175f357bdc05e815e5c4a02b319eb4f  models/w600k_mbf.onnx
EOF
```

## Note on the EdgeFace fallback

The task originally called for **EdgeFace-XS-gamma-06** (Apache-2.0, ~5–8 MB,
from `otroshi/edgeface`). We surveyed the upstream sources and could not find
an ONNX build:

- `github.com/otroshi/edgeface` ships only `.pt` PyTorch checkpoints
  (`checkpoints/edgeface_xs_gamma_06.pt`) and a `torch.hub` loader.
- The HuggingFace mirrors in the `Idiap/EdgeFace-*` collection
  (`EdgeFace-XXS`, `EdgeFace-XS-GAMMA`, `EdgeFace-S-GAMMA`, `EdgeFace-Base`)
  also publish only `.pt` files — no `.onnx`. Note: the HF cards declare
  `cc-by-nc-sa-4.0`, which is stricter than the Apache-2.0 we wanted.
- PyTorch is not installed on this machine and the task forbids on-the-fly
  `.pt -> .onnx` conversion.

As permitted by the task, we therefore fell back to **MobileFaceNet trained on
WebFace600K** (`w600k_mbf.onnx`), the embedder shipped inside the InsightFace
`buffalo_s` bundle. It produces a 512-d L2-normalised embedding, is
ONNX-ready, and matches the API contract we needed.

If/when an Apache-2.0 EdgeFace ONNX appears, swap `w600k_mbf.onnx` for
`edgeface_xs_gamma_06.onnx` and update the embedder loader accordingly.
