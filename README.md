# Legacy of Alicia

Dedicated server software for the Alicia game series.

## About this project

**This is a GPL-2.0 fork of [Story-Of-Alicia/alicia-server](https://github.com/Story-Of-Alicia/alicia-server) (pinned at `6bc81ca`), with thanks to the Story of Alicia Studio team. Original server code © Story Of Alicia, GPL-2.0.**

The upstream history is preserved in full in this repository; everything on top of `6bc81ca` is our own work. That work is focused on running the server for real players over a long period: crash and exploit fixes, data durability, the quest lifecycle and daily quests, race director/instance fixes, an achievement system, care skills, and content registries loaded from configuration.

This fork is **not** affiliated with or endorsed by Story of Alicia Studio. Please do not direct questions about this fork to them.

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
