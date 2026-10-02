# people-walking.mp4 — reference analysis

Externally-measured ground truth for the `people-walking.mp4` reference clip, for
use by the accuracy test harness. Method: a person detector run on every 3rd
frame plus a frame-to-frame tracker.

## Count

About **400 distinct people** (402 by the tracker's count). On-screen time per
person ranges from about half a second to 17.5 seconds, with a **median of
2.9 seconds**.

## Structure

The video is actually **8 separate fixed-camera clips** joined by crossfades, so
tracking restarts in each clip:

| Clip | Location | Starts at | People |
|---|---|---|---|
| 1 | Bond Street station | 0:00 | 69 |
| 2 | Bond Street, opposite corner | 0:16.5 | 52 |
| 3 | Outside the Next store | 0:32 | 47 |
| 4 | Outside the Vodafone store | 0:43.7 | 47 |
| 5 | Oxford St, Next / bus stop | 0:58.5 | 64 |
| 6 | Tottenham Court Rd / Bond St area | 1:16.2 | 57 |
| 7 | Bridge over the river | 1:31 | 36 |
| 8 | Same bridge, closer view | 1:42.2 | 30 |

## On-screen duration distribution

| Time visible | People |
|---|---|
| Under 1 s | 78 |
| 1–2 s | 72 |
| 2–5 s | 125 |
| 5–10 s | 88 |
| 10–17.5 s | 39 |

A per-person CSV (clip, first/last time seen, seconds visible) was produced as
`people_durations.csv` in the analysis session outputs (not included here).

## Accuracy notes

- **Missed people:** foreground and middle-distance people are picked up
  reliably. Very small people far down the street near the vanishing point are
  mostly missed, so the true count is higher. 400 is a fair count of the people
  you can actually make out.
- **Possible double counts:** a person hidden behind others for more than ~2 s
  may be counted twice. Broken tracks were rejoined when the person reappeared
  nearby at a similar size, and any track shorter than 0.5 s was dropped.
- **No cross-clip matching:** no face/clothing recognition, so someone appearing
  in two clips counts as two people.
