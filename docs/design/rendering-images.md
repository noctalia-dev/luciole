# Rendering and images

## Shared and application-owned code

`lib/` is a normal local copy of Noctalia's reusable library. Do not patch it in Luciole. Port generic fixes upstream before copying reviewed updates back; do not introduce a sibling include path, shell service or second renderer.

[library_setup.cpp](../../src/app/library_setup.cpp) supplies application identity, explicit asset roots, palette, UI strings and the Luciole image provider. Luciole selects its canonical environment/install asset root before library initialization. An invalid explicit root fails rather than silently searching elsewhere.

Luciole owns decoration negotiation, clipboard/drop transport, cursor fallback, browser composition and selection contours. Common widgets use the actual shared Button, Stepper, Select, Toggle, ScrollView and virtual-grid controls. A file-manager preference binding belongs here, while a generic widget correction belongs upstream.

## UI phases

Preserve PrepareFrame → Update → Layout → Render.

- Update applies prepared state and publishes model/UI changes.
- Layout measures, mutates scene structure, binds visible cells and chooses physical image sizes.
- Render only paints prepared state. It performs no layout, traversal, filesystem I/O or decoding.

Callbacks schedule the required phase. They do not synchronously rebuild a scene from an arbitrary worker or input callback. Virtual cells release work when hidden, retired or rebound; bounded pooling must not retain every file in a large directory.

## Icon theme updates

Luciole owns icon selection and monitoring in `src/system/icon_resolver.*` and `icon_theme_poll_source.*`. Desktop GSettings selects the theme when its schema is available; GTK settings.ini provides a fallback. The main-loop watcher observes `changed::icon-theme`, reads the key after connecting, and debounces selection changes alongside existing artwork file watches. Accepted changes invalidate the application icon cache and rebind browser artwork. GSettings objects are optional when the schema is absent; observer teardown disconnects callbacks. The `icon-theme` suite uses private schemas/settings and artwork to exercise selection, fallback, live changes and file replacement.

Resolver instances retain a shared immutable search plan and per-instance result/missing caches. A published plan owns a cached immutable watch-path snapshot; callers may keep old snapshots safely across theme/content generations. Temporary hash sets deduplicate search/watch paths while ordered vectors retain first-match theme, inheritance and size precedence. Generation changes clear local results and acquire the new plan. Filesystem discovery, signatures and watch installation remain synchronous for now; this increment changes neither watch coverage nor reload timing.

## Thumbnail and preview pipeline

[ThumbnailService](../../src/thumbnails/thumbnail_service.h) owns bounded generation requests, revisions, visibility priority and cancellation. [PreviewService](../../src/thumbnails/preview_service.h) requests appropriate preview content. Their generation policy, freedesktop disk store and FFmpeg/Poppler providers remain Luciole-owned.

Thumbnail policy defaults to Local. It requires an affirmative GIO local-filesystem result. All permits generation for regular files with an accessible filesystem path, including mounted network shares, without treating a remote/unknown filesystem flag as a generation ban. Locations with no filesystem path remain shared-cache-only; no whole-source remote download is introduced. Use the same held descriptor, identity/revision checks, cancellation and bounded worker/provider queue as for local files. Image/PDF sources retain the configured encoded-byte cap; seekable video/audio do not inherit that cap and remain subject to helper CPU, wall-time, memory, pixel and output bounds. Cache publication and source-change checks are unchanged.

Raster images use the [GdkPixbuf/Glycin adapter](../../src/imaging/pixbuf_image_decoder.cpp). Decoding/provider processes run outside Render with bounded input, pixels, memory and time. [thumbnail_worker.cpp](../../src/thumbnails/thumbnail_worker.cpp) and [provider_process.cpp](../../src/thumbnails/provider_process.cpp) enforce the helper protocol; [thumbnail_store.cpp](../../src/thumbnails/thumbnail_store.cpp) validates shared PNG cache entries. Dependencies and installed loader coverage belong in [PACKAGING.md](../../PACKAGING.md).

Shared-cache PNG metadata accepts `tEXt`, `zTXt` and `iTXt`, including Qt-compressed URI fields. Decompression is bounded to 128 KiB per value and 256 KiB total text, with the existing CRC, chunk, pixel and file-size checks retained. Browsing can reuse a valid smaller third-party cache class when a larger one is absent; high-resolution previews still require their requested size. Imported cached pixels and any baked-in decorations are left intact.

Cache writes require the existing destination to match Luciole’s ownership ledger and file identity before replacing it; untracked or externally replaced files are skipped, including stale or unreadable third-party PNGs. New files publish without overwriting a competing destination. Cache clearing/pruning retains the same identity checks. Thumbnail metadata’s Software label alone does not establish ownership.

Video generation probes duration, samples near 10%, and tries another position near one third when the first sample is almost entirely black. Short/unknown-duration clips and failed seeks retain a first-frame fallback, and genuinely dark clips remain valid. Probe and sample attempts share the existing eight-second video request budget; source descriptors and helper resource/protocol limits remain. `X-Luciole::VideoVersion` distinguishes the corrected sampling from Luciole’s older first-frame cache entries; only those old Luciole results need regeneration, while other thumbnailers’ cache images remain reusable.

[FileIcon](../../src/ui/browser/file_icon.h) holds shared-library prepared-image leases. Immutable RGBA and source/revision/size identities enter [SharedTextureCache](../../lib/render/core/shared_texture_cache.h). Dispatch owns GPU upload and switching; identical prepared requests share residency within an EGL group. Do not reinstate per-cell texture uploads or a second Luciole GPU cache.

Layout requests physical pixels, including display scale. Grid thumbnails cannot replace the larger preview request. Keep an already ready image visible until a higher-resolution replacement uploads, preserve aspect ratio, and retain source dimensions separately from decoded/output dimensions. Source revisions must invalidate same-size edits without flushing unrelated images.

Preview and Properties share [asynchronous metadata](../../src/thumbnails/preview_metadata.h). The preview presents essential content facts; Properties can expose technical and permission information. Recursive folder totals are a separate explicit Properties service, never a side effect of selecting an entry.

## Shared video decoration

Browsing requests send their selected video frame through the system `ffmpegthumbnailer -f` filter, matching its standard desktop thumbnailer entry. The input is a private PNG re-encoded from validated pixels, passed by descriptor, rather than the original media. This preserves Luciole’s representative-frame selection and source pixel/protocol limits. Decoration runs on the existing worker, with one CPU, at most two seconds and the remainder of the same eight-second video budget. Validate the resulting PNG and require unchanged dimensions. Missing/failed/timed-out helpers retain the selected plain frame; cancellation still prevents publication.

The decorated pixels are stored in the existing shared cache, so other file managers see the same border. The sampling/style version is now `3`; old Luciole video cache entries refresh, while foreign entries remain reusable and protected. Native video preview requests skip the shared video cache, generate a clean frame and retain it in the preview service’s memory cache. They must not overwrite the browsing thumbnail. Non-native/cache-only previews can only use existing cache content, including any baked-in decoration. This is an application-owned adapter for ffmpegthumbnailer, not a general `.thumbnailer` discovery implementation or an icon-theme overlay.

See [PACKAGING.md](../../PACKAGING.md#shared-video-thumbnail-appearance) for runtime availability and fallback behavior.

## File type detection

Directory listings and recursive filename search request GIO’s `standard::content-type` and `standard::fast-content-type` in their existing cancellable asynchronous batches. Worker-backed single-file details query both attributes too. The shared reader in [file_metadata.cpp](../../src/fs/file_metadata.cpp) prefers a valid regular type and uses the filename-only fast type when the provider omits or returns an invalid regular attribute. A valid `application/octet-stream` result remains authoritative. GIO can inspect content when a filename is insufficient, so extensionless text, dotfiles such as `.bash_history` and extensionless images no longer depend on filename guesses alone.

The resulting DirectoryEntry type drives icons, list type labels, preview selection and the Properties MIME field. File-tooltip details use the same provider policy through the worker query. Do not add filename-specific MIME overrides or move queries into layout/render callbacks. Retain no-follow-symlink flags; regular and special file handling belongs to GIO. This uses existing GIO and shared MIME data, with no decoder or packaging dependency changes.

## Verification

Use [thumbnail](../../tests/thumbnail_test.cpp), [preview](../../tests/preview_service_test.cpp), [metadata](../../tests/preview_metadata_test.cpp) and [icon](../../tests/file_icon_test.cpp) suites for deterministic contracts. Verify image sizing, placeholder transitions, scrolling, fractional scale and pane resizing in an isolated actual window. Loader failures, graphics-reset injection and hardware/monitor coverage must be reported at the level actually tested; old successful CPU fixtures are not proof of GPU recovery or universal format coverage.
