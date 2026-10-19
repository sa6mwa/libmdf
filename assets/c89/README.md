The SVG artwork, stylesheet and section navigation are from
https://c89.systems/lights-out/ (retrieved 2026-10-19).

- `c89-mark.svg`: `/lights-out/assets/c89-mark-white-colour-black-hexagon-transparent.svg`
- `c89-systems.svg`: `/assets/logos/c89-systems.svg`
- `style.css`: the page's brand styling; the renderer supplies the background
  image URI and keeps the caller's document width and font settings.
- `navigation.js`: `/lights-out/navigation.js`, adapted to select semantic
  heading markers from libmdf rather than heading-like code comments.

CMake embeds these assets into the library. Rendering and dumping use the same
SVG bytes; runtime network access is never needed to produce a document.
