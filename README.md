[![OpenCloud Desktop CI](https://github.com/RyanTT/opencloud-desktop-multisync/actions/workflows/main.yml/badge.svg)](https://github.com/RyanTT/opencloud-desktop-multisync/actions/workflows/main.yml)
# `OpenCloud Desktop` (multisync fork)

An unofficial fork of the [`OpenCloud Desktop`](https://github.com/opencloud-eu/desktop)
client, which synchronizes files from `OpenCloud` with your computer. It is not
affiliated with or supported by the upstream project.

## What this fork adds

- **Sync any folder to any local folder.** The new "Add Custom Folder" button
  syncs a whole Space, or any folder inside a Space, to a local folder of your
  choice. Upstream only syncs a whole Space into the default sync root.
- Pick the server folder from a tree that loads on demand.
- Virtual files can be switched on or off for each custom folder.
- Checks for overlapping server folders and invalid local paths before a sync is added.
- Custom folders keep their own look: the client does not write or remove a
  `Desktop.ini` in a folder you picked yourself.

Everything else behaves like upstream.

## Download

Releases are in the [Releases tab](https://github.com/RyanTT/opencloud-desktop-multisync/releases).
They are unsigned beta builds of the Windows setup, so Windows SmartScreen may
warn on first launch.

## Source code and upstream

This repository is a fork of https://github.com/opencloud-eu/desktop. The
authoritative upstream project, its releases and its contributors are there.

## Reporting issues

Problems with the features listed above belong in this repository's
[issues](https://github.com/RyanTT/opencloud-desktop-multisync/issues). For
anything that also happens in the upstream client, please use the
[upstream issue tracker](https://github.com/opencloud-eu/desktop/issues).

## License

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    for more details.
