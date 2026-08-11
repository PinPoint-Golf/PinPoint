# Wrist-cock model fit

- run root `/mnt/swingdata/stagegate/corpm3-off` · sha `c412703` · domain `to-impact`
- 59 swings, 23 with hand-placed shaft labels (805 labels)
- every number leave-one-swing-out: the held-out swing never fits the table it is scored against

## Against independent truth (the number the decision rests on)

| form | n | median | p10–p90 | \|err\|>30° | within 1σ |
|---|---|---|---|---|---|
| F0 shipped table (hand-authored) | 471 | -12.2° | **77.6°** | 37.4% | 50% |
| F1 refit on the shipped progress axis | 471 | +5.4° | **76.5°** | 38.0% | 58% |
| F2 refit on seconds-before-impact | 471 | -4.1° | **37.9°** | 10.4% | 58% |
| F3 + linear in phi | 471 | -2.8° | **32.4°** | 9.1% | 59% |
| F4 + shape constraints (one reversal, in line at impact) | 471 | -3.1° | **34.5°** | 9.1% | 58% |
| F5 parametric: cock x release logistics (7 params) | 471 | -3.5° | **38.5°** | 10.0% | 56% |

## Against the tracker's own vision tier (broader, but circular)

| form | n | median | p10–p90 | \|err\|>30° | within 1σ |
|---|---|---|---|---|---|
| F0 shipped table (hand-authored) | 9154 | -6.6° | 78.2° | 31.2% | 51% |
| F1 refit on the shipped progress axis | 9154 | +6.4° | 82.9° | 27.6% | 63% |
| F2 refit on seconds-before-impact | 9154 | +0.2° | 49.9° | 18.0% | 60% |
| F3 + linear in phi | 9154 | -0.6° | 44.5° | 17.1% | 56% |
| F4 + shape constraints (one reversal, in line at impact) | 9154 | -0.5° | 44.6° | 17.0% | 55% |
| F5 parametric: cock x release logistics (7 params) | 9154 | -1.4° | 42.3° | 16.3% | 56% |

## Grading by label provenance

- fusion phi source: `harness` · tiers `band,ray` · holdout `session`
- of the corpus labels on lab-covered swings, **565 are the instrumented band tier verbatim** and 61 are genuinely hand-placed
- `fuse_ray` is the material the corpus never had: the fast frames a human cannot label
- every truth column is printed beside `track` (frame-averaged), never alone

| form | track (frame-avg) | truth_hand | truth_band | fuse_all | fuse_band | fuse_ray |
|---|---|---|---|---|---|---|
| F0 | 78.2° (n=9154) | 59.5° (n=44) | 36.7° (n=307) | 43.1° (n=590) | 37.0° (n=307) | 50.9° (n=283) |
| F1 | 82.9° (n=9154) | 55.4° (n=44) | 69.4° (n=307) | 66.3° (n=590) | 69.5° (n=307) | 60.6° (n=283) |
| F2 | 49.9° (n=9154) | 33.9° (n=44) | 12.8° (n=307) | 17.4° (n=590) | 12.9° (n=307) | 21.3° (n=283) |
| F3 | 44.5° (n=9154) | 35.1° (n=44) | 21.0° (n=307) | 22.8° (n=590) | 21.0° (n=307) | 23.1° (n=283) |
| F4 | 44.6° (n=9154) | 35.3° (n=44) | 21.2° (n=307) | 22.3° (n=590) | 21.2° (n=307) | 23.1° (n=283) |
| F5 | 42.3° (n=9154) | 22.8° (n=44) | 17.1° (n=307) | 21.0° (n=590) | 17.1° (n=307) | 24.9° (n=283) |

## By phase segment (tracker tier, p10–p90)

| form | backswing | downswing | through |
|---|---|---|---|
| F0 | 66.7° | 139.4° | — |
| F1 | 74.2° | 133.6° | — |
| F2 | 34.1° | 175.4° | — |
| F3 | 32.2° | 182.9° | — |
| F4 | 32.6° | 182.6° | — |
| F5 | 31.3° | 180.5° | — |

## By session (tracker tier, p10–p90)

| form | 2026-06-11_Mark-Liversedge_Wrist_01 | 2026-07-03_Mark-Liversedge_Wrist_01 | 2026-07-04_Mark-Liversedge_Wrist_01 | 2026-07-05_Mark-Liversedge_Wrist_02 | 2026-07-08_Mark-Liversedge_Wrist_01 | 2026-07-09_Mark-Liversedge_Wrist_01 |
|---|---|---|---|---|---|---|
| F0 | 266.5° | 72.5° | 244.4° | 61.4° | 42.4° | 37.6° |
| F1 | 244.2° | 76.5° | 252.4° | 61.6° | 33.4° | 28.5° |
| F2 | 246.7° | 49.4° | 131.4° | 33.0° | 27.3° | 16.6° |
| F3 | 236.6° | 52.1° | 137.5° | 33.7° | 26.9° | 17.2° |
| F4 | 237.1° | 51.3° | 137.7° | 33.9° | 26.6° | 17.9° |
| F5 | 245.4° | 54.0° | 137.3° | 18.6° | 25.6° | 21.0° |
