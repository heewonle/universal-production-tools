# Universal Production Tools

An Unreal Engine 5.7 editor plugin that **reproduces the camera composition of a reference video**
as a Level Sequence, and **moves character animation between skeletons automatically**.

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

**12 suites** currently run. A sample:

| Suite | What it protects | Current |
|---|---|---|
| `framing_video` | shot segmentation, subject position error | horizontal error ≤ 0.02 |
| `twoshot_video` | two-character blocking, over-the-shoulder detection | 0 false positives |
| `letterbox_video` | vertical video with letterboxing and burned-in titles | 20 shots detected |
| `body_pose` | video joints vs. actual bones | 2.7% screen error |
| `body_height` | full-body height estimation, three body types (378 frames) | 10.1% median |
| `retarget` | does retargeting preserve the motion (2 sources x 3 targets, 19 pairs) | 0.7° median angle |

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


## What the panel actually does

Separately from verifying the pipeline through console commands, the editor panel itself was driven
end to end (level selection → JSON input → generate → edit a shot).

![panel walkthrough](Docs/images/panel_walkthrough.gif)

Confirmed: selecting an actor in the level updates the panel immediately; the
`Shot Plan JSON` mode runs with no video and no API (a three-shot plan produced
`/Game/Cinematics/Generated/Panel_Test` with lenses 28/50/85 mm and screen positions listed); and
selecting a shot opens the per-shot editor including the over-the-shoulder controls.

One small issue found on the way: **the over-the-shoulder actor list includes the shot's own subject.**
The solver rejects that combination, so nothing breaks, but the UI should exclude it up front.

### The over-the-shoulder edit, driven end to end from the panel

Picking a foreground actor and pressing **apply to this shot and update the sequence** was verified
twice, with the two characters placed differently each time.

**1. Foreground not in front of the subject — the apply is refused**

![over-the-shoulder guard](Docs/images/panel_ots_guard.jpg)

With `AutoHero2` standing beside the subject rather than in front of it, applying leaves the sequence
untouched and only warns. Rather than quietly producing a shot where the camera swings behind the
subject, it stops. The shot row still reads 50 mm and the azimuth is still 12.99°.

**2. Move the foreground in front — the same sequence is updated in place**

![over-the-shoulder applied](Docs/images/panel_ots_applied.jpg)

Moved `AutoHero2` in front of the subject and re-applied: it passes, and the shot is recomputed to
**50 → 44.67 mm, azimuth 12.99 → 24.13°, distance 266.8 → 233.3 cm**. Re-solving the camera height
for the over-the-shoulder framing also flipped the angle label from eye level to high angle.
**No new asset is created and the shot count stays at 2** — the same sequence is edited where it is.

The actual camera preview:

![over-the-shoulder preview](Docs/images/panel_ots_preview.jpg)

*The subject on the left, the foreground person's shoulder filling the right edge.*

One more thing surfaced here. **Composition check** rates this shot
`LOW 15/100 | full-body vertical occupancy 170%, head top y=0.02 / not enough headroom`.
Cropping the full body is the intended framing for an over-the-shoulder medium shot, yet it still
scores low — **the checker does not take shot size into account**, and that is on the fix list.

## Character animation pipeline

Separate from composition reproduction, the plugin also **moves animation between characters and
creates it from video**. Roughly 1,100 lines of C++ plus editor Python.

**1. Automatic skeleton analysis** — `UPTSkeletonAnalyzer` (395 lines)
Combines bone names, hierarchy and reference-pose spatial positions to infer humanoid roles (pelvis,
spine, both arms, both legs), with per-item confidence and an A/B/C readiness grade. Twist, finger and
face helper bones are classified separately and excluded from the core judgement. Non-standard naming
such as `L-Forearm` or `RigLArm1` is recognised.

**2. IK Rig / IK Retargeter generation** — `UPTIKRigBuilder` (214), `UPTIKRetargeterBuilder` (124)
Uses the engine's Auto Characterizer (`IKRigAutoCharacterizer`) first and falls back to the analysis
above for hierarchies the engine templates do not cover. Builds chains, hand/foot Full Body IK goals
and elbow/knee joint-limit presets.

**3. Batch retargeting** — `SUPTAnimationRetargetWindow` (317 lines)
Right-click a target mesh in the Content Browser, select any number of animations, convert in one go.
Profiles, IK Rigs and Retargeters are prepared per source skeleton automatically; upper/lower body
modes also emit slot Montages. **Below 70% readiness the automatic path stops and hands the case back
for manual review** — it does not quietly emit broken results.

**4. Face animation from a single video** — UE 5.8 MetaHuman Animator → transferred into 5.7
Solves face control curves from phone footage (mono, no Identity required). FBX import would not
produce an animation asset in 5.7, so **only the curve values were transferred and the AnimSequence was
re-baked in 5.7** (`Tools/editor/build_face_anim_57.py`).

![face curves](Docs/images/face_curves.jpg)

*134 of 251 controls actually animate. 191 frames, 23.976 fps, 8.0 s.*

What is blocked is recorded here too. **Body motion is stopped** because of the depth problem above;
with the `MetaHumanBodyTracker` plugin the UE 5.8 route would be the proper one. The traps hit on the
5.8 side (frame filename rules, `metadata.frame_rate`, the async pipeline, hardcoded paths inside the
engine's own exporter) are written up in
[METAHUMAN_CAPTURE_5_8.md](Docs/METAHUMAN_CAPTURE_5_8.md) (Korean).


### The animation side is measured too

**Skeleton analyzer validated across 1,691 assets.** Automatic retargeting stops below 70% readiness,
but nobody had checked whether that gate behaves on real assets. A console command
(`UPT.SkeletonAudit`) analyses every Skeletal Mesh in the project in 21 seconds.

The 196 C-tier meshes were mostly two-bone cubes, camera rigs, vehicle templates, spiders and dragons —
**the gate was blocking the right things**. But **7 of the meshes that passed carried a wrong mapping**.
Mixamo's `X_Bot` had **only its left leg shifted by one joint** (LeftThigh←LeftLeg) while the right leg was
correct, so it scored **100% readiness and passed**. Roles were chosen independently of one another and
never checked for mutual consistency.

→ Added a **chain consistency pass** after assignment (is each role a descendant of its parent role; are
left/right pairs at the same depth). Exactly **3 of 1,691** meshes changed tier — all genuinely shifted —
with zero false positives. One earlier version was too strict and demoted 10 Sidekick meshes; an exception
for strong name evidence (asymmetric rigs are an asset trait) fixed that.

**Then detection was turned into repair.** The shifted left leg was not a leg problem — it was `Pelvis`.
Mixamo has no separate root bone: the topmost bone `Hips` *is* the pelvis, and it matches the Pelvis
tokens exactly. But roles were assigned in spec order, so **`Root`, with no name evidence at all, took
`Hips` on a "parentless bone" bonus alone.** The displaced `Pelvis` then grabbed `LeftUpLeg`, and
everything below it shifted by one joint.
→ Name-backed assignments now run **first**, and a rig whose root and pelvis are the same bone is allowed
to share it. Across 1,691 meshes **exactly 3 changed tier, all upward**, none down: `X_Bot`,
`Look_Around` and `Polygonal_Golem` went **C 0.44 → A 0.94**, with every limb at confidence 1.00.
Rather than stop at the grade, the fix was measured end to end: **X_Bot now retargets at 5.2°**, on par
with the mannequin's 4.7°. That combination used to be blocked from the automatic path entirely.

**Retargeting quality measured.** Joint positions cannot be compared across body types, so the suite
measures **segment directions** and **foot sliding**. The first run showed legs at 2.2° but **arms off by
52.9°**, with a standard deviation of **0.0** across all 24 samples — the motion transferred exactly, but
the rest-pose (A-pose vs T-pose) difference was never corrected. The builder skipped pose alignment
entirely because `AutoAlignAllBones` can assert on partial mappings; the cost of skipping it had never
been measured.

→ Aligning only the bones of mapped chains brought **median 7.8° → 3.7°, worst 58.4° → 10.4°, arms
52.9° → 0.1°**. Foot sliding stayed at the source level, so retargeting adds none.

**A second character pair then showed that fix was only correct for one pair.** Pointing it at a
character with a completely different build and naming scheme (43 bones, `L-Thigh`) **brought the whole
editor down on an assertion** — the exact assertion the original comment had warned about. The engine's
`AutoAlignBones` reads an empty array as "align everything", and a single bone that also belongs to an
unmapped chain kills the process. After three different ways of deciding "is this chain mapped" all
failed, the alignment is now **restricted to rigs built by the engine's Auto Characterizer** and skipped,
with a log line, for rigs built by the fallback analyzer.

**Then that cost was paid back directly.** Reading how the engine applies a retarget-pose offset
(`LocalRotation = RefLocal · Delta`), the same correction is now **computed by hand, without the engine
API**, and written into the retarget pose. Positions are matched by cumulative length along the chain,
so differing bone counts (twist bones present or not) still line up.
→ golem **18.4° → 5.6°, worst 70.0° → 10.7°**, no assertion.

An **off-by-one-segment** bug in the same rule turned up too. Source segments were picked by the
target segment's *start* ratio: the golem's forearm segment starts at 0.519 and the source's at 0.529,
so **the forearm was being aligned to the upper arm's direction** — and that gap is exactly the elbow
angle, 33°. Matching segment **midpoints** instead brought it to **4.8°**.

That then overturned the earlier conclusion that the hand-rolled alignment was worse than the engine's —
most of that gap *was* the off-by-one (Manny pair 7.2° → **3.3°**, against the engine's 4.7°).
**So the engine alignment path was deleted outright.** The assertion that took the editor down three
times is gone from the code, and so is the branch on how the rig was built. Worst segment across the
whole suite: **33.1° → 10.7°**; median **4.8° → 3.5°**.

**Whether the hand-rolled version could replace the engine's was measured too — it could not.**
Switching the Manny pair to it moved 4.7° → 7.2°, so it was reverted. Both paths stay: the engine's
alignment for engine-built rigs, the hand-rolled one for fallback rigs.

The sample grew as well: **a 180° turn, a running jump and an arm swipe** joined walking and running,
and **a second source skeleton** (UE4 mannequin) was added, for 2 sources × 2 targets = **14 pairs**.
The 3.7° reported earlier was **the median of two easy motions**; with the wider sample it is 4.7°.

**A ground-contact metric, independent of the angles, was then built** — the foot's height above the
floor over time, compared against the source. It immediately showed the plan was aimed at the wrong
thing: the `calf→foot` angle that was going to be fixed sits at 9.5–13.3° across all 10 pairs —
**essentially constant, while the contact error varies 18×**. That angle does not explain foot-plant
quality. The real problem it surfaced: **in a standing idle the golem's feet hover 12–24% of leg length
above the floor**, where the source's are flat on it.

The same metric was used to check that the rest-pose alignment did not trade contact for angles. Toggled
on and off from a console variable, both improve — angles 18.6° → 6.5°, contact 0.0409 → 0.0229. It is
not a trade-off.

The hovering feet were chased to the end and **the cause was not found.** Comparing pelvis-height curves
showed that **of four source→target combinations only the UE-standard-to-UE-standard one carries vertical
pelvis motion through; the other three come out at exactly zero.** Three hypotheses — a duplicated default
op stack, the rest-pose alignment itself, and the choice of retarget root — were each rejected by
measurement, and the same behaviour **reproduces through the engine's own API with the plugin out of the
loop.** What could not be found is written down as not found; how bad it is and what it is *not* are recorded
as numbers.

**With the cause out of reach, the symptom was fixed directly.** A post-process now redoes the
calculation the engine op was supposed to do and writes it into the result's track. Combinations that
already carry pelvis motion are left alone; only the ones that come out at zero on all three axes get
filled in. Two mistakes along the way, both caught by measurement: a reversed composition order showed
up immediately as a height ratio of `-0.000`, and then the keys were going to **a bone that does not
carry the body's height** (the golem's `Root→CG→Pelvis` has zero local offset on `Pelvis`; `CG` holds
the height — and `CG` turned out to be the bone the retargeter actually drives, which became obvious
from **which bone's rotation changes**).

| metric | before | after |
|---|---|---|
| worst foot clearance error | 0.192 | **0.0413** |
| contact mismatch | 0.0625 | **0.0417** |
| foot sliding (source 0.0391) | 0.0708 | **0.0428** |
| segment angles | 3.5° / 10.7° | **unchanged** |

**Feet that hovered up to 19% of leg length now sit at 4%**, and not one angle moved — which is exactly
what should happen when only translation is touched, and is the check that it was.

**And the cause did turn up in the end.** Every comparison until then had been **confounded by using
different meshes**. A switch that forces the fallback rig on the *same* mesh settled it in one run:
engine rig 11.2, fallback rig **0.00**. Not the engine, not the retargeter — **our rig construction**.
Diffing the two rigs showed it immediately: our `Root` chain ran `root → pelvis`, which **traps the
pelvis inside an FK chain**, and FK chain retargeting carries rotation only, so the pelvis translation
gets overwritten by the rest pose.

Three fixes — shrink the `Root` chain to the root bone alone, pick the retarget root as the bone that
**actually carries the body's height** (`CG` on the golem), and drop `Root` from the required-chain list
(that requirement was **pushing Mixamo rigs off the engine path onto the fallback**). All three
combinations now move the pelvis correctly, and **the post-process baker stands down and writes nothing.**

In hindsight, the baker built as "fix the symptom since the cause is out of reach" is what opened the
door: the **"which bone carries the height" rule** written for it became the core of the second fix, and
the fact that the baker's numbers and the engine op's numbers **agree to the decimal** is the check that
both are right.

## Layout

```
Source/UniversalProductionTools/   C++ (Slate panel, blocking solver, sequence builder, automation tests)
Tools/                             Python analyzers (pose analysis, link download, library search, verification)
Tools/eval/                        ground-truth capture + error measurement + regression
Tools/editor/                      5.7 editor utilities (bake 5.8 face curves into an AnimSequence)
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
