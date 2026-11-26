# VitaSurf

A lightweight, HTTPS-only browser for PS Vita homebrew.

- Touch: tap links/tiles, drag to scroll, top bar = back, forward, reload, home, address, star (bookmark), list (bookmarks), tab count (tab switcher)
- Tabs: Start toggles the tab switcher; tap a card to switch, x to close, "+ New tab" to add (up to 9)
- Bookmarks saved to ux0:data/VitaSurf/bookmarks.txt; Cross toggles a bookmark for the current page
- Buttons: Triangle = address/search, Square = reload, L / Circle = back, R = forward, Select = home, D-pad / right stick = scroll
- HTTPS only (http:// is upgraded), TLS 1.2+, certificates checked against a bundled CA file
- Renders text, headings, lists and links (no images, JavaScript or forms)

Build: push to GitHub; Actions builds `VitaSurf.vpk` (download it from the run's artifacts).
