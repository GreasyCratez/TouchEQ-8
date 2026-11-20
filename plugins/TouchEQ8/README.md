# TouchEQ 8

An 8-band parametric EQ for standalone MPCs, with a live graph you can play with your finger: a filled EQ curve, a
spectrum analyser behind it, and one coloured point per band that you drag to set its frequency and gain.

## Install (MPC OS 3.10.1 or later)

1. Download `TouchEQ-8-<version>-mpc-sdk.zip` from the GitHub Releases page and unzip it.
2. Copy the folder `GreasyCratez - VST - TouchEQ 8` into a folder named exactly `VST2 Plugins` at the root of an MPC
   drive (SD card, USB stick or internal drive).
3. On the MPC, turn on Preferences > Enhancements (Beta) > Enable Enhancements and External Plugin Support.
4. Restart the MPC. TouchEQ 8 appears as an insert effect.

The folder holds both builds: 32-bit (Key 61, Key 37, Live, Live II, One, X, Force) and 64-bit (Live III, XL).

## Using it

* **Pick a point:** tap a band's number button (1-8) at the left of its row. That band's point gets a white ring.
* **Drag it:** drag on the graph. Left/right is frequency, up/down is gain. For cut and notch types (which have no
  gain), up/down is Q, so the point rises as the filter gets more resonant.
* **Power button** next to the number turns a band on or off. Off points are drawn hollow.
* **Knobs:** every row is Freq, Gain, Q, Type. Double-tap a knob for the large editor; double-tap Type for the list.
* **Header:** shows the selected band's type, frequency, gain and Q.

| Tab | Q-Link rows (top to bottom; one bank of four each) |
|---|---|
| BAND 1-4 | Band 1, Band 2, Band 3, Band 4: Freq, Gain, Q, Type |
| BAND 5-8 | Band 5, Band 6, Band 7, Band 8: Freq, Gain, Q, Type |
| OUTPUT | Output, Scale, Mix, Band (which point the finger drags) |

On the Key 61's four Q-Links, touching any knob brings its band's row under the knobs; the Q-Link bank button
steps through the rows. With **QLink Follow** on (OUTPUT tab, on by default), tapping a band's number button also
puts the Q-Links on that band's row, so tap 3 and the knobs are Band 3's Freq, Gain, Q, Type. Turn it off to keep
the Q-Links where they are while you pick points. A number button only reaches the rows of the tab it is on
(the skin cannot switch tabs), so for bands 5-8 use the BAND 5-8 tab. The switches (band On, Adapt Q, Analyzer,
QLink Follow) are touch buttons, not Q-Links. They are ordinary plugin parameters, so they can still be automated or
MIDI-mapped like any other.

### Filter types

| Type | Gain | Q does |
|---|---|---|
| LoCut48 / HiCut48 | - | resonance (0.71 = plain 48 dB/oct Butterworth) |
| LoCut12 / HiCut12 | - | resonance (0.71 = Butterworth) |
| LoShelf / HiShelf | +-15 dB | slope / overshoot |
| Bell | +-15 dB | width (Adapt Q narrows it as the gain grows) |
| Notch | - | width |

**Scale** multiplies every band's gain (0-200 %). **Mix** blends the dry signal back in. **Output** is +-24 dB.
The curve is the EQ's real response, computed from the same filter coefficients the audio uses.

## Programs

Init (Flat), Vocal Presence, Kick Punch, Clean Bass, Air Lift, Lo-Fi Phone, Mix Smile, Hat Tamer.

## CPU

About 0.7 % of one x86 core with all 8 bands and the analyser on. The MPC's ARM cores are much slower; expect
roughly 5-15 % of one core on a 32-bit MPC. Turning the analyser off saves its FFT.

## Licence

MIT. The filters follow Andrew Simper's published linear-trapezoidal SVF equations (Cytomic, 2013). No code from
FreeEQ8 (GPL-3) or any other EQ is used; FreeEQ8 was only read for its feature set and parameter ranges.
