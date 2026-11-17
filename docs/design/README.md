# Maintainer design notes

The [user guide](../user/index.md) explains how to use and configure Luciole. These notes describe the contracts that maintainers need when changing it.

- [Architecture and ownership](architecture.md): modules, windows, pages and asynchronous work.
- [Rendering and images](rendering-images.md): shared-library boundary, UI phases, thumbnails and texture residency.
- [Configuration and persistence](configuration.md): precedence, reload, typed edits and atomic writes.
- [File jobs and desktop boundaries](file-jobs.md): job ownership, conflicts, provider operations and launching.

[CONTRIBUTING.md](../../CONTRIBUTING.md) defines development conventions and [PACKAGING.md](../../PACKAGING.md) owns dependency and install requirements.

When behavior changes, update its user guide. When ownership, phase rules, persistence or provider guarantees change, update the relevant design note too.
