# Map label glyphs

`Noto Sans Regular/*.pbf` are signed-distance-field glyph ranges for MapLibre,
taken from the `smp-noto-glyphs` package (MIT, by Digital Democracy), which
renders GoNotoKurrent, a merged build of the Noto Sans fonts.

The font data is licensed under the SIL Open Font License 1.1
(https://openfontlicense.org). The files are stored uncompressed because
MapLibre Native does not decompress local glyph files.

`Walker Mono/*.pbf` (used by the WALKER map styles) are the same Noto ranges
with every character of VT323 (Copyright 2011, The VT323 Project Authors;
SIL Open Font License 1.1, see `App/Resources/Fonts/VT323-OFL.txt`) rendered over
them by `Tools/make_glyphs.py`.

CJK and Hangul are not included: MapLibre renders those characters with the
phone's own fonts.
