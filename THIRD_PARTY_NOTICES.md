# Third-party notices

STYLY NetSync for Godot is licensed under Apache-2.0 (see [LICENSE](LICENSE)),
matching upstream STYLY NetSync.

It builds against the components below. Both are git submodules under
`third_party/`, pinned to the revisions listed and used **unmodified**. Their
full licence texts ship with the submodules at the paths given.

---

## godot-cpp

| | |
|---|---|
| Upstream | https://github.com/godotengine/godot-cpp |
| Version | branch `4.3`, commit `d5cc777a89d899665fb61f1650ef0dc0cf6488c4` |
| Licence | **MIT** |
| Full text | `third_party/godot-cpp/LICENSE.md` |
| Copyright | Copyright (c) 2017-present Godot Engine contributors.<br>Copyright (c) 2014-2017 Godot Engine Authors. |
| How it is used | Compiled into the GDExtension shared library (`addons/styly_netsync/bin/`) |

MIT permits redistribution in source and binary form provided the copyright
notice and permission notice are included. Redistributing a built
`libstyly_netsync.*` therefore requires shipping this file (or an equivalent
notice) alongside it.

---

## ZeroMQ (libzmq)

| | |
|---|---|
| Upstream | https://github.com/zeromq/libzmq |
| Version | `v4.3.5`, commit `622fc6dde99ee172ebaa9c8628d85a7a1995a21d` |
| Licence | **MPL-2.0** (Mozilla Public License, Version 2.0) |
| Full text | `third_party/libzmq/LICENSE` |
| How it is used | Built as a static library and **linked into** the GDExtension shared library |

### What MPL-2.0 requires here

MPL-2.0 is a *file-level* copyleft. It attaches to the libzmq source files
themselves, not to code that merely links against them.

* **This project's own source is unaffected.** MPL-2.0 §3.3 explicitly permits
  combining Covered Software with other code under different terms and
  distributing the combination — so linking libzmq into an Apache-2.0 extension,
  and into your own game, is fine, and your code stays under your own licence.
* **Distributing a binary that contains libzmq** obliges you to make the libzmq
  *source* available to recipients (MPL-2.0 §3.2) and to keep its licence notice
  intact (§3.1). Since libzmq is used unmodified at a public, pinned commit,
  pointing recipients at
  `https://github.com/zeromq/libzmq/tree/622fc6dde99ee172ebaa9c8628d85a7a1995a21d`
  satisfies that, as does shipping this file.
* **If you modify libzmq**, those modified files remain under MPL-2.0 and their
  source must be released. This project does not modify it; the submodule is
  used as-is.

libzmq 4.3.5 is the first release under MPL-2.0 (earlier versions were LGPLv3
with a static-linking exception), which is why this specific version is pinned.

### Build configuration

libzmq is built with `ENABLE_CURVE=OFF`, `WITH_LIBSODIUM=OFF` and `WITH_TLS=OFF`
(see `scripts/build_libzmq.py`), so **no** additional cryptographic dependency —
libsodium, OpenSSL or GnuTLS — is pulled in or redistributed. Draft APIs are
disabled; only libzmq's stable API is used.

---

## Upstream STYLY NetSync

| | |
|---|---|
| Upstream | https://github.com/styly-dev/STYLY-NetSync |
| Licence | Apache-2.0 |

Upstream STYLY NetSync is **not** vendored, redistributed or modified by this
project. It is referenced in two ways only:

* as the specification this client implements, documented in
  [`docs/PROTOCOL_V8.md`](docs/PROTOCOL_V8.md);
* as a test oracle — `tests/tools/generate_golden_vectors.py` clones it
  read-only at a pinned commit and imports its `binary_serializer` module to
  generate expected bytes.

The generated expectations in `tests/golden/vectors.json` are test data derived
from running upstream's serializer, not a copy of its code.
