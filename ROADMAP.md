# KoraLo roadmap

Current r42: native vector animation workspace, transform/opacity/mask/crop keys, cubic easing, incremental brush rasterization and OpenGL UI (GLX/WGL) are implemented. Vector rasterization and Anime remain CPU; point morphing, the full GPU compositor and OFX rendering remain pending. Linux GUI and software OpenGL were exercised; Windows was cross-built only. See `docs/R42_ANIMATION_GPU_RU.md`.

This roadmap keeps the editor native C++20 and advances in small testable layers. Later work should extend the existing architecture rather than restart the application.

## Native workspace UI reference pass

Completed as a presentation-layer pass on top of the existing editor architecture:

- compact command toolbar;
- Project Media tree/grid workspace;
- Video Preview + Master layout;
- timeline toolbar/timecode shell;
- widened track headers and event styling based on the supplied mockup;
- existing edit hit-testing adapted to the new track-header geometry.

This pass intentionally does **not** introduce HTML/WebView runtime code. A follow-up functional-control pass now wires the visible command buttons, Project Media filtering/search, transport, Trimmer In/Out + initial Insert/Overwrite, track Lock/Mute/Solo/level/pan, basic opacity controls and Auto Crossfade into real KoraLo actions. Docking, resizable pane splitters, thumbnail decode caching, real audio metering and a full generator/FX/transition model remain later UI work.

The current hardening pass also replaces text-surrogate toolbar controls with a native generated bitmap icon atlas sourced from the supplied Font Awesome SVG set, adds persistent Win32 GDI double buffering to prevent half-painted/flickering frames, and makes track level/pan drags preview-first with one undo commit on mouse-up.

The branding pass also embeds the supplied KoraLo startup artwork into the Win32 executable as a native splash resource and uses the supplied square badge as the executable/window/taskbar icon. This does not introduce a PNG/SVG runtime dependency.

## Current milestone — timeline interaction hardening

Completed in this update:

- `SelectionModel`;
- marquee multi-select and additive marquee;
- group-aware selection;
- multi-selection/group-aware move;
- group-aware trim and slip;
- transaction-based drag preview/commit;
- Roll;
- Slide;
- Ripple Trim;
- same-kind track reorder;
- native regression tests for move, trim, group move, marquee, Roll, Slide, Ripple Trim, track reorder, undo, and redo.

Deferred within this milestone:

- Stretch drag mode;
- richer trim cursor/tool affordances;
- track-header ghost/insertion visualization beyond the current target-row highlight.

## Current — Media / Take / Event model

Refactor incrementally toward:

```text
Media -> Take -> TrackEvent -> Track -> Project
```

Priorities:

- **done in r6:** explicit `Media`/`Take`/`TrackEvent` separation with v5 project backward loading;
- **done in r6:** Source Trimmer Insert/Overwrite now create TrackEvents with Takes;
- timeline In/Out;
- harden the initial transactional Insert/Overwrite implementation around takes, target-track choice and timeline ranges;
- **done in r6:** group-aware split creates independent left/right linked groups;
- advanced trim tools and edge cases;
- **in progress:** source/take offsets and playback-rate semantics; r6 covers active/all-take slip, trim offsets, loop and take switching.

## Then — persistent media engine

The temporary process-per-operation layer is now shell-free and Unicode-safe. ffprobe metadata parsing is delimiter-based with a selected-stream fallback rather than relying on `std::regex` over one writer format, but the process-per-operation architecture is still intentionally transitional. Replace per-frame process launches in the uncached preview path with persistent sessions:

- persistent decoder sessions;
- seek queue and request coalescing;
- frame prefetch/cache;
- realtime audio mixer;
- audio master clock;
- A/V synchronization;
- scrub playback;
- proxy/cache policy integration.

## Later — compositor / FX / GPU

The compositor must be independent from FFmpeg render-command generation. Minimum graph nodes:

```text
source
transform
crop
opacity
blend
transition
effect
output
```

Effects should use:

```text
EffectDefinition -> ParameterDefinition -> EffectInstance
```

Minimum parameter types:

```text
double
integer
bool
choice
color/RGBA
vec2
string
group
action/button
```

Each parameter declares whether it is animatable; keyframes belong to a specific parameter. Background-analysis effects such as stabilization or optical flow should use a separate `AnalysisJob`/cache subsystem.

OpenColorIO/ACES remains a separate color-management layer around the compositor, not timeline logic.

## Long-term NLE features

- fades/transitions and transition editor;
- keyframed VideoMotion/transform;
- Media FX / Event FX / Track FX / Video Output FX separation;
- markers/regions and render Project/Selection/Regions;
- renderer/template model;
- waveform cache;
- nested timelines;
- multicam;
- OFX/VST hosting after the native edit/playback core is stable.

## r4 completed — audio preview, command menus and settings separation

- Added interim realtime audio preview through shell-free `ffplay` child processes for active timeline audio events. It is intentionally isolated behind `MediaEngine` so the later native realtime mixer can replace it without changing timeline/UI semantics.
- Added playback preview throttling, duplicate-request coalescing and synchronized frame-cache access as an optimization bridge before persistent decoder sessions.
- Added a native File/Edit/View/Insert/Tools/Options/Help command surface based on observed NLE workflow categories, exposing only implemented KoraLo commands.
- Added native Project Properties and Preferences overlays. Project settings apply transactionally; Preferences controls audio preview/quality/snapping/crossfades.
- UI font selection now requests Open Sans with native fallback. No web UI or font runtime is introduced.

r15 lands the first persistent decoder session for ordinary sequential video playback. Next playback work remains the generalized seek queue/prefetch path, a KoraLo realtime mixer, audio master clock, A/V sync and scrub. The current ffplay audio preview is a temporary functional layer, not the final audio engine.


### r6 editing-semantics checkpoint

Completed after clean-room review of supplied VEGAS shipped scripts/public facade metadata:

- canonical TrackEvent + Take model and project-format v6;
- active take / next-prev take;
- event loop and Slip All Takes;
- Ignore Event Grouping;
- proper split of linked groups into left/right groups;
- Quantize to Frames;
- Trim Start/End to Cursor;
- relative vertical group movement among same-kind tracks;
- two-sided automatic overlap fade generation in final render.

Next research/edit block: timeline In/Out/time selection, post-edit ripple edge cases, event heal/close gaps, additional fade curve shapes, and Source Trimmer target-track/range semantics.


## Completed — Event Pan/Crop r7

- `VideoMotion` + parameter-owned keyframes for Center X/Y, Zoom and Rotation.
- Native Pan/Crop editor with frame manipulation and event-local keyframe timeline.
- Transactional Pan/Crop gestures and undo/redo.
- Split-safe motion curves.
- Preview/render integration.

Next compositor work: Track Motion, richer interpolation, masks/perspective, then graph nodes (`source -> crop -> transform -> opacity -> effect -> blend -> output`) independent from FFmpeg command generation.

### Completed in r8
- Real cached Master audio meter envelope (interim ffplay preview remains isolated behind MediaEngine).
- Timeline-visible Event Pan/Crop motion/keyframe lane.
- Interactive Project Media tree and toolbar de-duplication.
- First direct clean-room `.veg` container reader: validated project settings + media references + conservative single-source import.

### VEG compatibility next
- Controlled-project differential research for TrackEvent start/length/take offsets/groups/fades.
- Stable media/event reference mapping and import parity report.
- Do not guess unknown private fields; preserve original `.veg` read-only and save KoraLo edits to `.kora`.

## r9 completed — GPU assist, event thumbnails, FX foundation

- Added phase-1 GPU assist in `MediaEngine`: runtime capability discovery plus hardware decode/proxy/H.264 encode attempts with deterministic CPU fallback.
- Added asynchronous cached filmstrip thumbnails on video TrackEvents. Filmstrips follow Take offset/rate/loop and update against transactional trim previews.
- Added `EffectDefinition -> ParameterDefinition -> EffectInstance` and parameter-owned keyframes.
- Added first built-in KoraLo effects: Brightness/Contrast, Saturation and Gaussian Blur.
- Added Media/Event/Track/Video Output FX containers to the project model and v8 serialization.
- Event FX has a usable first UI in the Video FX pane; Track/Output controls remain intentionally smaller while the compositor graph is being formalized.

Next GPU/compositor work: generalize the r15 persistent playback decoder into a seek/prefetch session layer, add texture/frame upload abstraction and the native compositor graph (`source -> crop -> transform -> opacity -> effect -> blend -> output`), then platform GPU backends (D3D11 on Windows and Vulkan/OpenGL path on Linux). FFmpeg hardware assist remains an acceleration bridge, not the final compositor.

## r10 completed — viewport / direct VEG timing / Video FX browser

- Timeline track body is clipped and vertically scrollable; hit-testing follows `scroll_y` and a scrollbar exposes hidden tracks.
- Timeline minimum zoom is reduced and direct VEG import performs a one-time fit-to-project.
- Direct `.veg` import now restores validated track-list order, TrackEvent start/length and Take source offsets from the supplied VEGAS 17 Wave64 project shape.
- MP3/M4A/etc. embedded cover art is normalized away at the media probe boundary so audio files do not create fake video events.
- Pan/Crop editor shows event-local and project time together; render-time event motion uses normalized input PTS.
- Video FX browser now has search, All/effect list, native preset preview, Event FX chain, enable/remove and numeric parameter editing.

Next reverse-engineering targets: controlled differential VEG projects for serialized fades, groups, playback rate, envelopes and Pan/Crop records; then map only fields that can be independently validated. In parallel, generalize the r15 sequential playback session into random-seek/prefetch decoding and a native GPU compositor graph.

- [x] r11: 12h timeline navigator, mouse navigation, P/C rotation, system/media preflight, FX drag-drop, editable Render As presets.

## r12 completed — long-form preview preparation

- [x] Real Draft / Preview / Good / Best preview decode tiers.
- [x] Independent 15/24/30/60 preview FPS limit.
- [x] Background import preparation with visible progress/status.
- [x] Persistent seek-friendly preview cache for long video sources with GPU attempt + CPU fallback; r17 moves it to all-I H.264 and starts preparation at >=20s.
- [x] Representative frame warmup and audio-envelope warmup.
- [x] Prepared-cache toggle in Preferences.
- [x] Alpha-dimmed native settings/render overlays on Win32 and transparency fallback on X11.
- [x] Startup splash loading/system-check progress on Windows.
- [~] Persistent sequential decoder session implemented in r15; random-seek session reuse, seek queue and frame prefetch remain.
- [ ] Native realtime audio mixer/master clock and A/V sync.

## r13 completed — Windows/MSVC portability hardening

- [x] Split the oversized UI command dispatcher into small command-domain handlers to avoid MSVC C1061 block-nesting limits.
- [x] Remove reported C4458/C4456 shadowing warnings.
- [x] Enable `-Wshadow` on GCC/Clang builds to catch the same class of portability warnings earlier.
- [x] Re-run clean Release build and full native/media regression suite before packaging.
- [ ] Confirm this exact r13 on Visual Studio 2022 from a Windows user build log.


### Completed in r14
- Runtime `locales/*.lang` discovery/fallback and language selection infrastructure.
- Right-click context workflows for Event / Timeline / Track / Project Media.
- Next localization step: replace remaining legacy hard-coded UI strings and upgrade Linux X11 text to a full Unicode font renderer.

## r15 completed — direct event fades + sequential decoder session

- [x] Top-left Fade In and top-right Fade Out mouse handles on events.
- [x] Frame-quantized, preview-first fade drag; Project commit only on mouse-up; one drag = one Undo command.
- [x] Live diagonal fade line and offset tooltip; 29.97/59.94 UI timecode uses drop-frame `;`.
- [x] Video preview fade opacity, audio meter fade gain, realtime audio-preview fade envelope, final-render fade path.
- [x] Persistent shell-free FFmpeg raw-video pipe for consecutive playback frames with bounded catch-up and CPU fallback.
- [x] r17: keep dynamic Pan/Crop/FX playback on the persistent decoder with an in-process CPU preview composite.
- [ ] Generalize random scrubbing into a reusable seek queue/prefetch session and native compositor surfaces.


## r16 completed — horizontal timeline viewport isolation

- [x] Clip Event rendering to the content pane when horizontal scroll moves an Event start behind the track headers.
- [x] Keep track-header mouse input isolated from hidden/off-screen Event geometry.
- [x] Limit audio waveform drawing to visible sample columns for long Events.
- [x] Add a regression test for a horizontally scrolled Event whose raw left edge lies behind the fixed header pane.


## r17 completed — HiDPI UI / stills / A-V detach / useful preview cache

- [x] Adaptive native logical UI scaling (1.0–1.75) for larger displays and high-resolution windows.
- [x] Win32 DPI-awareness entry point and scalable X11 text; richer native shadows/highlights without a web UI.
- [x] Still-image media (`png/jpg/jpeg/bmp/webp/tif/tiff`): five-second default Event, arbitrary right extension, persistent preview and full-duration render.
- [x] Undoable **Detach Audio from Video** context action for linked A/V groups.
- [x] Separate source-frame and composite caches so FX/Pan-Crop edits reuse the decoded source frame.
- [x] Dynamic Pan/Crop/FX playback stays on the persistent FFmpeg raw-video pipe and is composited in-process for realtime preview.
- [x] Seek-oriented all-I prepared preview cache for video >=20s; timeline thumbnails use it when ready.
- [x] Native/media regressions for A/V detach, still extension/render, persistent FX playback and source-cache reuse.
- [ ] Native GPU compositor with exact shader parity for effects/transforms.
- [ ] General random-seek decoder session, seek queue and frame prefetch.
- [ ] Native realtime audio mixer/master clock and final A/V sync architecture.


## r18 completed — dedicated FX editor / project-owned preview controls

- [x] Dedicated native Video Event FX window with effect catalog, event chain, preview and parameter inspector.
- [x] Horizontal numeric FX sliders with transient preview that does not mutate Project during drag.
- [x] One mouse-up = one Undo transaction for an FX slider gesture; Escape cancels and restores committed preview.
- [x] Preview quality, preview FPS cap and prepared-cache policy moved into ProjectSettings and `.kora` serialization.
- [x] Preview settings gear now opens Project Properties instead of application Preferences.
- [x] New native add/remove/reset/power/close glyphs plus refined button shadows/highlights.
- [ ] Expand non-double FX parameter widgets (choice/color/vec2/bool) and add keyframe curve UI inside the FX editor.

## r19 follow-up — Sv_intro75 compatibility target

The first `Sv_intro75` pass establishes a concrete VEGAS 12 regression target rather than treating the file as an opaque sample. Completed foundations are VEGAS 12 project settings, same-folder media relinking, WMV/ASF discovery, preservation of empty track slots, validated Event timing/Take offsets, Legacy Text placeholder events, opaque Event FX identity nodes, and imported audio-track FX identity references.

Next work should remain controlled/differential: implement native Legacy Text rendering only from validated project fields; identify and map the supplied Chroma Keyer, Color Corrector and Cookie Cutter parameters one field at a time; validate Noise Gate/Track EQ/Track Compressor parameter storage before implementing DSP import; then validate group/fade/envelope/track-level properties. Unsupported private fields must stay opaque rather than receiving guessed defaults that change the source project semantically.


## r20 follow-up — Titles & Text / recovery

Implemented in r20: native Titles & Text generated media; UTF-8 multiline text; Open Sans regular/bold rasterization; italic simulation; alignment; color presets; scale; normalized location and direct preview drag; 3x3 anchor; outline; shadow; basic fade animation; transactional Apply/Cancel; preview and final-render integration; 30-second recovery autosave; Save/Discard/Cancel close flow; modern dark UI pass.

Next text milestones: arbitrary RGBA color editor, additional animation presets/keyframes, per-character/rich-text runs, exact VEGAS legacy/title payload recovery only where differential experiments validate fields, and optional startup recovery discovery UI.


## r21 UI polish completed

- Modern GNOME/libadwaita-inspired dark visual language in the existing native Canvas renderer.
- Rounded cards/buttons/tabs and softer shadows/separators across the main workspace.
- Refreshed Preview, Project Media, Timeline, menus, context menus and editing dialogs.
- Keep future UI work behavioral and native; do not replace the C++ editor with a web/GTK shell.

## r22 completed — Anime Restore filters

- [x] Split the supplied restoration workflow into native Event/Track/Output FX rather than one monolithic Python tool.
- [x] Anime Line Restore with native realtime line-field detection/dark-line reconstruction controls.
- [x] Anime Vector Colors with smoothing, posterization, contrast/saturation and original blend.
- [x] Anime Text Restore, Paper Texture, Detail Sharpen and Deinterlace as separate reorderable filters.
- [x] New filters are first in the Video FX browser and work with the existing dedicated FX editor/sliders.
- [x] Native/media regressions cover catalog creation, combined realtime preview and final render graph.
- [ ] AnalysisJob implementation for ML character isolation (`u2net`-class models) without adding a Python runtime.
- [ ] Stateful temporal line confirmation on the prefetch compositor path.
- [ ] IVTC as a source/media preparation operation that preserves timeline and audio sync semantics.
- [ ] Native GPU kernels so final render and realtime preview use the identical Anime Restore algorithms.


## r23 completed — still-image Anime FX validation

- fixed random-access/stopped preview for JPG/PNG still media on FFmpeg 7.x;
- fixed odd-dimension still preview and thumbnails;
- added real preview regression for a 1125x631 JPEG with Anime Restore FX;
- validated the Anime Restore stages against the supplied anime thumbnail.


## OpenFX / Boris FX host

- [x] Standard OFX path discovery (`OFX_PLUGIN_PATH`, Windows Common Files, Linux `/usr/OFX/Plugins`).
- [x] Dynamic discovery of `OfxGetNumberOfPlugins` / `OfxGetPlugin` identities and Image Effect API filtering.
- [x] Boris FX / GenArts / Sapphire / Continuum identification in the native FX browser and diagnostics.
- [x] Synthetic OFX discovery regression fixture.
- [ ] Implement host Property suite and host capability descriptors.
- [ ] Implement Image Effect clips/images, render window and pixel-depth negotiation.
- [ ] Implement Parameter suite and map OFX parameter UI into the native FX editor.
- [ ] Implement Memory / MultiThread / Message suites required by production plug-ins.
- [ ] Validate with a legally installed Boris FX Continuum/Sapphire trial or licensed installation on Windows.
- [ ] Only after validation, enable add/render for third-party OFX nodes.

## r25 completed — free editing FX

Added a clean-room native starter pack based on broadly useful free-editor effect concepts: Glow, Hue Shift, Levels, Vignette, Pixelate/Mosaic, Lens Distortion, RGB Split, Letterbox and Scanlines. Next candidates for the same clean-room path are Gate Weave, row/block Glitch, Spill Suppression, Soft Glow with an exact multi-branch compositor node, and a proper denoise stage after the GPU compositor/analysis layers mature.


### r26 font consistency
- User-supplied Open Sans Regular/Italic now drives the generated native atlas.
- Win32 exact-family fallback uses the same atlas if Open Sans is not installed.
- Titles & Text uses true Open Sans Italic atlas data.

## r27 completed — recording / Audio FX

- [x] Record default microphone/input directly into the current project.
- [x] REC/STOP timeline control plus Insert -> Record Audio.
- [x] Safe raw PCM capture followed by native WAV finalization.
- [x] Automatically create Project Media + audio TrackEvent on stop with Undo support.
- [x] Separate Audio FX browser for Event and Track chains.
- [x] Echo/Delay, Room Reverb, Chorus, Flanger, Compressor, Limiter and Noise Gate.
- [x] 3-band EQ, High Pass, Low Pass, Distortion, Tremolo, Stereo Width and Gain.
- [x] Audio FX persistence in `.kora` v13 and regression coverage.
- [x] Audio FX in realtime preview and final render graph.
- [x] Conservative mapping of validated VEGAS audio-FX identities without guessing private parameters.
- [ ] Select/route named capture devices instead of default-input only.
- [ ] Input monitoring with controllable latency and feedback protection.
- [ ] Native realtime mixer/master clock replacing per-event ffplay preview processes.
- [ ] Audio buses, sends/returns, automation envelopes and VST3/OpenFX-style audio plug-in hosting.


## r29 completed — Windows test reliability

- [x] Visual Studio multi-config synthetic OFX fixture uses the standard `Contents/Win64` layout without an extra `Release/Debug` directory.
- [x] OFX unit test is isolated from real system-installed third-party plug-ins.
- [x] OFX fixture path no longer embeds the Unicode Windows build path as a narrow compile-time string.
- [x] Native test runner reports assertion exceptions instead of opaque MSVC fast-fail termination.
- [x] Remove the reported MSVC `C4244` byte-fill warning.
- [ ] Confirm r29 with the next real Windows/MSVC run.

## r31 completed: smooth native UI/rendering

- anti-aliased icon coverage atlas;
- fractional rounded-corner coverage;
- eased hover/press motion for native buttons;
- Win32 persistent GDI/AlphaBlend caches and preview conversion cache;
- X11 backbuffer + optional XRender alpha;
- redraw-on-demand animation clock instead of continuous idle repaint.


## r33 UI idle/performance pass

- Windows backbuffer is now a persistent top-down 32-bit DIBSection with direct software compositing for KoraLo UI rectangles, rounded-corner alpha coverage, lines, borders, icons and the generated Open Sans atlas. This removes thousands of tiny GDI/AlphaBlend calls from a full UI repaint.
- The Windows message loop uses `MsgWaitForMultipleObjectsEx` instead of an unconditional sleep, so an animation/preview invalidation can be painted immediately while idle still sleeps efficiently.
- Empty timelines no longer enter a pointless playback/redraw loop; an empty/no-video preview is cleared only when its visual state actually changes.
- Preview/video `StretchDIBits` remains native and is synchronized before software overlays continue.
- Goal: smooth 60 Hz hover/press animation without a permanent repaint loop, and low CPU while completely idle.

- [x] r34: per-monitor DPI driven Win32 UI scale and crack-free fractional rectangle mapping for 2K/4K displays.

## r36 VEGAS compatibility direction

- Native Chroma Key now produces alpha in preview/render and can reveal a lower video layer.
- Validated old Svfx Chroma Key named values are imported from observed `.veg` layouts.
- VEGAS Bézier Masking's five-slot OFX control schema is documented; curve `AnchorList` serialization and tracker state remain clean-room research targets requiring differential `.veg` fixtures.
- Full `.veg` compatibility remains incremental: preserve unknown state, validate one record/property at a time, and use installed OFX plug-ins when the host layer supports the required suites.
