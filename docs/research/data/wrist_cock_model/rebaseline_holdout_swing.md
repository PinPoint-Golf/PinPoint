# Wrist-cock model fit

- run root `/mnt/swingdata/stagegate/corpm3-off` · sha `c412703` · domain `to-impact`
- 59 swings, 23 with hand-placed shaft labels (805 labels)
- every number leave-one-swing-out: the held-out swing never fits the table it is scored against

## Against independent truth (the number the decision rests on)

| form | n | median | p10–p90 | \|err\|>30° | within 1σ |
|---|---|---|---|---|---|
| F0 shipped table (hand-authored) | 471 | -12.2° | **77.6°** | 37.4% | 50% |
| F1 refit on the shipped progress axis | 471 | +1.4° | **62.7°** | 32.5% | 66% |
| F2 refit on seconds-before-impact | 471 | -2.5° | **26.8°** | 9.6% | 70% |
| F3 + linear in phi | 471 | -1.6° | **25.7°** | 8.3% | 66% |
| F4 + shape constraints (one reversal, in line at impact) | 471 | -1.7° | **26.6°** | 8.3% | 65% |
| F5 parametric: cock x release logistics (7 params) | 471 | -3.3° | **38.2°** | 10.0% | 54% |

## Against the tracker's own vision tier (broader, but circular)

| form | n | median | p10–p90 | \|err\|>30° | within 1σ |
|---|---|---|---|---|---|
| F0 shipped table (hand-authored) | 9154 | -6.6° | 78.2° | 31.2% | 51% |
| F1 refit on the shipped progress axis | 9154 | +5.6° | 72.7° | 24.3% | 68% |
| F2 refit on seconds-before-impact | 9154 | -0.0° | 40.0° | 16.5% | 62% |
| F3 + linear in phi | 9154 | -1.1° | 39.1° | 15.8% | 62% |
| F4 + shape constraints (one reversal, in line at impact) | 9154 | -1.0° | 39.6° | 15.8% | 60% |
| F5 parametric: cock x release logistics (7 params) | 9154 | -0.8° | 40.2° | 15.7% | 54% |

## Grading by label provenance

- fusion phi source: `harness` · tiers `band,ray` · holdout `swing`
- of the corpus labels on lab-covered swings, **565 are the instrumented band tier verbatim** and 61 are genuinely hand-placed
- `fuse_ray` is the material the corpus never had: the fast frames a human cannot label
- every truth column is printed beside `track` (frame-averaged), never alone

| form | track (frame-avg) | truth_hand | truth_band | fuse_all | fuse_band | fuse_ray |
|---|---|---|---|---|---|---|
| F0 | 78.2° (n=9154) | 59.5° (n=44) | 36.7° (n=307) | 43.1° (n=590) | 37.0° (n=307) | 50.9° (n=283) |
| F1 | 72.7° (n=9154) | 46.4° (n=44) | 52.1° (n=307) | 51.3° (n=590) | 52.2° (n=307) | 47.3° (n=283) |
| F2 | 40.0° (n=9154) | 15.3° (n=44) | 12.1° (n=307) | 16.7° (n=590) | 12.2° (n=307) | 20.5° (n=283) |
| F3 | 39.1° (n=9154) | 21.3° (n=44) | 16.1° (n=307) | 20.5° (n=590) | 16.1° (n=307) | 23.9° (n=283) |
| F4 | 39.6° (n=9154) | 23.1° (n=44) | 17.1° (n=307) | 20.1° (n=590) | 17.1° (n=307) | 22.7° (n=283) |
| F5 | 40.2° (n=9154) | 22.8° (n=44) | 16.8° (n=307) | 20.7° (n=590) | 16.8° (n=307) | 24.7° (n=283) |

## By phase segment (tracker tier, p10–p90)

| form | backswing | downswing | through |
|---|---|---|---|
| F0 | 66.7° | 139.4° | — |
| F1 | 64.0° | 127.4° | — |
| F2 | 27.2° | 172.7° | — |
| F3 | 26.3° | 176.3° | — |
| F4 | 27.1° | 176.1° | — |
| F5 | 29.6° | 179.4° | — |

## By session (tracker tier, p10–p90)

| form | 2026-06-11_Mark-Liversedge_Wrist_01 | 2026-07-03_Mark-Liversedge_Wrist_01 | 2026-07-04_Mark-Liversedge_Wrist_01 | 2026-07-05_Mark-Liversedge_Wrist_02 | 2026-07-08_Mark-Liversedge_Wrist_01 | 2026-07-09_Mark-Liversedge_Wrist_01 |
|---|---|---|---|---|---|---|
| F0 | 266.5° | 72.5° | 244.4° | 61.4° | 42.4° | 37.6° |
| F1 | 239.2° | 69.5° | 251.0° | 48.7° | 30.7° | 27.5° |
| F2 | 247.5° | 46.0° | 138.9° | 15.7° | 23.5° | 15.4° |
| F3 | 212.8° | 47.5° | 138.8° | 18.1° | 22.9° | 15.8° |
| F4 | 213.0° | 47.1° | 138.9° | 18.7° | 23.3° | 16.2° |
| F5 | 246.0° | 52.8° | 138.1° | 18.1° | 23.9° | 20.2° |
