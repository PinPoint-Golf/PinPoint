# Corpus 2 — Data Collection Protocol (IMU + HackMotion, concurrent)

**Document type:** Experimental methods / data-collection protocol (collection team).
**Corpus:** 2 of 3 — *concurrent IMU + HackMotion; criterion (concurrent) validity of the wrist axes.*
**Version:** 1.0 (2026-06-26) · **Status:** ready once Corpus 1 frozen + HackMotion pilot complete ·
**Owner:** Mark Liversedge.
**Reference (the "why"):** `docs/validation/pipeline_validation_and_tuning.md` (§3.2 LoA sizing; §6 the
HackMotion criterion programme). **Reporting standard:** GRRAS (Kottner 2011) + the Bland–Altman
reporting conventions (Bland & Altman 1999) for the agreement analysis; STROBE for design.

> **What the collection team must produce:** ~50 **paired** swings in which the *same shots* are recorded
> simultaneously by the PinPoint IMUs **and** the HackMotion wearable sensor, reconciled in sign/zero/time,
> so PinPoint's lead-wrist FE/RUD/PS can be compared against an accepted criterion reference. This is the
> dataset that **locks the metric signs, diagnoses anatomical calibration, and re-seats the scoring bands**.

---

## 0. Changes from Corpus 1 (read first)

Corpus 2 **is Corpus 1's protocol, plus a co-mounted criterion device.** Everything in
`corpus1_collection_protocol.md` still applies (Wrist session, IMU mount/calibration discipline, Tier-0
gate, swing stratification, markup workflow, provenance, the full Corpus-1 test suite as a regression net).
The **deltas** are:

| Area | Corpus 1 | Corpus 2 (new) |
|---|---|---|
| Reference device | none (HackMotion = pilot only) | **HackMotion co-mounted on every paired swing** |
| Unit of analysis | one swing | one **paired** swing (IMU + HackMotion of the same shot) |
| Calibration | PinPoint 2-pose | PinPoint 2-pose **+ HackMotion zero**, both per session |
| New procedures | — | **static-pose anchors**, **temporal-sync action**, **co-mount perturbation check** |
| New artifact | `truth.json` | **`hackmotion.json`** sidecar (reconciled) per paired swing |
| Sizing | reliability/coverage | **Bland–Altman LoA** → n ≈ 50, *confirmed from the Corpus-1 pilot SD* |
| Validity earned | within-subject reliability/consistency | + **concurrent (criterion) validity** |

Still **single golfer** (within-subject) — population generalisation remains Corpus 3.

---

## 1. Study identification & scope

| Field | Value |
|---|---|
| Corpus ID | `corpus-2` |
| Instruments | PinPoint IMUs (forearm + hand; ± upper arm) **+ HackMotion wearable sensor**, worn together |
| Primary unit | one **paired** swing |
| Validity earned | **criterion / concurrent validity** for lead-wrist FE / RUD / PS |
| Validity NOT earned | external/population (Corpus 3); absolute accuracy (HackMotion is a criterion, not gold standard) |

---

## 2. Objectives, hypotheses & pre-registered acceptance

| # | Objective | Pre-registered acceptance | Axis caveat |
|---|---|---|---|
| H-O1 | PinPoint agrees with HackMotion (FE) | RMSE ≤ 4–5°; ICC(A,1) ≥ 0.90; bias ±2° | strongest axis |
| H-O2 | …(PS) | RMSE ≤ 4–5°; ICC ≥ 0.90 | **single- vs two-sensor construct caveat (§7.5)** |
| H-O3 | …(RUD) | RMSE ≤ 8°, *reported honestly as the weak axis* | criterion's own floor ≈ 5° |
| H-O4 | Trajectory shape agrees | Pearson r ≥ 0.95; residual lag < 1 frame after reconciliation | per axis |
| H-O5 | Signs are correct & stable | reconciliation transform constant across swings (sign flip only) | a *varying* offset is a finding |
| H-O6 | Corpus-1 gates still hold | Tier-0 100 %; corr ≥ 0.9; repeatability unchanged | regression net |

**Pre-registration note:** the agreement is bounded by HackMotion's own LoA (ICC 0.95–0.99 / ~1° FE /
~5° RUD). We target *agreement consistent with the criterion's floor*, not zero difference (reference
§6.7).

---

## 3. Study design

- **Design:** prospective, **method-comparison (concurrent criterion)** study, within-subject.
- **Participant:** **one** golfer (same as Corpus 1 if possible, for continuity). Single-golfer is correct
  — sign/`A·M`/measurement-error depend on the instrument + mount, not the population (reference §3.5).
- **Sample size (the LoA driver, reference §3.2):** target **~50 paired swings**. SE of a limit of
  agreement ≈ `1.71·s/√n`; at n = 50 the 95 % CI half-width ≈ 0.48·s. **Confirm `n` per axis from the
  Corpus-1 pilot `s`** (recorded in Corpus-1 §14): FE may permit 40; RUD (loose) may want more. Because all
  three axes are captured on every paired swing, ~50 paired swings sizes FE/RUD/PS together.
- **Reliability sub-stratum:** ≥ 25 paired repeatability swings so **both** devices' SEM is estimated —
  agreement can never be tighter than the criterion's own repeatability (reference §6.5).

---

## 4. Participant: inclusion, consent

As Corpus 1, plus: consent explicitly covers wearing a **third-party device (HackMotion)** and exporting
its data; HackMotion data is stored under the same athlete UUID and data-management plan.

---

## 5. Equipment & instrumentation

As Corpus 1 (§5), **plus:**

| Item | Spec / setting | Note |
|---|---|---|
| HackMotion sensor | on the lead wrist + hand, per HackMotion's mount guide | the criterion device |
| HackMotion app/export | record firmware + export version in `CORPUS.md` | the `hackmotion.json` source schema is confirmed during the pilot |
| Co-mount clearance | HackMotion must not displace the PinPoint hand IMU (🟡) | verified by the perturbation check (§6.4) |
| Sync aid | a defined sync action (e.g. 3 sharp wrist snaps) at each capture block | for clock cross-correlation (§6.5) |

---

## 6. Setup & calibration protocol

### 6.1 Dual calibration (per session)
1. Mount **all** sensors (PinPoint IMUs + HackMotion). Order: PinPoint first, then HackMotion, so the
   HackMotion strap does not shift an IMU after its calibration.
2. **PinPoint** 2-pose calibration + Confirm Tracking (Corpus 1 §6).
3. **HackMotion** zero/calibration per its own app (it zeros at address).
4. **Co-mount perturbation check (§6.4).**

### 6.2 Calibration cadence
Inherit Corpus 1 §6.2 for PinPoint. **Additionally:** re-zero HackMotion whenever PinPoint is
recalibrated, and after any sensor re-don. Both devices' calibration state must be current for a paired
swing to count.

### 6.3 Repeatability regimes
Inherit Corpus 1 §6.3 (Regime A fixed-calibration; Regime B re-mount). In Regime B, **re-don and
re-zero both devices** between sets — this also measures HackMotion's own re-mount variance.

### 6.4 Co-mount perturbation check (new, mandatory)
After donning **both** devices, re-run PinPoint **Confirm Tracking**. If the 3D model now tracks worse
than before HackMotion was added, the HackMotion sensor has shifted the hand IMU → **re-seat and recalibrate**.
A paired swing captured with a perturbed mount is invalid (it would attribute a mounting artifact to a
device disagreement).

### 6.5 Temporal-sync action (new, mandatory)
The two devices keep **independent clocks**; at 45 m/s, 1 ms = 45 mm. At the start of **every capture
block**, perform the defined sync action (e.g. 3 sharp deliberate wrist snaps). This creates a sharp,
co-observed feature for clock cross-correlation (reference §6.3). The fallback anchor is impact, but the
explicit sync action is more robust on partial/abandoned swings. **Record that the sync action was
performed** in Table B.

### 6.6 Static-pose anchors (new, once per session)
Capture, with **both** devices, six held static poses (≈ 3–5 s each): **cupped, bowed, ulnar, radial,
pronated, supinated** (lead wrist/forearm). These give an **absolute per-axis anchor independent of swing
dynamics** — they isolate sign and frame alignment from segmentation/timing error, and are the
independent witness that prevents locking a sign to a possibly mis-zeroed HackMotion (reference §6.2.4,
§6.6.1). Store as a labelled static-pose record per session.

---

## 7. Data collection protocol

### 7.1 Paired swings
Every Corpus-2 swing is **paired**: PinPoint and HackMotion record the *same shot simultaneously*. A swing
where one device dropped out is **not** a paired swing — recapture or relegate it to the Corpus-1-style
robustness set.

### 7.2 Full vs incomplete
As Corpus 1 §7.1 — full swings dominate; a small partial-swing set tests graceful degradation. Partials
are still useful here for the **temporal-sync robustness** check (does the sync hold without an impact?).

### 7.3 Variants & scripted faults
Inherit Corpus 1 §7.2–7.3. **Scripted faults double in value here:** cup/bow/cast/flip not only provide
known-groups evidence but **widen the angle range over which agreement is measured**, so LoA is not
estimated only near neutral (a common method-comparison pitfall). Aim to span the clinical ROM, not just
the easy middle.

### 7.4 Reconciliation (the three problems — do this in tooling, before any statistic)
Per reference §6.3, **log the resolution as data**:
1. **Sign:** HackMotion extension-positive → ×(−1) to PinPoint flexion-positive (FE); RUD/PS per
   `wrist_assessment.md` §7. A *remaining* sign disagreement after this is a real PinPoint sign bug.
2. **Zero/neutral:** compare **Δ-from-address** on both, never absolutes (cancels the differing zeros). A
   constant residual Δ-offset ⇒ anatomical `A·M` mis-alignment.
3. **Temporal:** cross-correlate the two FE traces (using the §6.5 sync action) to a sub-frame lag.

### 7.5 The single- vs two-sensor caveat (PS)
HackMotion *infers* forearm rotation from one glove sensor; PinPoint *measures* it with a dedicated
forearm + hand pair. A PS disagreement may be a **genuine construct difference**, not a PinPoint error —
**interpret, do not auto-tune to erase it** (reference §6.7). Flag PS conclusions accordingly.

---

## 8. Sampling plan & stratification (TARGET — fill in §13)

| Stratum | Swing type | Pairing | Calibration regime | Target n (paired) | Sparse markup | Dense markup |
|---|---|---|---|---|---|---|
| Repeatability A (noise) | full | paired | fixed within set | **25** | 5 | — |
| Repeatability B (re-mount) | full | paired | recal both between sets | **8** | 2 | — |
| Diversity (club × tempo, wide ROM) | full | paired | per-session | **12** | 5 | 2 |
| Stress / extreme ROM (range-widening) | full | paired | per-session | **5** | 2 | 1 |
| **Core paired total** | full | paired | — | **50** | **~14** | **~3** |
| Static-pose anchors | held poses | paired | per-session | 6 poses × ≥ 2 sessions | — | — |
| Partial (sync robustness) | partial | paired | per-session | 6 | — | — |
| Scripted faults | full + control | paired | per-session | ≥ 10–15 / priority fault | flag | — |

*Confirm the 50 against the pilot `s` per axis before committing studio time (§3).*

---

## 9. Markup & annotation plan

- **`truth.json` (hand labels)** — inherit Corpus 1 §9: sparse P-positions on 10–15 paired swings, dense
  on 2–3, inter-rater re-label on ~5, **labeller blinded** to analyzer output. (Hand labels remain
  independent of HackMotion — they validate the *vision/track* path, HackMotion validates the *wrist
  angles*; keep them separate streams of evidence.)
- **`hackmotion.json`** — generated by **export + reconcile (§7.4)**, not hand-marked. The labelling-team
  task here is a **pairing & sync QC step**, not annotation: confirm each paired swing's HackMotion export
  is matched to the correct PinPoint swing dir, the sync action is present, and the reconcile transform is
  stable. Record in Table C.

---

## 10. Data management, naming & provenance

As Corpus 1 §10, with corpus root `<SwingData>/corpus-2/`. **`CORPUS.md` additionally records:**
HackMotion firmware + export version, the co-mount order, the sync-action definition, the per-session
static-pose record, and the **per-axis reconciliation transform** applied. Partition (Tune/Validation/
Held-out) at blessing as before.

---

## 11. Quality control & acceptance gates (per paired swing)

Inherit Corpus 1 §11 (Tier-0 MUST gates), **plus the paired-capture gates:**

**MUST (additional):**
- [ ] HackMotion recorded the *same shot* (paired); export matched to the swing dir.
- [ ] Sync action present and cross-correlation succeeded (lag resolved).
- [ ] Co-mount perturbation check passed (PinPoint tracking unaffected by the glove sensor).
- [ ] HackMotion zeroed this session; reconciliation transform applied and **stable** (sign flip only).

**SHOULD:**
- [ ] Static-pose anchors captured this session.
- [ ] Angle range spans beyond neutral (range-widening swings present).

---

## 12. Statistical analysis (this corpus's contribution)

Full plan in reference §6.5. Corpus 2 produces, **per axis × per phase (Address/Top/Impact) × trajectory:**
**Bland–Altman** bias + 95 % LoA; **ICC(A,1)** (absolute agreement); RMSE; max error; trajectory Pearson r,
per-sample RMSE, DTW + warp-path; and both devices' **test–retest ICC/SEM/MDC** from the repeatability
sub-stratum. Report as a **family** (all axes/phases tabulated, no cherry-picking). Then the refinements
(reference §6.6): **lock signs**, **diagnose `A·M`** from constant bias / cross-axis leakage, **re-seat
`kWristBands`** (escalation-class C++ change, diff-gated).

---

## 13. Completion tracking tables — *fill in as you progress*

### Table A — Paired-capture progress tracker

| Stratum | Target n | Paired captured | Passed QC (Tier-0 + paired) | % complete | Notes |
|---|---|---|---|---|---|
| Repeatability A | 25 | | | | |
| Repeatability B | 8 | | | | |
| Diversity | 12 | | | | |
| Stress / range-widening | 5 | | | | |
| **Core paired** | **50** | | | | confirm vs pilot `s` |
| Static-pose sessions | ≥ 2 | | | | 6 poses each |
| Partial (sync robustness) | 6 | | | | |
| Faults (per priority) | 10–15 | | | | + control |
| Sparse markup | 10–15 | | | | blinded |
| Dense markup | 2–3 | | | | |
| Inter-rater re-label | 5 | | | | |

### Table B — Session log

| Session | Date | Golfer | Sensors (2/3) | PinPoint calib | HackMotion zero | Perturbation check | Sync action | Static poses | Paired swings | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| S01 | | | | | | | | | | |

### Table C — Per-paired-swing QC ledger

| Swing ID | HackMotion file matched? | Sync OK (lag ms) | Stratum | Full/Partial | Variant | Tier-0 PASS? | Reconcile stable? | Markup | Partition (T/V/H) | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| sw_0001 | | | | | | | | | | |

### Table D — Agreement roll-up (complete at freeze; per axis × phase)

| Axis | Phase | n | Bias (°) | 95 % LoA (°) | ICC(A,1) | RMSE (°) | Traj r | Lag (frames) | Pass? |
|---|---|---|---|---|---|---|---|---|---|
| FE | Address | | | | | | | | |
| FE | Top | | | | | | | | |
| FE | Impact | | | | | | | | |
| RUD | Address | | | | | | | | |
| RUD | Top | | | | | | | | |
| RUD | Impact | | | | | | | | |
| PS | Address | | | | | | | | |
| PS | Top | | | | | | | | |
| PS | Impact | | | | | | | | |

### Table E — Reliability denominator (both devices)

| Device | Axis | Test–retest ICC | SEM (°) | MDC₉₅ (°) |
|---|---|---|---|---|
| PinPoint | FE / RUD / PS | | | |
| HackMotion | FE / RUD / PS | | | |

### Table F — Refinement outcomes

| Refinement | Finding | Action taken | Diff-gate regressions |
|---|---|---|---|
| Sign lock (FE/RUD/PS) | | | |
| `A·M` bias diagnosis | | | |
| `kWristBands` re-seat | | | |
| Gimbal threshold check | | | |

---

## 14. Deviations & risk log

As Corpus 1 §15. Additionally log: any swing where the sync failed, the perturbation check failed, or the
reconciliation transform was unstable across the session (and the disposition).

---

## 15. Roles & sign-off

As Corpus 1 §16, plus a **HackMotion data steward** responsible for export–swing pairing, the reconcile
transform, and Tables C–F.

---

## References

As Corpus 1, plus: Bland JM, Altman DG. *Measuring agreement in method comparison studies.* Stat Methods
Med Res 1999. · Concurrent-validity reviews of wrist IMU vs goniometry (ICC 0.95–0.99; RUD weakest) — see
`docs/reference/wristmetrics.md`. · HackMotion target ranges (lead-wrist bands relative to address).
