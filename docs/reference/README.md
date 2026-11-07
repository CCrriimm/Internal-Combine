# Reference uploads

Files here are kept as the user supplied them, with the file extensions restored
(`capture_protect.hpp.txt` -> `capture_protect.hpp`,
`capture_protect_patches.diff.txt` -> `capture_protect_patches.diff`).

They are **not compiled and not part of the build**. They are the original
capture-protection sketch that the shipped implementation was written from:

* `capture_protect.hpp` - affinity + a Present-skip sketch for OBS/Steam hooks.
  A Present-skip freezes this game's flip-model swap chain, so what ships instead
  is `src/protected_output.hpp`: the overlay is drawn into its own composition
  window and the game's back buffer is never written, which needs no hook
  chaining at all.
* `capture_protect_patches.diff` - the three overlay.cpp edits that sketch
  assumed.

Current behaviour and the reasoning behind it: `docs/capture-protection.md`.
