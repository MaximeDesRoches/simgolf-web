SimGolf for desktop
===================

This app runs Sid Meier's SimGolf (2002) natively.

  WINDOWS VERSION: UNTESTED. The Windows build compiles, but nobody has run
  it on Windows yet. It may not start or may crash. If you try it, please
  report what happens: https://github.com/RichardAtCT/simgolf-web/issues
  The macOS version has been tested on Apple Silicon and Intel (Rosetta).
 It contains no game
files: on first launch it asks for your own copy, the same two things the
browser version needs:

  * the SimGolf CD image (.iso, .bin or .img), and
  * the v1.02 no-CD golf.exe,

or the .zip or .7z they came in. Drop them on the setup window or click
"Choose files". They're unpacked into your app data folder and never leave
your computer:

  macOS:   ~/Library/Application Support/SimGolf/
  Windows: %APPDATA%\SimGolf\

Your saves are in the game/Saved Games folder there.

Opening the app the first time
------------------------------
The app isn't signed by Apple or Microsoft, so the system warns about it.

  macOS: open the .dmg and drag SimGolf to Applications. The first time,
  Control-click (or right-click) SimGolf and choose Open. On macOS 15 and
  later, if that only offers "Move to Bin", open System Settings > Privacy &
  Security and click "Open Anyway" next to SimGolf, then open it again.

  Windows (untested, see above): unzip the folder anywhere and run
  SimGolf.exe. If SmartScreen
  says "Windows protected your PC", click "More info", then "Run anyway".

Keys
----
  Alt+Enter or F11 (Ctrl+Cmd+F on a Mac)   full screen on/off
  Ctrl+F10                                 smooth or sharp scaling
  Everything else is the game's own keys.

Mods
----
Unzip a mod into the mods folder in the app data folder above, one folder
per mod. Theme packs then show up under Select A Theme on the main menu.

Settings (screen width for widescreen, edge scrolling, scaling) are in
settings.ini in the app data folder.

Not yet in this version: the intro and closing videos, and the browser
version's sharper high-resolution rendering.

SimGolf is (c) Firaxis Games / Electronic Arts. This port is an unofficial
fan project: https://github.com/RichardAtCT/simgolf-web
