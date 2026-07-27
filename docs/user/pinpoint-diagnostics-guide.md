# Pinpoint — Understanding and Changing the Diagnostics

**Who this is for:** anyone who wants to know how Pinpoint decides that something in a swing is worth
mentioning — and how to change its mind. No technical background assumed, and no golf coaching
background assumed either. Every term is explained the first time it appears.

**How to read it:** sections 1–4 explain how it works, ending with two worked examples. Section 5 says
where everything lives in the app. Sections 6–8 are how to change things, what to be careful of, and
what the app's own warnings mean. There is a glossary at the end.

> **⚠ What ships today, and what does not**
>
> Everything in this guide about **how the numbers are stored, shown and edited** is live in the app
> right now: the metric catalogue, the corridors, the editors, the health list.
>
> The **automatic per-shot findings** — the app watching a swing and saying "this is scooping, and
> here is what usually causes it" — are **not switched on yet**. The knowledge is all in place and the
> screens that let you inspect and edit it work, but nothing is yet feeding real shots through it. So
> if you read Section 3 and go looking for a list of findings after a swing, you will not find one.
> That is the next piece of work, not a fault.

---

## 1. The idea, in plain language

Pinpoint measures things about your swing, compares each measurement against **what is normal for
that kind of shot**, and only says something when a measurement is genuinely unusual.

That is the whole idea. The rest of this section is the vocabulary, because each step in that sentence
has a name in the app, and you need the names to find anything.

Here is the chain, and then each link explained:

```
   a metric          →   a measure        →   a norm         →   a grade
   (what we can          (one specific        (what normal       (how unusual
    measure)              reading of it)       looks like)        this reading is)
                                                                      │
                                                                      ▼
   a characteristic   ←   a signal        ←────────────────────  Watch or Action?
   (something worth       (a rule that
    telling you)           watches for it)
        │
        ▼
   its causes  (what tends to produce it, and what it tends to produce)
```

### A metric — something we can measure

A **metric** is a quantity the software can extract from a swing. "Ball position in the stance."
"Lead wrist bow/cup." "Clubhead speed."

A metric on its own is just a name and a unit. It does not know what a good value is.

> **Golf term: "lead" and "trail".** Your *lead* side is the side nearer the target — for a
> right-handed player, the left arm, hand and foot. Your *trail* side is the other one. The app always
> says lead/trail rather than left/right, so it reads correctly for both handednesses.

### A measure — one specific reading of a metric

Here is the first thing that surprises people. A metric like "lead wrist bow/cup" is not one number —
it is a number that **changes continuously** through the swing. So to compare it against anything, you
first have to say *exactly which reading you mean*.

A **measure** is that decision. It names the metric plus how to reduce it to a single number:

| The measure says | Meaning |
|---|---|
| **at P7** | the value at impact |
| **change from P1 to P4** | how much it moved from address to the top of the backswing |
| **highest between P5 and P6** | the peak somewhere in that stretch |

> **Golf term: P1 to P9.** A standard way of naming eight or nine moments in a swing, so that
> everyone means the same instant. The ones you will meet most:
> **P1 = address** (set up, before you move), **P4 = the top of the backswing**,
> **P7 = impact** (the moment you strike the ball), **P8/P9 = after impact**.

**This distinction matters more than it looks.** "Lead wrist at impact" and "how much the lead wrist
changed between address and impact" are two different numbers, about the same part of the body, and a
good value for one is not a good value for the other. In the app they are two separate measures, each
with its own idea of normal. Section 4 works through exactly this case.

### A norm — what normal looks like

A **norm** is the app's description of a normal range for one measure. It is also called a
**corridor**, because of how it is drawn on screen: a band you would expect most people's readings to
fall inside.

A norm holds:

- a **centre** — the typical value;
- a **tolerance** either side — how far from the centre is still unremarkable. The two sides can
  differ, and often should: for ball position, being a little forward is far more common and far less
  interesting than being a long way back.

**A norm describes a population, never a person.** It is "what golfers' readings look like", not "your
target". There is no personal best hiding in here. That matters when you start editing them
(Section 7).

### A grade — how unusual this reading is

Given a reading and a norm, the app grades it in four bands:

| Grade | Meaning in plain words |
|---|---|
| **Ideal** | right in the middle of normal |
| **Good** | comfortably normal |
| **Watch** | unusual enough to be worth noticing |
| **Action** | far enough out that it is very likely a real feature of your swing |

There is a fifth state, **Not measured**, which means the app could not get a reading at all — no
camera, no sensor, or the swing could not be broken into positions confidently. **Not measured never
means "fine".** It means we do not know, and the app is careful to keep those two apart.

### A signal — the rule that watches

A **signal** is a small rule attached to a measure. The commonest kind says: *"tell me when this
reading is unusual, on this particular side."*

The side matters. Ball position being too far forward and too far back are different problems with
different consequences, so they are two separate signals on the same measure, each watching one
direction.

**A signal fires on Watch or Action — not merely on leaving Ideal.** This is deliberate and it is
worth understanding: "Ideal" is the middle of normal, so a large fraction of perfectly ordinary
golfers sit outside it on any given measurement. If the app spoke up every time, it would flag
something on nearly every swing and you would stop reading it.

### A characteristic — something worth telling you about

A **characteristic** is a named feature of a swing, in the language a coach would use: *Scooping.*
*Ball too far back.* *Loss of posture.* When a signal fires, its characteristic becomes a **finding**
for that swing.

A characteristic carries more than a name. It carries the **consequence** — why it matters. For
example, for *Scooping*:

> Adding loft through impact with the lead wrist means the club is rising at the ball, costing
> compression and distance control.

### Causes — how characteristics connect

Characteristics are linked to each other, because swings are not lists of independent faults. One
thing produces another.

*Scooping*, for instance, is recorded as being caused by *casting*, by *trying to lift the ball*, and
by *hanging back* — each link labelled with how strong the relationship is. Some causes are
**latent**: they cannot be seen in the swing directly, only inferred from the things they explain.

This is what makes the model useful rather than just a list of complaints. Several visible
characteristics often trace back to one underlying cause — and that one cause is the thing worth
working on.

---

## 2. Shot type: context

The same reading can be normal for one shot and odd for another. A driver is played well forward in
the stance; a wedge is played near the middle. One "normal ball position" for both would be wrong for
both.

So norms are organised by **shot type**, called a **context** in the app, arranged as a family tree:

```
Any shot                            ← the general case lives here
├── Full swing
│   ├── Driver
│   ├── Fairway wood
│   ├── Iron
│   ├── Wedge
│   ├── Bowed top      (a style, not a club — see Section 4)
│   └── Cupped top
├── Partial swing
│   ├── Pitch
│   └── Chip
├── Bunker
└── Specialty
```

**How a norm is found.** The app starts at the shot's own context and walks *upward* until it finds a
norm for that measure. The nearest one wins. So for an iron shot it looks at *Iron* first, then *Full
swing*, then *Any shot*.

Two consequences worth holding on to:

1. **You only author what is different.** If a measure behaves the same whatever club is in hand, one
   norm at *Any shot* covers everything. Add a *Driver* norm only when the driver genuinely differs.
2. **The general case belongs at *Any shot*, not at *Full swing*.** A full swing is a *type* of shot,
   with no more claim to being the default than a pitch has. If you put a general norm on *Full swing*
   instead, a pitch or a bunker shot cannot see it — those branches sit beside *Full swing*, not under
   it — and every reading on such a shot comes back *Not measured*.

That second point is a real trap and the app now watches for it: the health list (Section 8) reports
any context that nothing grades.

**Shot type informs a diagnosis. It never silences one.** Pinpoint will not decide that a
characteristic is irrelevant to your bunker shot and quietly skip it. It measures, grades and reports;
whether the result matters for the shot you were playing is your call, not the software's.

---

## 3. Worked example: ball position

The simplest end-to-end case, and the one where shot type does real work.

### The scale

Ball position is measured as a **percentage across your stance**:

- **0 %** = level with your **lead** heel (the foot nearer the target)
- **100 %** = level with your **trail** heel
- **50 %** = the middle of your stance

A *higher* number means the ball is *further back*, toward the trail foot. (A driver is sometimes
played slightly ahead of the lead heel, which reads as a small negative number. That is a real setup,
not an error.)

This is read at **P1 — address**, before you move. The measure is therefore *"Ball position in the
stance, at P1"*.

### The norms that ship

| Context | Centre | Tolerance | Ideal band |
|---|---|---|---|
| **Any shot** | 30 % | ± 14 | 16 % – 44 % |
| **Driver** | 5 % | ± 8 | −3 % – 13 % |
| **Fairway wood** | 18 % | ± 9 | 9 % – 27 % |
| **Iron** | 33 % | ± 10 | 23 % – 43 % |
| **Wedge** | 50 % | ± 10 | 40 % – 60 % |

Read that table as golf, not arithmetic: the driver is played off the lead heel, the wedge in the
middle, irons a little back of centre — and if the app does not know which club you used, it falls
back to a wide general band.

### Grading a reading

Take an **iron** shot with the ball at **50 %**.

The app finds the *Iron* norm: centre 33, tolerance 10. The reading is 17 above centre, which is 1.7
tolerances out. Under the standard setting that is **Good** — noticeably back of typical, but not
unusual enough to be a finding. Nothing is reported.

Now the same 50 % on a **wedge**. Centre 50, tolerance 10 — dead centre. **Ideal.**

The identical measurement, graded differently, correctly, because the shot was different. That is the
context tree earning its place.

Push the iron further back, to **60 %**: now 2.7 tolerances out, which is **Watch**. The signal
watching the "too far back" side fires, and the characteristic **Ball too far back** becomes a
finding, with its consequence:

> A ball played too far toward the trail foot is met early and steeply, which tends to close the
> shoulders at address to compensate.

### The two sides, and the causal links

Ball position has **two** characteristics, one per side — *Ball too far forward* and *Ball too far
back* — because they are different problems. They are recorded as two ends of one **axis**, so the app
knows they are a pair rather than two unrelated faults.

Each is also linked into the causal model:

- *Ball too far back* → tends to cause *closed shoulder alignment* (a moderate link). Setting up with
  the ball back encourages you to close your shoulders to compensate.
- *An open aim bias* → weakly tends to cause *Ball too far forward*.

So if the app reports both a ball-position finding and an alignment finding, the model already knows
they may be one story rather than two.

---

## 4. Worked example: lead wrist bow/cup

Harder, and worth the effort — it shows why "which reading" matters, and why two golfers can both be
right.

### What is being measured

Your lead wrist can be **bowed** (flexed — the back of the hand angled toward the forearm, the strong
"hands ahead" look) or **cupped** (extended — the back of the hand angled back). The app measures this
in degrees, with **positive = more bowed** and **negative = more cupped**.

It matters because this angle is the biggest single influence on where the clubface points. Cupping
tends to leave the face open; bowing tends to square or close it.

### Two measures, one axis

The wrist angle changes constantly through the swing, so the app carries **two different measures** of
it — and this is the part to slow down on:

| Measure | What it is | Its norm |
|---|---|---|
| **Lead wrist bow/cup — change from address at P4** | How much your wrist moved between address and the top of the backswing | centre **+5°**, tolerance ± 11 |
| **Lead wrist flexion/extension, at P7** | The actual angle at impact, not a change | centre **+15°**, tolerance ± 12 |

The first is a *change*; the second is an *absolute* reading. **+10° means two entirely different
things depending on which one you are looking at.** On the app's screens, a corridor that is a change
from address is labelled as such, precisely so the two cannot be confused.

### Grading impact

Impact norm: centre 15, tolerance 12, giving **Ideal from 3° to 27°**.

- A reading of **+15°** (nicely bowed at impact): **Ideal**.
- A reading of **−10°** (cupped at impact): 25 below centre, 2.1 tolerances out → **Watch**, on the
  low side.

That low side has a signal on it, and the characteristic it belongs to is **Scooping**:

> Adding loft through impact with the lead wrist means the club is rising at the ball, costing
> compression and distance control.

Note that the signal watches **only the low side**. A wrist *more* bowed than typical at impact is not
scooping — it is a different thing entirely, and it would be wrong for one signal to fire on both.

### Where it gets interesting: two valid styles

Some excellent players arrive at the top with a bowed wrist; others arrive cupped. Both win golf
tournaments. If the app graded everyone against one corridor, it would flag one of those two groups as
faulty on every single swing.

So *Bowed top* and *Cupped top* are **contexts** — shot "types" in the family tree, sitting under Full
swing — each with its own corridor for this one axis:

| Context | Centre of the change-to-the-top norm |
|---|---|
| Any shot (neutral) | +5° |
| **Bowed top** | **+15°** |
| **Cupped top** | **−5°** |

A player identified as bowed is graded against the bowed corridor. Every *other* measurement is
unaffected — only this axis shifts, because style only makes a difference here.

### The causal chain

*Scooping* is recorded as being caused by:

- **casting** (strong) — releasing the angle in the wrists too early in the downswing;
- **trying to lift the ball** (strong) — the instinct to help the ball into the air;
- **hanging back** (moderate) — weight staying on the trail foot through impact.

This is the payoff of the causal model. Told only "you are scooping", you would work on your hands.
The model points at three upstream candidates instead — and *trying to lift the ball* is an intention,
not a movement, which is a very different conversation.

---

## 4b. What the ball did — and what a launch monitor adds

The library now names the shots themselves, not only the swings that produce them: pull, push,
slice, hook, chunk, thin, top, sky, shank and the rest. **They are ordinary characteristics**, in a
group called Ball flight, which is what lets an explanation run all the way from a physical
restriction to the shot you actually saw — a tight lead hip causes early extension, early extension
causes the club to come from under, and that causes the block right.

Two things are worth understanding about them.

**Some are camera-work and some need a launch monitor.** Where the ball STARTS is something two
cameras can see: pulls, pushes, launch height, ball speed. How it CURVES develops over a flight an
indoor capture never sees, so slice, hook, spin and strike location need a launch monitor. Until one
is connected those read *"needs a launch monitor"* rather than going quietly blank — the same way a
metric says *"needs a face-on camera"* when a camera is missing. Launch-monitor integration is
planned; when it arrives those readings simply start working, with no change to the library.

**Some the app cannot yet tell apart, and says so.** A chunk is the ground struck before the ball
AND a collapse in ball speed — two readings at once — and the app currently judges one reading at a
time. So chunk, thin, top, sky and shank are marked as things only you or your coach can confirm.
They are still in the library, still carry their definitions, and still sit at the end of the causal
chains; the app simply does not claim to have seen one.

**Draw and fade are deliberately absent.** A draw is a hook you meant. The shape is identical and the
app has no way to know your intention, so it reports the curvature and leaves whether it was wanted
to you — the same reason nothing in this model calls a finding good or bad.

---

## 5. Where to find all this in the app

| What you want | Where to go |
|---|---|
| Every metric the app can measure, what it means, its corridors | **Settings → Metrics** |
| The corridors themselves, to inspect or edit | **Settings → Diagnostics → Measures & norms** |
| Characteristics, their consequences, their causes | **Settings → Diagnostics → Characteristics** |
| **What a coaching term means, in plain language** | **Settings → Diagnostics → Glossary** |
| **The physical tests, and the drills that answer a fault** | **Settings → Diagnostics → Screens & drills** |
| What is missing or wrong in the library | **Settings → Diagnostics → Causes & health** |
| What is not yet measurable, and why | **Settings → Diagnostics → Roadmap** |

**Search by the word you were taught.** Every characteristic carries the other names coaches use for
it, so *flip* finds Scooping, *OTT* finds Over the top, and *standing up* finds Early extension. That
works in the Characteristics list and in the Glossary, and the row tells you which term matched so
you can see the library understood you rather than guessed.

**A shortcut worth knowing.** On any metric's page in **Settings → Metrics**, each corridor is drawn
as a band with a coloured link underneath it naming the norm behind it — for example
*"Lead wrist bow / cup — change from address at P4 · Any shot →"*. Tapping it takes you straight to
that norm, where you can edit it. There is one link per corridor, because (as Section 4 showed)
different phases of one metric are different measures with different norms.

---

## 6. How to change things

### Change a corridor

**Settings → Diagnostics → Measures & norms**, choose the measure, then tap the norm row for the
context you want to change. The corridor editor opens.

It offers three routes:

1. **Drag the two handles** to set the band by eye.
2. **Type the numbers** if you have a figure in mind.
3. **Seat it from swings** — let the app fit the corridor to a set of stored swings you choose.

Underneath, it draws a **histogram**: every stored swing's reading for this measure, with your
corridor overlaid, and a running count of how many land in each grade. **Watch that histogram.** It is
the single best check that a corridor is sane: a band that grades almost every swing in your library
as *Action* is visibly wrong without needing any statistics, and so is a band so wide that nothing
ever falls outside it.

The editor also tells you where a corridor came from — whether it is the shipped figure or one of
yours — and offers the matching undo:

- **Reset to shipped** — when Pinpoint ships a corridor at exactly this context, this puts it back.
- **Remove your override** — when it does not. Your row is dropped and the corridor is inherited from
  further up the tree instead. Both restore the app's own behaviour; the wording differs because what
  you get back differs.

### Change which side fires

Open the characteristic in **Settings → Diagnostics → Characteristics** and edit the measure attached
to it. Instead of asking you to pick "high" or "low", the app shows you the measure's own words — for
ball position, *"further back, toward the trail foot"* versus the other end of that range — and you
pick the sentence that describes what the characteristic actually is.

This is not decoration. Picking the wrong side is the single most damaging mistake available here,
because the result *looks completely correct*: the app fires confidently, with sensible-sounding text,
on exactly the wrong swings. Reading a sentence rather than choosing a direction is what prevents it.

### Add a measure

From the same editor, "add a measure" opens a picker. You choose the metric, then how to reduce it
(at a position, a change between two positions, a peak across a stretch).

The picker will **refuse to create a measure that does not state what a high value means.** That
sentence is what the direction control shows later, so a measure without one cannot be attached
safely.

### Change a characteristic, or its causes

The characteristic's page shows its consequence text, what detects it, and a **map of its causes and
effects** — tap any box to re-centre the map on it, or long-press for options including adding or
removing a link. Removing a link offers an **undo** in the same breath, which puts it back with its
original strength.

### Change how strict everything is

**Settings → Diagnostics → Measures & norms** carries the **grade policy**, which applies to the whole
library:

| Setting | Effect |
|---|---|
| **Lenient** | flags less; suits a wide range of styles |
| **Standard** | the shipped setting; ordinary variation is not a finding |
| **Strict** | flags more; suits a narrow, coached population |

It is one choice for everything rather than a slider per corridor, deliberately: if "Ideal" means one
thing on one measurement and something else on another, nothing can be compared.

---

## 7. What to be careful with

The honest list. Most of these are mistakes that produce *confident, plausible, wrong* results, which
are far more dangerous than obvious breakage.

**1. You are editing a population norm, not your target.**
Widening a corridor because your own swings fall outside it does not improve your swing — it removes
the app's ability to tell you about it. If you want to track your own progress, that is a different
feature; this is "what is normal for golfers".

**2. The numbers that ship are starting points, not findings.**
Every corridor Pinpoint ships is an authored estimate — the app marks them as such, with no sample
size behind them. They are expected to move as real data accumulates. Do not treat a shipped band as
established fact, and do not be shy about correcting one you have good reason to doubt.

**3. Never judge a corridor on one swing.**
A single reading outside a band tells you almost nothing — about the swing *or* the band. The
histogram over your whole library is the evidence; one swing is an anecdote.

**4. Check which reading you are editing.**
The most common confusion in the whole system. "At impact" and "change from address to impact" are
different measures with different corridors, and they look almost identical in a list. Read the
measure's full name before you drag anything.

**5. Units can match while conventions do not.**
The app refuses a corridor whose *unit* differs from its measure's — degrees against a percentage is
caught immediately. What it cannot catch is a corridor written against a different *convention* in the
same unit: two things both labelled "% of shoulder width" where one means something else. If readings
sit consistently at roughly double or half your corridor, suspect this before adjusting the numbers.

**6. A corridor that never says anything is a broken corridor.**
It is tempting to widen a band until nothing gets flagged. The result cannot report a deviation at
all. The health list watches for both failure modes — almost everything outside, and almost everything
inside — and reports them differently, because they have opposite fixes.

**7. Mind where you author, in the shot-type tree.**
Editing a corridor at *Full swing* creates a row that applies to full swings **only** — pitches,
chips and bunker shots carry on using the general one. That may be exactly what you want. If you meant
"change this for everything", author at *Any shot*.

**8. Bowed and cupped are both correct.**
Before deciding the wrist corridor is wrong for you, check which style context your swings are being
graded against. Two valid styles is the whole reason those contexts exist.

**9. "Not measured" is not "fine".**
If a finding is absent because no camera saw the swing, or no sensor was attached, or the swing could
not be segmented, that is silence — not a clean bill of health. The app keeps these apart everywhere
and never collapses one into the other; be equally careful when reading it.

**10. Editing a shared measure affects every characteristic using it.**
Before you change one, the app tells you how many characteristics ride on it and which ones. Read
that. A measure feeding four characteristics changes all four.

---

## 8. The health list, translated

**Settings → Diagnostics → Causes & health** is the app checking its own knowledge, grouped by kind.
The messages are written in plain language, and here is what the common ones mean:

| What it says | What it means | What to do |
|---|---|---|
| **Nothing to compare against — the signal cannot fire** | A characteristic is watching a measurement that has no corridor anywhere. It can never report anything. | Author a corridor, or accept that this characteristic is undetectable for now |
| **A context graded by nothing at all** | Some shot type has no corridors anywhere up its branch, so every reading on such a shot comes back *Not measured* | Author corridors for it, or move it under a context that has them |
| **A context that changes no grade** | A shot type with no corridors of its own. Harmless — it grades exactly as its parent does | Nothing, unless you intended it to differ |
| **Grades almost the whole library into one band** | The corridor is in the wrong place, in the wrong unit, or so wide it can never speak | Open it and look at the histogram |
| **Your corridor, seated on no swings** | You typed a corridor rather than fitting it to data. Fine as a starting point | Seat it from swings when you have a sample |
| **Club-dependent, but graded by one corridor** | The metric's own description says the number depends on the club, and only one corridor exists | Add per-club corridors |
| **Corridor and measure are in different units** | A real error — degrees graded against percent, or similar | Fix the unit; the grade is meaningless until you do |
| **Flags both sides at once** | One characteristic is watching *both* ends of a range, so it fires whichever way the reading goes and cannot tell too much from too little | Remove one side; the two ends belong to two characteristics |
| **The shipped corridor has been revised since you changed it** | You overrode a corridor, and Pinpoint has since updated its own version | Compare, then keep yours or take theirs |

There is also a **Check corridors** button. It reads every stored swing, grades it against every
corridor, and reports any corridor that puts nearly everything in one band. It needs a swing library
configured (**Settings → Storage**), and it tells you how many swings it looked at — so "nothing
found" is never confused with "nothing checked".

---

## Glossary

| Term | Meaning |
|---|---|
| **Address** | Set up to the ball, before the swing starts. Also **P1** |
| **Axis** | A pair of characteristics describing the two ends of one range (too far forward / too far back) |
| **Bowed** | Lead wrist flexed, back of the hand angled toward the forearm. Positive degrees |
| **Characteristic** | A named feature of a swing, in a coach's language, with a consequence |
| **Context** | A kind of shot — a club, a style, or a shot type — that norms can be organised by |
| **Corridor** | Another word for a norm, from how it is drawn: a band of normal readings |
| **Cupped** | Lead wrist extended, back of the hand angled back. Negative degrees |
| **Finding** | What a characteristic becomes when a signal fires on a particular swing |
| **Grade** | Ideal / Good / Watch / Action — how unusual a reading is. Plus *Not measured* |
| **Impact** | The moment the club strikes the ball. Also **P7** |
| **Latent** | A cause that cannot be seen directly, only inferred from what it explains |
| **Lead / trail** | Lead is the side nearer the target; trail is the other. Handedness-neutral |
| **Measure** | A metric plus exactly which reading of it is meant (at impact, change to the top, …) |
| **Metric** | A quantity the software can extract from a swing |
| **Norm** | The normal range for one measure — a centre and a tolerance either side |
| **P1 … P9** | Standard names for moments in the swing. P1 address, P4 top, P7 impact |
| **Signal** | The rule that watches one measure on one side and fires when a reading is unusual |
| **Tolerance** | How far from the centre still counts as unremarkable. Can differ on each side |
| **Top** | The end of the backswing, where the club changes direction. Also **P4** |
