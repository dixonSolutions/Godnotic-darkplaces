# Godnotic-darkplaces

**The engine of [Godnotic](https://github.com/dixonSolutions/Godnotic)**, a custom fork of [Xonotic](https://xonotic.org) for Android, Linux, Ubuntu Touch and the browser.

This is a fork of the [DarkPlaces engine](https://github.com/DarkPlacesEngine/darkplaces) as Xonotic carries it. On top of upstream it adds what Godnotic needs from an engine: the touch HUD and touch input, the UI bridge that Godnotic's native interfaces talk to, the Lua mod runtime, faster pack and map loading, and the network changes behind quicker joins and LAN games. Godnotic's own files are named `touch_*`; everything else is upstream's, changed as little as possible. Upstream's own description of the engine is in [README.md](../README.md).

**This repository does not build a game by itself, and nothing is released from it.** To play, to build, or to report a problem, go to [dixonSolutions/Godnotic](https://github.com/dixonSolutions/Godnotic).

## The Godnotic repositories

| Repository | What it is |
|---|---|
| [Godnotic](https://github.com/dixonSolutions/Godnotic) | The game as it ships: packaging, native interfaces, releases, docs, issues. Carries a copy of this repository in `engine/darkplaces` |
| **Godnotic-darkplaces** (this one) | The engine |
| [Godnotic-data](https://github.com/dixonSolutions/Godnotic-data) | The game code (QuakeC) and the data packs clients download |
| [dixonSolutions.github.io](https://github.com/dixonSolutions/dixonSolutions.github.io) | Keeps the update addresses of installs made before the rename working |

Play in the browser: <https://godnotic.vercel.app>

## Changing the engine

Most engine work is done in the main repository, where the engine, the game code and the packages build together, and is published here from there (`scripts/component-push.sh`). A commit or pull request made here directly is built and tested inside the main repository by this repository's workflow; once on `main` it is imported there, merged and released.

`.github/` belongs to this repository alone and is never copied to or from the main one.

## Licence

GPL-2.0-or-later, as DarkPlaces: see [COPYING](../COPYING). Lua 5.4 (`touch_lua54`) is MIT and the zstd decoder (`touch_vendor/zstd`) BSD-3-Clause; each keeps its licence file.
