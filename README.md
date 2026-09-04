# Legacy of Alicia

Dedicated server software for the Alicia game series.

## About this project

**This is a GPL-2.0 fork of [Story-Of-Alicia/alicia-server](https://github.com/Story-Of-Alicia/alicia-server) (pinned at `6bc81ca`), with thanks to the Story of Alicia Studio team. Original server code © Story Of Alicia, GPL-2.0.**

The upstream history is preserved in full in this repository; everything on top of `6bc81ca` is our own work. That work is focused on running the server for real players over a long period: crash and exploit fixes, data durability, the quest lifecycle and daily quests, race director/instance fixes, an achievement system, care skills, and content registries loaded from configuration.

This fork is **not** affiliated with or endorsed by Story of Alicia Studio. Please do not direct questions about this fork to them.

## What this fork adds

Plain-language summary of the gameplay and reliability work we built on top of the upstream server. This list will grow as we keep adding to the project.

- **Story quests and quest arcs** — playable questlines with proper start/finish, rewards, and multi-step story arcs.
- **Daily quests** — daily tasks that reset each day and hand out their rewards.
- **Achievement system** — achievements that track your progress as you play and award cosmetic rewards, built on a clean event system so new achievements are easy to add. Racing has its own set of 17 more achievements — finishing, winning, mastery over many races, riding inside a time window, retiring, and revenge on a rival. The award popup is held until you are back on your ranch, so it never interrupts a race or the results screen, and game time runs on Moscow time.
- **Horse racing that actually works** — fixes to race start, the countdown, in-race item pickups, and matchmaking so races run correctly for multiple players.
- **Race statistics** — every course you ride keeps your best time and how many times you raced it, and each horse remembers its own records: longest glide and the longest run of boosts. A win is credited exactly once, even if something goes wrong after you cross the finish line.
- **Horse care skills** — the grooming/feeding care activities and their progression.
- **Breeding and horse market fixes** — breeding, the stallion market, and horse handling behave correctly.
- **Breeding fairness fixes** — coat/mane/tail tables are read by the right key, the rarest coat is kept out of breeding, the emblem chance is driven by data instead of always firing, and foal potential is weighted by coat tier.
- **Economy protection** — closed a number of exploits that let players gain free or negative currency, and reworked how rewards are paid out.
- **Moderation and chat** — improvements to in-game moderation tools and chat.
- **Crash and stability fixes** — the server no longer crashes on malformed or unexpected client data; it logs and keeps running.
- **Reliable saving** — player progress is written to disk atomically, so a crash can't corrupt your data.
- **Configurable content** — quests, items, achievements, magic, pets, breeding and more are loaded from readable config files.
- **Russian localization of the item catalog** — item, package and set names are translated into Russian.

## License

GPL-2.0. See [LICENSE](LICENSE). Every file that carries a `Copyright (C) ... Story Of Alicia` header keeps it — those are the upstream authors' and are preserved as the license requires.

Third-party components keep their own licenses:

- `3rd-party/` — git submodules, each under its own upstream license.
- `include/libserver/util/picosha2.hpp` — PicoSHA2, MIT license (license block retained in the file).

## Building

[Read the build instructions](COMPILING.md).

## How to run

[Read more on how to run the server and connect the client.](HOWTORUN.md)

## Contributing

[Read how to contribute to the project](CONTRIBUTING.md).
