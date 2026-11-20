# TouchEQ 8: design notes

## References and what was taken from them

| Source | Used for |
|---|---|
| Ableton Live manual, EQ Eight | 8 bands; per-band type, frequency, gain, Q and on/off; the type set (48/12 dB cuts, shelves, bell, notch); Adaptive Q; Scale; gain +-15 dB |
| FabFilter Pro-Q screenshots | the look: dark well, gold curve filled from the 0 dB line, coloured numbered points, analyser behind the curve, readout of the selected band |
| FreeEQ8 (GPL-3, github.com/GareBear99/FreeEQ8) | read only. It confirmed the usual feature set and ranges (Simper SVF bands, 12/24/48 dB slopes, Q 0.1-24, curve drawn as a path filled to 0 dB over an FFT analyser). None of its code is used, so this plugin can stay MIT. |
| A. Simper, "Linear trap optimised SVF" (Cytomic, 2013) | the filter equations (bell, shelves, HP/LP/notch mixes) |

## Parameters (order frozen)

| Index | Parameter | Notes |
|---|---|---|
| 0 | Output | -24..+24 dB. Index 0 is the one the host nudges on every load, so it is smoothed and harmless. |
| 1, 2 | Scale, Mix | 0-200 %, 0-100 % |
| 3 | Band | 1-8: which point the finger drags. A parameter, not a UI-only local, so it is robust and MIDI-mappable. |
| 4, 5 | Adapt Q, Analyzer | switches |
| 6 + 5b .. | On, Type, Freq, Gain, Q for bands b = 0..7 | Freq 20 Hz-20 kHz log, so the knob, the XY pad x axis and the curve columns share one axis. Gain +-15 dB = the graph's dB axis. Q is stored as a 0..1 position with 0.71 at the centre (0.10-18 in two log halves). A cut filter's point therefore sits on the 0 dB line at Q 0.71 and rises with resonance. |
| 46-237 | Curve 001-192 | read-only, not automatable |
| 238-333 | Spectrum 01-96 | read-only, not automatable |
| 334 | QLink Follow | switch (0.1.2), default On: band-number taps also bank the Q-Links to that band's row |
| 335-342 | Pick 1-8 | momentary triggers for the band-number buttons (0.1.2), not automatable |

Type order is LoCut48, LoCut12, LoShelf, Bell, HiShelf, Notch, HiCut12, HiCut48. The gain types (2-4) are adjacent so
one `RangedEnabling` condition picks the right XY pad.

## Screen and Q-Links (the user's rules)

* Four knobs per row and one Q-Link bank per row (`Bank Direction: RowTopDown`). Knobs come first and the
  selector (Type, a knob with a list) comes last. On/off switches are touch buttons outside the Q-Link rectangle.
  They are still parameters, so they can be automated or MIDI-mapped.
* BAND 1-4 / BAND 5-8: rows Freq, Gain, Q, Type, so a 4 x 4 MPC shows two banks of four bands. OUTPUT: Output,
  Scale, Mix, Band.
* The graph is one shared component, on all three tabs.

## Getting the curve onto the screen (learned on the Key 61, 0.1.0 to 0.1.1)

The MPC draws a control from its own copy of the parameter value and never polls the plugin. In 0.1.0 the plugin
updated the curve and analyser columns silently, so the screen kept the flat curve it read at load and the analyser
stayed empty, while the XY pads (moved by the user, so known to the host) worked. From 0.1.1 the plugin calls
`audioMasterAutomate` for each column that moved: the curve at most ~30 times a second and only for columns that
moved half a pixel or more; the analyser only for bars that moved about 0.5 dB or more (about 1900 updates a second
with full-range music, none when silent). The columns are not automatable, so the MPC should not record them.
Confirmed on hardware in 0.1.0: the slider columns render, and the XY pads have gain up = up.

## CPU: what runs where (0.1.2)

0.1.1 worked on the Key 61 but crackled now and then. Everything for the screen ran inside the audio callback,
in bursts: the curve was rebuilt in every block while a control moved, the 2048-point FFT ran in a single block,
and up to 288 host notifications went out in one block. 0.1.2 splits the work:

* **Audio thread:** the EQ only, plus copying its mono output into a lock-free `SampleTap`. Settled bells and
  shelves at exactly 0 dB are skipped (bit-exact pass-through), and the logs of the target frequency and Q are
  computed when a parameter changes instead of every 16 samples.
* **Screen thread** (one per instance, `nice 10`, wakes 30 times a second): rebuilds the curve when a user
  parameter changed (`CurveModel`, one log10 per column), analyses the newest 2048 samples from the tap about 21
  times a second while audio flows, and sends `audioMasterAutomate` for the columns that moved. When nothing changes
  it only compares numbers.

Checked: `tests/plugin_e2e.cpp` fails if any notification comes from the thread that calls `processReplacing`, and a
ThreadSanitizer build of the plugin runs that test with no races reported.

## Notification budget (0.1.3)

0.1.2 still clicked now and then on the Key 61 while a point was dragged with music playing. A phone recording of it
shows short gaps (a few ms of near-silence inside continuous music, then the sound snapping back), i.e. dropouts, not
a DSP artefact: an offline drag test (a 150 Hz tone through a shelf moved 100 Hz-8 kHz and -15..+15 dB twice a
second, in 30 steps a second) leaves nothing above 4 kHz louder than -61 dB. 0.1.0 (no notifications) did not
crackle; 0.1.1 and 0.1.2 (thousands a second) did, so the host's handling of `audioMasterAutomate` is the suspect.

0.1.3 announces per 30 Hz tick only the columns that moved the most: at most 64 curve and 16 analyser columns,
with a 1-pixel curve step, a ~1 dB analyser step and at most 15 analyser frames a second. The rest catch up on
later ticks. The screen thread runs at normal priority (it was nice 10), so a host lock it holds is released soon.
Measured by the real-time drag test in `tests/plugin_e2e.cpp` (128-frame blocks paced at 44.1 kHz, music-like
input, band 3 dragged 30 times a second): 0.1.2's settings 3949 notifications a second, 0.1.3 1744.
If the Key 61 still drops out, the next levers are the budgets (fewer, slower) and the analyser (off = 0).

## Why a "selected band" instead of grabbing any point

An XY pad covers the whole graph. If eight full-size pads were stacked, the top one would take every touch. So all
eight points are drawn by display pads underneath, and only the selected band's pad, on top, is live (visibility:
`IndexedEnabling/<b>/8/Parameter 3`). If the firmware turns out to hit-test XY pads by their puck only, touches
fall through to the lower pads, and any point could be dragged directly. That would also be fine.

## Band-number buttons and QLink Follow (0.1.2)

On the Key 61 a tap on a band number selected the band but left the Q-Links where they were. A component's
Mouse Down actions fire only when the touched child does not take the touch itself, and a Button does, so the
Q-Link action on the button's wrapper most likely never ran (knob cells work, so their Knob/Label children let it
through).
Now the button is a film-strip `Image` of `Band` (lit on frame b) and the component's own actions do the work:

* `Toggle Switch` on `Pick b`. The skin has no "set a parameter to a value" action, so the plugin's screen thread
  sees the trigger, sets `Band = b`, resets the trigger and announces both.
* With `QLink Follow` on, also `Q-Link` on `Freq b`, which banks the knobs to band b's row. Two copies of each
  button exist, shown by `IndexedEnabling/<0|1>/2/Parameter 334`.

A Q-Link action can only bank to rows of the current tab; the skin format has no action that switches tabs.
The grab pad and power buttons have no Q-Link action any more, so with Follow off only knob touches move the bank.
Unverified on hardware: that the Image child really lets the touch through to the component.

## DSP

* Bands run in series, each a TPT state-variable filter: one section, or four for the 48 dB types (8th-order
  Butterworth, with resonance on the last section). A 0 dB bell or shelf is an exact pass-through.
* Frequency, gain and Q glide (12 ms, coefficients updated every 16 samples). On/off fades over 6 ms, and a type
  change fades out, swaps the filter, clears its state and fades back in.
* The curve uses the bilinear identity: the digital response at f equals the analog prototype at
  tan(pi f / fs) / g. The drawn curve is therefore exact, not an approximation (tested to 0.1 dB against measured audio).
* Analyser: mono sum post EQ, 2048-point Hann FFT every 2048 samples, 96 log columns (interpolated below a bin's
  width), and a +4.5 dB/oct tilt. It rises instantly and falls at about 30 dB/s.
