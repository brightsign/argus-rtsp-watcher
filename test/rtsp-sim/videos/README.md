# Drop your test videos here

Put H.264 `.mp4` clips in this folder. For each camera (`lobby`, `entrance`,
`aisle`) pick one of:

- **Single clip, looped:** `lobby.mp4`
- **A series (played in order, then looped):** a folder `lobby/` with clips —
  they play sorted by name, so name them `01.mp4`, `02.mp4`, ...
- **Explicit playlist:** `lobby.txt` in ffmpeg concat format, one line per clip
  with a container path:

  ```
  file '/videos/clip1.mp4'
  file '/videos/clip2.mp4'
  ```

  (paths are as seen *inside the container* — this folder is mounted at `/videos`.)

Use clips with people walking through your zone areas so detection and `in_zone`
have something to report. Zones are normalized `0..1`, so any resolution works.

`.mp4` files and clip subfolders here are git-ignored.
