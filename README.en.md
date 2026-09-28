# Universal Production Tools

An Unreal Engine 5.7 editor plugin that **reproduces the camera composition of a reference video**
as a Level Sequence.

Feed it a video: it splits the footage into shots, reads each shot's subject position, size and
camera angle, and builds a Level Sequence that frames your level's characters the same way.
It runs on local pose analysis (YOLOX + RTMPose) — no LLM required.

[한국어](README.md) · [One-page summary](Docs/PORTFOLIO.md) (Korean)

![reference vs generated](Docs/images/reference_vs_generated.gif)

*Left: the reference clip. Right: the Unreal sequence generated from it. Shot segmentation, subject
size, camera angle and over-the-shoulder placement are reproduced together. (The reference here is a
five-shot test clip built for the evaluation suite.)*

## What it catches

Over-the-shoulder shots. The solver reported **success in both cases**, but in the first one the
foreground person had fallen out of the frame — it only checked horizontal placement and never
looked at the vertical.

| Before — only a blob in the bottom corner | After — shoulder sits on the right edge |
|---|---|
| ![before](Docs/images/ots_before.jpg) | ![after](Docs/images/ots_after.jpg) |

The same code asked for the opposite side. The foreground's head and shoulder frame the left edge
without covering the subject's face.

![left side](Docs/images/ots_after_left.jpg)

To catch this without a human looking at every frame, generated shots are re-rendered and measured.
Green X is where the reference asked the subject to be; red + is where it actually landed.

![verification overlay](Docs/images/verify_overlay.jpg)

## The point: verified with numbers, not eyeballs

The core of this repository is not the feature list — it is **the apparatus that measures accuracy**.

Generated sequences are re-captured and re-measured with the same pose analyzer, compared against
the reference, and the resulting numbers are locked down by a regression suite. Because the engine
knows the camera and the bone positions, ground truth can be generated directly.

```
Tools/eval/
  editor/upt_capture_*.py   capture ground truth inside the editor (bone positions, screen projections)
  eval_*.py                 measure error against ground truth
  run_regression.py         run every suite, compare against baseline.json
```

**11 suites** currently run. A sample:

| Suite | What it protects | Current |
|---|---|---|
| `framing_video` | shot segmentation, subject position error | horizontal error ≤ 0.02 |
| `twoshot_video` | two-character blocking, over-the-shoulder detection | 0 false positives |
| `letterbox_video` | vertical video with letterboxing and burned-in titles | 20 shots detected |
| `body_pose` | video joints vs. actual bones | 2.7% screen error |
| `body_height` | full-body height estimation, two body types | 11.6% median |

## What the apparatus actually found

Problems that would have shipped unnoticed without measurement. Full write-ups live in [Docs](Docs)
(Korean).

- **Over-the-shoulder shots passed horizontally and fell out of frame vertically.** The solver
  reported success while the foreground person was not on screen at all.
  → [OTS_FRAMING_FIX.md](Docs/OTS_FRAMING_FIX.md)
- **The verifier mis-measured a cropped subject's position by 9%.** Extrapolating the body axis of a
  cropped, leaning figure pushes the aim point sideways. Three candidate measurement rules were
  scored against ground truth and the best one replaced it (error 0.035 → 0.012).
- **Automatic over-the-shoulder verification was impossible by recognition.** Neither the person
  detector nor the segmentation model sees a close, untextured mannequin shoulder as a person (both
  returned zero). Replaced by rendering a second frame with the foreground hidden and diffing the
  two — no model involved, and it works.
- **Depth from video-derived 3D joints was unusable.** Depth correlation 0.215; the right hand
  correlated at −0.35 (moving the wrong way) and both feet at 0.0 — it cannot tell which foot is
  forward. **Animation baking was stopped there.**
  → [BODY_MOTION_FROM_VIDEO.md](Docs/BODY_MOTION_FROM_VIDEO.md)

  Below: joints estimated from video (red) over the engine's actual bone positions (green). The 2D
  estimate is usable at 7.2% of body height; the depth was noise.

  ![joint comparison](Docs/images/bodypose_overlay.jpg)

Changes that were measured and then **reverted** are recorded too. Swapping the size score to an
exact geometric height made results worse (the reference side is measured by the same estimator), and
the improvement from widening a body-proportion clamp did not reproduce on a re-capture.
→ [BODY_HEIGHT_ESTIMATOR.md](Docs/BODY_HEIGHT_ESTIMATOR.md)

## Layout

```
Source/UniversalProductionTools/   C++ (Slate panel, blocking solver, sequence builder, automation tests)
Tools/                             Python analyzers (pose analysis, link download, library search, verification)
Tools/eval/                        ground-truth capture + error measurement + regression
Docs/                              engineering notes (Korean)
MetaHumanCapture58/Scripts/        UE 5.8 MetaHuman face capture scripts
```

## Setup

### 1. Plugin

Drop it under your project's `Plugins/` and build the editor target.

```bash
"C:/Program Files/Epic Games/UE_5.7/Engine/Build/BatchFiles/Build.bat" \
  <Project>Editor Win64 Development -Project="<path>/<Project>.uproject" -WaitMutex
```

### 2. Analyzer environment

```powershell
Tools\setup_analyzer_env.ps1
```

Creates a Python environment at `ThirdParty/UPTAnalyzer/.venv` and installs
`requirements-pose-analyzer.txt`. Pose models (ONNX) download on first run and are not committed.

### 3. Verify

```bash
cd Tools/eval
../../../../ThirdParty/UPTAnalyzer/.venv/Scripts/python.exe run_regression.py
```

Suites without ground-truth data (`Saved/UniversalProductionTools/GroundTruth/`) are skipped.
Ground truth is produced by running `Tools/eval/editor/upt_capture_*.py` inside the editor.

## Usage

Open `Window > Universal Production Tools` in the editor.

1. Pick a reference video file or paste a link (a time range can be specified).
2. Select the character actors in the level and pull them in.
3. Analyze → review role mapping → build the Level Sequence.
4. Edit individual shots (size, angle, over-the-shoulder) and re-apply to the same sequence.

The same pipeline runs headless from the console:

```
UPT.ReferenceE2E "<reference_plan.json>" <ActorLabel...> [-editshot=N] [-link=URL -section=A-B -rights]
```

The complete feature list, the optional LLM path and the skeleton analysis / retargeting features are
in [PLUGIN_FEATURES.md](Docs/PLUGIN_FEATURES.md) (Korean).

## Known limits

- **Faceless mannequins** score poorly on close-up composition. Reproducing an extreme close-up of a
  real person's face is not possible in principle (a 20-shot vertical-video set scores 23/100).
- **A white mannequin in a face close-up** fails person detection outright (30 of 126 frames).
- **Body-motion extraction is stopped** because of the depth problem above. With the
  MetaHumanBodyTracker plugin, the UE 5.8 path would be the proper route.
- Ground truth covers **two characters only** (an armored stylized character and a standard
  mannequin). More varied body types would require re-checking the calibration constants.

## License

Personal project. All rights reserved until a license is stated.
