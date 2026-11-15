# Configuration and persistence

The [user configuration guide](../user/configuration.md) owns the preference reference. The implementation is [settings.h](../../src/config/settings.h), [settings.cpp](../../src/config/settings.cpp) and [ViewPreferenceWriter](../../src/config/view_preference_writer.h).

## Ownership and reload

Built-in defaults are overlaid by the optional base `config.toml`, then writable GUI `settings.toml`. GUI edits never modify the base. Canonical names have one spelling. Runtime loading warns about unknown root/browser keys and decodes recognized preferences normally. The validation CLI remains strict about unknown keys and checks one required file, with no adjacent overrides and no compositor initialization. Wrong types, unsupported known values, incompatible schema versions and malformed TOML still fail. Schema version is integer 1 when supplied. This policy applies to configuration files, not the separate Places state schema.

The worker serializes typed edits and reload requests, coalescing pending edits per preference. Saves reread and validate actual persisted inputs before merging only the requested keys. The override document retains unknown values, arrays and tables while known preference fields are replaced or removed; comments and original formatting are not retained on a changed save. No-op saves preserve the exact bytes. Removing an override restores inheritance. Reset removes only recognized preferences, preserving unknown values for another application version. An invalid reload keeps the last valid snapshot; failed saves reload actual persisted state. Publish only after pending edits/reloads drain.

File monitors follow canonical files, existing path components and resolved declarative targets. Coalesced events trigger worker reads; the read after watch installation closes the read/install gap. Callbacks publish during Update. Only relevant global changes should replace a live preference; an unrelated reload must not reset the current visit's view/zoom/history state. Ordinary close drains pending preference saves.

## Settings layout

[SettingsController](../../src/app/page/settings_controller.cpp) retains the preferred 880-pixel dialog width independently of the opening window size. The common popup layout in [BrowserPage](../../src/app/page/browser_page.cpp) calls `layoutDialog()` before measuring Settings on each requested layout. It reapplies the parent-constrained height up to 640 pixels and the compact navigation breakpoint. Parent configuration already requests popup layout and recenters the dialog. Resize the existing view so its category, scroll state, focus and drafts survive; dismiss an open select dropdown when its anchor geometry changes. Do not persist the temporarily constrained popup dimensions as its preferred size.

## Typing and query scope

`browser.typing_mode` accepts `search` (default), `filter` and `typeahead`. The Settings General page uses the existing Select control for all three. Search and filter typing, including input-method commits, use the same toolbar field; current-folder mode updates its placeholder and tooltip. Explicit field entry and Ctrl+F use that same scope. Typeahead retains recursive explicit search. Preference changes update every existing pane and Miller controller.

[BrowserController](../../src/browser/browser_controller.cpp) owns the scope. Recursive queries keep their 175 ms debounce and worker traversal. Local edits project the current DirectorySession listing through DirectoryModel’s folded literal name filter, without starting traversal or re-reading the folder on each keystroke. Session monitor updates remain visible under the filter; refresh, navigation history and duplication retain the scope. Filtering temporarily projects only the root listing when expandable list folders are enabled. Mode changes cancel pending recursive work before publishing the appropriate source. Miller child and ancestor controllers inherit the source scope.

## Atomic writes

[atomic_file.cpp](../../src/fs/atomic_file.cpp) implements anchored local transactions. Cooperating writers use a persistent advisory lock. Preserve that lock across ordinary cleanup; unlinking it can split writer coordination. New private directories use 0700 and data/temporary/lock files use 0600.

Writes use an exclusive neighboring temporary, file flush, destination revision recheck, atomic publication and parent-directory flush. First creation must not replace an unexpected destination. Reject unsafe sidecar targets, including symlink/hard-link, foreign-owned, special and read-only files. Declarative base symlinks remain readable under the loader's separate rules.

Outcomes distinguish unchanged, not committed, committed but directory synchronization failed, and durable. Report visible-but-not-durable commits accurately. Cleanup removes only the transaction's own temporary. This protects cooperating writers and ordinary races; it does not promise atomic exclusion of an uncooperative same-user writer between a final check and rename.

## State and caches

[StateStore](../../src/config/state_store.h) owns versioned Places and optional last-window tab state with separate persistence revisions. Session writes are serialized with Places writes while reporting their errors independently. Do not conflate settings, Places/session state, future operation journals and regenerable thumbnails. Credentials do not belong in configuration, state, URI history or cache identities.

The shared thumbnail cache and Luciole' ownership ledger are regenerable. Pruning must preserve untracked entries and foreign replacements. A future operation journal has different ordering/recovery requirements; cache persistence is not an implementation of crash-safe file-job recovery.

Relevant suites are [settings](../../tests/settings_test.cpp), [preference writer](../../tests/view_preference_writer_test.cpp), [atomic files](../../tests/atomic_file_test.cpp) and [state store](../../tests/state_store_test.cpp). Use temporary XDG roots and actual invalid/replaced inputs. Test automatic reload and settings layout in an isolated window when those behaviors change.
