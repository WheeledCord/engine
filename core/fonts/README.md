# Core UI font

The default UI theme loads `unifont-17.0.04.bdf` directly from this directory.
It is GNU Unifont's 8x16 bitmap source, so raylib builds the atlas from hard
one-bit glyphs rather than rasterizing an outline font. Core applies point
filtering and draws it at its native 16-pixel height.

The initial atlas contains printable ASCII and Latin-1. Measuring or drawing text
loads additional glyphs from the font on demand, retaining bitmap alignment and
point filtering. `fontCodepoints` and `fontGlyphCount` select the initial set.
Use `UiTextWidth` when measuring UI text so those glyphs are ready before layout.
Characters absent from the font still display as `?`; this Unifont file covers
the Basic Multilingual Plane, not supplementary emoji. A project can instead
supply an already-loaded `Font`; the UI borrows it without extending or unloading it.

The font remains an external disk asset. `Makefile.core` neither embeds it nor
copies it into project output. A packaged project must choose which font data
to ship and set `UiTheme.fontPath` to its installed location.

GNU Unifont is distributed under the SIL Open Font License 1.1 or GPLv2+ with
the GNU Font Embedding Exception. The license texts are beside this file.
