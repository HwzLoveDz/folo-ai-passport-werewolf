# Kode Mono UI fonts

Source: <https://github.com/isaozler/kode-mono>

Kode Mono is distributed under the SIL Open Font License 1.1; the full license
is included in `OFL.txt`.

The firmware embeds the printable ASCII range (`0x20-0x7E`) at the five sizes
used by the UI typography tokens:

- Regular 11: metadata
- Regular 13: body copy
- Bold 13: compact emphasis
- Bold 15: section titles
- Bold 21: primary status

The generated files use LVGL format, 4-bpp antialiasing, strong small-screen
autohinting, and no kerning because Kode Mono is monospaced. Source font
SHA-256 values:

- `KodeMono-Regular.ttf`: `7d0cbc9dd7fdd37c344d2512e0485652f22acce2c4b99e54d938419fb3467c6d`
- `KodeMono-Bold.ttf`: `a2cb404f5c6d410e97d64bde5fbfa6eaa51d705869b2e0399e5723081857d3d5`
