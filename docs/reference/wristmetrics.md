# Wrist & forearm metrics — measurement, sign, names, norms

Reference for the three lead-arm joint motions PinPoint derives from arm-mounted IMUs:
how they're measured and **signed**, the **user-facing names** we use, and the
**normative data** we score against. Distilled from a multi-source literature + product
review; citations at the end. Where the evidence is thin (especially quartiles), this
says so plainly.

> **Status:** decisions below are codified in `wrist_angles.h` (axis math + sign note),
> `metric_extractor.cpp` (labels), `wrist_analyzer.cpp` (value descriptors), and
> `swing_scorer.cpp` (reference bands). The *absolute signs* are still verified per-rig on
> the wizard "check your sensors" page — see [[imu-wrist-rig-coplanar]] context.

## The three motions

| Clinical axis | ISB axis (Wu et al. 2005) | PinPoint user-facing name | Value descriptor |
|---|---|---|---|
| Wrist flexion / extension | proximal segment **Z** (Zi) | **bow / cup** | `+N° bowed` · `flat` · `N° cupped` |
| Wrist radial / ulnar deviation | floating **e2** axis | **hinge** | `N° ulnar` · `neutral` · `N° radial` |
| Forearm pronation / supination | distal segment **Y** (Yj) | **roll** | `N° pronated` · `square` · `N° supinated` |

(`leadArmFlexion` = elbow flexion, reported as a magnitude in degrees.)

## (a) Measurement & sign convention

The canonical standard is **ISB / Wu et al. 2005** — `ref.wu2005` in the model reference
pack — built on the **Grood & Suntay (1983)** joint-coordinate system. Signs (ISB, high
confidence):

> Wu G, van der Helm FCT, Veeger HEJ, Makhsous M, Van Roy P, Anglin C, Nagels J, Karduna AR,
> McQuade K, Wang X, Werner FW, Buchholz B. *ISB recommendation on definitions of joint
> coordinate systems of various joints for the reporting of human joint motion — Part II:
> shoulder, elbow, wrist and hand.* J Biomech 2005;38(5):981–992.
> doi:[10.1016/j.jbiomech.2004.05.042](https://doi.org/10.1016/j.jbiomech.2004.05.042) ·
> PMID 15844264

**This is a society standard, not one laboratory's preference**, and that distinction is the
reason it wins here. It is the recommendation of the **Standardization and Terminology
Committee of the International Society of Biomechanics**, twelve authors across institutions
in five countries, [published by the society itself](https://media.isbweb.org/images/documents/standards/Wu%20et%20al%20J%20Biomech%2038%20(2005)%20981%E2%80%93992.pdf)
under its standards collection, and the companion to the Part I recommendation covering
ankle, hip and spine (Wu et al. 2002, PMID 11934426).

- **Flexion +**, extension − (about proximal Z).
- **Ulnar +**, radial − (floating e2 axis).
- **Pronation +**, supination − (about distal Y).
- In the anatomical position the Z-axis points **ulnarly**, so flexion, ulnar deviation and
  pronation are **all positive for both left and right arms**.
- **Neutral / zero:** wrist neutral when the 3rd-metacarpal long axis is parallel to the
  radius Yi axis; forearm zero ≈ elbow flexed 90° with the thumb to the shoulder. *This is
  what an IMU calibration zeroes against.*

**PinPoint's chosen convention** = ISB signs, mapped to coaching language:

| Motion | Positive (good-side cue) | Negative |
|---|---|---|
| bow / cup (flex/ext) | **bowed** (flexion — desired at impact) | cupped (extension) |
| hinge (rad/ulnar) | **ulnar** (wrist cock/hinge) | radial |
| roll (pron/sup) | **pronated** | supinated |

This agrees with both ISB (flexion +) and golf coaching (a *bowed* lead wrist at impact is
the goal → positive). **Caveats that bit us and bite the literature:**

- Most golf studies (≈10 of 92 reviewed) do **not** follow ISB; some local-frame studies
  define **radial deviation as positive** — i.e. signs invert between sources. Don't compare
  raw signs across papers without checking the frame.
- **Commercial wrist sensors invert bow/cup against us, and this is the one to expect.** At
  least one widely used device reports **extension (cupping) as positive and flexion (bowing)
  as negative** — the exact inverse of ISB and of us. A golfer or coach moving between the two
  and comparing raw signs reads every wrist as its own opposite. The rule is that **the
  published standard wins over the popular product**: we stay ISB-compliant and record the
  disagreement rather than adopting a vendor convention, because a vendor can change theirs and
  a standard is the thing that lets two datasets be compared at all. What both agree on is the
  *principle* — the sign means the same thing whichever way the golfer plays — and ISB delivers
  it outright, flexion being positive for **both** the left and the right arm.
- ISB itself is hard to implement in vivo (cadaveric/imaging landmarks), which is why
  IMU/coaching systems use functional calibration instead.
- The decomposition math (`wrist_angles.h`) is sign-*consistent*; the *physical* sign of
  each channel is confirmed against a known cup/bow pose on the "check your sensors" page.

## (b) User-friendly names

Coaching/consumer terms mapped to the clinical axes (medium confidence — synthesis):

- **bow / cup** = flexion / extension. *Bowed* = flexion, *cupped* = extension (lead wrist),
  *flat* ≈ neutral.
- **hinge** (wrist cock) = ulnar deviation.
- **roll** = forearm pronation / supination.

**Lead vs trail wrists differ statistically across all swing phases** (verified) — never
treat them interchangeably; PinPoint scores the **lead** wrist only.

## (c) Normative data — and the quartile gap

**There is no published quartile/percentile benchmark dataset** — not in the peer-reviewed
literature and not public from HackMotion, DeWiz, GEARS, K-Motion or Sportsbox AI (all
measure these same three axes). That's a real finding, not a search miss. What *does* exist:

- **Handicap benchmark (radial/ulnar):** high-handicap golfers (HCP ≥10) show **more lead
  radial deviation** than low-handicap (≤5) — **+5.7° at peak (p=0.008), +7.1° at ball
  contact (p=0.001)**; absolute at impact ≈ **8° (low HCP) vs 15° (high HCP)**
  (Buchanan/Zhang, *J Sci Med Sport* 2012). → *less* deviation is better.
- **What matters most (flex/ext):** wrist **flexion/extension drives clubhead speed more
  than radial/ulnar** — restricting flexion cut clubhead speed ~46%; restricting ulnar
  deviation did nothing (Sweeney et al., forward-kinematic model; medium confidence). →
  **weight flex/ext highest.**
- **Plausibility bounds (clinical ROM):** functional max **60° ext, 54° flex, 40° ulnar,
  17° radial** (Ryu et al. 1991). → sanity clamps for reported values.
- **IMU validity:** ICC **0.95–0.99** vs goniometry (~1° error) — **but radial/ulnar is the
  weakest IMU axis** (~5° mean error, "use with caution"), and validation was slow static
  ROM, **not** swing-speed dynamics. → trust flex/ext + roll over hinge.

### Scoring bands codified (provisional)

`swing_scorer.cpp` reference table for Wrist (deadband + falloff, weighted geometric mean):

| Metric | mu | sigma | one-sided | weight | rationale |
|---|---|---|---|---|---|
| `leadWristFlexExt` (bow/cup) | 15 | 12 | penalise below (cupping) | **0.45** | FE drives clubhead speed (Sweeney) — highest weight |
| `leadWristRadUln` (hinge) | 0 | 12 | two-sided | 0.15 | weak IMU axis + secondary; Buchanan/Zhang: high-HCP ~7° more deviation — refine once signs locked |
| `forearmPronation` (roll) | 0 | 25 | two-sided | 0.20 | no published benchmark; square-ish through impact |
| `leadArmFlexion` (elbow) | 5 | 12 | penalise above (bent) | 0.20 | near-straight lead arm |

All values are **provisional** until the FE/hinge/roll signs are confirmed on the
"check your sensors" page; the math and aggregation are correct regardless.

## Sources

- **ISB standard:** Wu G. et al. (2005) *ISB recommendation on definitions of joint
  coordinate systems… Part II: shoulder, elbow, wrist and hand.* J Biomech 38(5):981–992,
  doi:10.1016/j.jbiomech.2004.05.042 · ISB standards page `isbweb.org/standards/wrist.html`
  · Werner & Buchholz wrist proposal.
- **JCS method:** Grood & Suntay (1983).
- **Handicap benchmark:** Buchanan/Zhang, *J Sci Med Sport* (2012) — lead-arm radial
  deviation vs handicap.
- **Speed contribution:** Sweeney et al. — forward-kinematic model, flexion vs clubhead speed.
- **Functional ROM:** Ryu J. et al. (1991) — functional wrist ROM for daily tasks.
- **IMU validity:** concurrent-validity reviews vs goniometry (ICC 0.95–0.99; rad/ulnar weakest).
- **Products surveyed:** HackMotion, DeWiz, GEARS, K-Motion, Sportsbox AI — all measure the
  three axes; none publish a citable sign-convention or quartile benchmark dataset.
