# r38 status update

Current r42: native vector animation workspace, transform/opacity/mask/crop keys, cubic easing, incremental brush rasterization and OpenGL UI (GLX/WGL) are implemented. Vector rasterization and Anime remain CPU; point morphing, the full GPU compositor and OFX rendering remain pending. Linux GUI and software OpenGL were exercised; Windows was cross-built only. See `docs/R42_ANIMATION_GPU_RU.md`.

Linux/Windows x64 portable packages are included in the offline distribution. Linux native/core/media/runtime tests and native dialog integration were validated; Windows was cross-compiled, not executed on a real Windows machine. New atomic save/export, recovery, relative project paths, collect/relink and CLI diagnostics are implemented. Full OFX rendering and complete VEGAS compatibility remain unavailable. Historical matrix below documents earlier feature work.

# KoraLo parity matrix

This is a development matrix, not a claim of binary/API compatibility with VEGAS. KoraLo reimplements selected NLE workflows in its own native C++20 architecture.

| Area | KoraLo status | Notes |
|---|---|---|
| Native Windows/Linux application | Implemented | Win32 and X11 frontends exist; current validation is Linux-only. |
| Native startup branding | Implemented (Windows) | Supplied KoraLo logo is compiled as a splash bitmap; supplied badge is compiled as a multi-size executable/window/taskbar icon. Linux packaging keeps the branded PNG asset; X11 splash integration remains optional follow-up. |
| Workspace UI shell | Implemented / evolving | Native icon toolbar, Project Media tree/grid/search/filter, Preview transport/Trimmer, timeline toolbar and track controls are interactive. r17 adds adaptive logical HiDPI scaling, DPI-aware Win32 startup, size-aware X11 fonts, and layered native shadows/highlights. Docking/resizable panes and full subsystem editors remain. |
| Multitrack video/audio timeline | Implemented | Native `Project`/`Track`/`TrackEvent` model; `Clip` is a transitional source alias. |
| Still-image media | Implemented first pass | PNG/JPEG/BMP/WebP/TIFF import, 5s default Event, arbitrary extension, continuous preview and full-duration render. |
| Linked A/V groups | Implemented / hardening | Group-aware selection/move/trim/slip; split now creates independent left/right linked groups. Ignore Event Grouping is available. |
| Detach linked A/V | Implemented | Event context action clears the group for linked video/audio in one undoable transaction so streams can be edited independently. |
| Selection model | Implemented | Dedicated `SelectionModel`, multi-select, linked-group expansion. |
| Marquee selection | Implemented | Replace or additive marquee. |
| Move | Implemented | Preview-first transaction, snapping, multi-selection. |
| Trim left/right | Implemented | Group-aware and transaction-based. |
| Slip | Implemented | Group-aware source offset edit. |
| Roll | Implemented | Touching-edge roll with source/duration constraints. |
| Slide | Implemented | Touching-neighbor slide; preserves selected event source range. |
| Ripple Trim | Implemented | Explicit drag mode with downstream preview/commit. |
| Stretch | Planned | Deferred. |
| Track reorder | Implemented | Transactional; constrained to same track kind. |
| Undo/redo | Implemented | One edit gesture = one project transaction; track controls, locks, clipboard edits, Insert/Overwrite and project toggles participate. |
| Snapping | Implemented | Project start, playhead, markers, other event edges. |
| Clipboard Cut/Copy/Paste | Implemented | Internal event clipboard preserves relative timing and remaps linked groups on paste; one paste/cut is one undo transaction. |
| Event/track lock | Implemented | Locked events/tracks block destructive timeline edits; lock state is undoable and serialized. |
| Track mute/solo/level/pan | Implemented / evolving | Controls are interactive; level/pan dragging is preview-first and commits once on mouse-up; mute/solo, video opacity, audio gain/pan feed preview visibility or final render as applicable. |
| Markers | Partial | Marker command and snapping/serialization exist; marker management/regions remain. |
| Split/delete/ripple delete | Implemented / evolving | Group-aware split creates a separate right-side group; no-selection Split cuts events at the cursor. Delete/ripple delete remain transactional. |
| Project save/load | Implemented | Native KoraLo project format; filesystem boundary uses UTF-8-safe native paths and regression coverage includes a non-ASCII project filename. |
| Media import/probe | Implemented | ffprobe-backed metadata; native shell-free process launch with UTF-8/UTF-16 path boundary on Windows; compact delimiter parser with selected-stream key/value fallback. |
| Preview cache/scheduler | Implemented / expanded | Composite LRU + r17 raw source-frame LRU + async preparation. FX/Pan-Crop parameter changes can reuse the decoded source frame; a genuinely uncached arbitrary source seek can still launch one exact FFmpeg process. |
| Persistent decoder engine | Working sequential path / evolving seeks | r15 keeps one FFmpeg raw-video pipe alive; r17 keeps dynamic Pan/Crop/FX playback on it and composites in-process. Random seek/scrub still needs the generalized reusable seek/prefetch session. |
| Proxy creation | Implemented | FFmpeg backend. |
| Final render | Implemented / evolving | KoraLo builds render behavior; FFmpeg executes codecs/filters. |
| Source Trimmer | Partial / implemented UI | Source selection, frame stepping, source In/Out, Insert and Overwrite now target the Media/Take/TrackEvent model. Timeline range targeting remains. |
| Timeline In/Out | Planned next | Needed for insert/overwrite/render ranges. |
| Insert / Overwrite | Partial / implemented | Initial transactional edits split/trim target ranges, preserve linked A/V groups and are undoable; Take/Event and timeline-range semantics remain. |
| Media -> Take -> TrackEvent model | Implemented / migrating callers | Multi-take TrackEvent model, active take, offsets, looping and project v6 serialization exist; `Clip` remains a compatibility alias while callers migrate. |
| Fades/transitions | Functional event fades / partial transitions | Direct mouse Fade In/Fade Out handles are transaction-based and affect video preview/render plus audio meter/preview/render. Auto Crossfade remains functional; dedicated transition objects/editor and alternate fade curve shapes remain. |
| Media/Event/Track/Output FX layers | Planned | Effect graph comes after edit/playback hardening. |
| Effect definitions/instances/parameters | Planned | Typed animatable parameter model required. |
| Analysis jobs/cache | Planned | For stabilization/optical flow and similar effects. |
| Compositor node graph | Planned | Must be independent from FFmpeg command generation. |
| GPU compositor | Planned | After persistent playback engine and graph model. |
| OpenColorIO/ACES | Planned | Separate color-management layer. |
| OFX/VST hosting | Long-term | After core edit/playback architecture stabilizes. |

## r4 delta

| Area | KoraLo r4 status | Notes |
|---|---|---|
| Realtime preview audio | Implemented / interim | Active audio events play through shell-free ffplay child processes; mute/solo/gain/pan/rate and event fade envelopes are honored. Planned native realtime mixer/audio-master-clock will replace this backend. |
| Playback decode optimization | Working sequential session / evolving seeks | Quality throttling + request coalescing + separate raw/composite LRUs + persistent raw-video decoder for consecutive playback, including dynamic FX/Pan-Crop in r17. Uncached random source seeks remain exact fallback decodes. |
| Main menus | Implemented / evolving | Native File/Edit/View/Insert/Tools/Options/Help surface exposes implemented commands only. |
| Project Properties | Partial / functional | Resolution, fps, sample rate and common presets are editable and undoable; advanced color/interlace/pixel settings remain. |
| Preferences | Partial / functional | Audio preview, preview quality, snapping and automatic crossfades; broader device/cache settings remain. |
| Open Sans UI typography | Implemented with system fallback | Native font request; font files are not bundled. |
| Event Pan/Crop | Implemented core / first native editor | Draggable frame, corner zoom, rotation, event-local keyframes, undo/redo, split continuity, preview and final render. Masks/Bézier/Track Motion remain. |


## r6 delta — editing-semantics research

| Area | KoraLo r6 status | Notes |
|---|---|---|
| Takes | Implemented core | Multiple Takes per TrackEvent, active-take switching, independent Media/Offset. |
| Group split | Implemented | Linked group splits into independent left/right groups in one undo transaction. |
| Ignore Event Grouping | Implemented | Selection/edit expansion can be disabled without destroying group metadata. |
| Slip All Takes | Implemented | Normal Slip changes active Take; option moves every Take offset. |
| Event Loop | Implemented core | Preview/render wrap or loop finite media when an event is extended. |
| Quantize to Frames | Implemented | Move/trim/roll/slide/ripple-trim/split/insert/overwrite boundaries quantize to project FPS. |
| Trim Start/End to Cursor | Implemented | Group-aware, source-offset-correct and undoable. |
| Vertical group move | Implemented | Relative same-kind track movement is preserved for linked/multi-selected events. |
| Snap Offset | Partial | Stored and participates as a snap target/drag anchor; dedicated UI handle remains. |


## r7 delta — Event Pan/Crop / spatial keyframes

| Area | KoraLo r7 status | Notes |
|---|---|---|
| VideoMotion model | Implemented | Parameter-owned Center X/Y, Zoom and Rotation keyframes. |
| Native Pan/Crop editor | Implemented first pass | Frame drag, corner zoom, rotation buttons, key timeline and add/delete/reset. |
| Transactional interaction | Implemented | Project mutates once on mouse-up. |
| Split animation continuity | Implemented | Left/right events receive sampled boundary state and shifted right keys. |
| Preview motion | Implemented | Async preview request carries sampled event motion. |
| Render motion | Implemented | Animated FFmpeg filter expressions for center/zoom/rotation. |
| Bézier/masks/perspective | Not yet | Planned on the same parameter/keyframe infrastructure. |

## r8 compatibility additions

| Area | r8 status | Notes |
|---|---|---|
| Master audio meter | Partial/working | Real cached source envelope; native realtime mixer/master clock still planned |
| Event Pan/Crop timeline feedback | Working | Motion lane + key diamonds on event; editor/preview/render already functional |
| Project Media explorer tree | Working baseline | All visible tree rows clickable; bins/tags data models remain future work |
| Direct `.veg` open | Experimental | Validated settings/media refs; conservative single-online-source timeline only |
| Full `.veg` round trip | Not claimed | Original VEG stays read-only; save as `.kora` |

## r9 delta — GPU assist / thumbnails / FX foundation

| Area | KoraLo r9 status | Notes |
|---|---|---|
| GPU preview decode | Partial / working bridge | FFmpeg hardware acceleration is attempted when capability is detected; CPU fallback remains mandatory. Persistent decoder sessions are still planned. |
| GPU proxy/render encode | Partial / working bridge | Hardware H.264 encoders are attempted where FFmpeg exposes them; failure falls back to software encoding. |
| Native GPU compositor | Not yet | Planned after persistent decoder + graph/frame abstraction. |
| Timeline video thumbnails | Implemented first pass | Async cached real frames; respects Take offset, playback rate, loop and trim preview. |
| EffectDefinition/ParameterDefinition/EffectInstance | Implemented foundation | Generic native model, v8 serialization, animatable numeric parameter keys. |
| Event FX | Functional first pass | Add/toggle/remove/edit Brightness/Contrast, Saturation and Gaussian Blur. |
| Media FX | Model/render foundation | Chain exists; richer UI is pending. |
| Track FX | Partial | Chain exists and render path is wired; current per-event approximation will move to the compositor graph. |
| Video Output FX | Model/render foundation | Final-stage chain exists; richer UI/presets pending. |
| Background analysis effects | Planned | Stabilization/optical-flow class effects will use a separate AnalysisJob/cache subsystem. |

## r10 delta — timeline viewport / VEG TrackEvent timing / Video FX browser

| Area | KoraLo r10 status | Notes |
|---|---|---|
| Timeline vertical viewport | Working | Track rows are clipped, wheel-scrollable and use scroll-aware hit testing; visible scrollbar added. |
| Timeline fit/zoom | Working baseline | Zoom floor is 8 px/s; direct VEG import fits recovered project duration once. |
| Pan/Crop event-local timing | Hardened | Editor shows local + project time; render resets source PTS before local keyframe evaluation. |
| Direct `.veg` TrackEvent timing | Experimental / working for validated shape | Supplied project recovers two events with start/length/Take offsets in original track-list order. |
| `.veg` fades/groups/envelopes/FX | Research only | Not guessed; pending controlled differential projects. |
| Video FX browser | Functional baseline | Searchable list + preset panel + Event FX chain + enable/remove/numeric parameters. |
| VEGAS built-in FX parity | Partial | Only independently implemented KoraLo effects are exposed; no proprietary effect code is copied. |

- Timeline navigation: 12-hour horizon + horizontal scrollbar + middle-mouse pan — implemented r11.
- Event Pan/Crop rotation handle/keyframes — implemented r11.
- Drag Event FX from browser to timeline — implemented r11.
- Render As editable native presets — first implementation r11.

## r12 delta — preview/cache UX

| Area | KoraLo r12 status | Notes |
|---|---|---|
| Preview quality | Functional | Draft 320x180, Preview 640x360, Good 960x540, Best/Full; not just a label change. |
| Preview FPS | Functional | User-selectable 15/24/30/60 cap, additionally bounded by the quality profile. |
| Long-form import preparation | Functional bridge | Async representative-frame warmup + audio analysis + r17 all-I seek-oriented preview cache for >=20s video. Timeline thumbnails use the prepared cache when available. |
| Cache progress UI | Functional | Bottom progress/stage indicator asks user to wait while preparation is active. |
| Cache GPU assist | Partial / fallback-safe | Attempts detected FFmpeg GPU backend; retries CPU when hardware cache generation fails. |
| Settings/render transparency | Functional Win32 | True alpha blend on the GDI back buffer; X11 uses a native stipple transparency fallback. |
| Splash startup progress | Functional Win32 | Loading/check phase text and progress bar during initialization. |
| Persistent decoder sessions | Expanded in r17 | Sequential playback including dynamic Pan/Crop/FX reuses one decoder process with in-process preview compositing; generalized random seek/prefetch sessions remain. |

## r13 delta — MSVC portability

| Area | KoraLo r13 status | Notes |
|---|---|---|
| MSVC UI command dispatch | Hardened | Monolithic `else-if` dispatcher split into modal/edit/FX/workspace/track handlers to avoid C1061 compiler nesting limit. |
| Shadow-warning parity | Hardened | Reported MSVC C4458/C4456 fixed; GCC/Clang now build `koralo_core` with `-Wshadow`. |
| Functional behavior | Intended unchanged | r13 is a structural compiler-portability pass over r12 behavior. |
| Windows validation | Pending user confirmation | This environment still cannot run Visual Studio/MSVC. |


## r14 additions
| Area | Status | Notes |
| --- | --- | --- |
| Right-click Event menu | Implemented | Context-sensitive, real edit commands/undo |
| Right-click Timeline menu | Implemented | Import/paste/tracks/marker/preferences |
| Right-click Project Media | Implemented | Trimmer/timeline/proxy |
| Runtime language packs | Implemented foundation | External UTF-8 `.lang`, English fallback, Win32 Unicode; Linux non-Latin renderer upgrade pending |

## r15 delta — direct fades / playback throughput

| Area | KoraLo r15 status | Notes |
|---|---|---|
| Event Fade In / Fade Out handle | Working | Drag top event handle inward/outward to any frame-quantized duration; transaction preview + one mouse-up Undo commit. |
| Fade visual / tooltip | Working | Live diagonal fade line, movable top handle, localized offset tooltip/status. |
| Video fade preview | Working | Event fade envelope multiplies event/track opacity over the preview background. |
| Audio fade preview/meter | Working interim | ffplay volume expression and cached Master-meter envelope both apply event fade gain. |
| Persistent playback decoder | Working common path | One FFmpeg process streams raw RGB frames across sequential playback; large seek restarts and GPU failure falls back to CPU. |
| Dynamic FX/motion persistent path | Working CPU preview path in r17 | Sequential Pan/Crop/FX frames stay on the persistent raw decoder and are composited in-process; final render remains authoritative. |


## r16 delta — timeline viewport correctness

| Area | KoraLo r16 status | Notes |
|---|---|---|
| Fixed track-header pane | Corrected | Horizontally scrolled Events are clipped at the content boundary and no longer paint over headers. |
| Header/event hit isolation | Corrected | Hidden Event geometry behind the header cannot receive trim/move/fade clicks. |
| Long audio Event drawing | Optimized | Waveform columns are generated only for the visible horizontal viewport. |


## r17 delta — HiDPI / still media / detachable A-V / preview cache

| Area | KoraLo r17 status | Notes |
|---|---|---|
| Large-resolution native UI | Working | Logical UI scale 1.0–1.75, Win32 DPI awareness, size-aware cached X11 fonts, native shadows/highlights. |
| Still images | Working first pass | PNG/JPEG/BMP/WebP/TIFF are video-only media, default 5s, extendable and looped for preview/render. |
| Detach Audio from Video | Working | Context action breaks linked A/V group in one Undo transaction. |
| Source-frame cache | Working | Raw source frame survives composite invalidation, avoiding decoder respawn during stationary FX/Pan-Crop edits. |
| Dynamic FX/Pan-Crop playback | Working CPU path | Persistent raw decoder stays alive and KoraLo applies supported preview transform/effects in-process. Some sampling/effect details are approximations of final render. |
| Prepared seek cache | Improved | Built for video >=20s as all-I H.264 with GPU attempt/CPU fallback; timeline filmstrips consume it when ready. |
| Random seek session | Still planned | An uncached arbitrary source seek can spawn one exact FFmpeg decode; reusable seek queue/prefetch remains future work. |


## r18 delta — effect editing UX / project preview configuration

| Area | KoraLo r18 status | Notes |
|---|---|---|
| Video Event FX editor | Working native window | Effect catalog + event chain + large live preview + enable/remove + numeric parameter inspector. |
| FX parameter sliders | Transactional | Drag updates a transient effect copy used only for preview; Project changes on mouse-up in one Undo command. |
| FX preview during slider drag | Working | Up to ~30 preview requests/s; source/composite caches remain available and final render stays authoritative. |
| Project preview settings | Working / serialized | Quality, FPS limit and prepared-cache policy live in ProjectSettings and `.kora` v10 rather than application-wide preferences. |
| UI icon/control polish | Expanded | Added add/remove/reset/power/close glyphs and layered button hover/active treatment. |
| Non-double FX controls | Pending | Bool/choice/color/vec2/string/group/action widgets remain to be promoted into the dedicated editor. |

## r19 delta — Open Sans / VEGAS 12 Sv_intro75 import

| Area | KoraLo r19 status | Notes |
|---|---|---|
| Open Sans UI | Working native path | Win32 requests system Open Sans; X11 normal UI text uses a generated Open Sans raster atlas. No TTF/OTF font is bundled. |
| VEGAS 12 project settings | Working for supplied fixture | Recovers 1920x1080, 23.976 fps and 48 kHz from the observed VEGAS 12 record layout while retaining VEGAS 17 support. |
| Same-folder media relink | Working | Old absolute VEGAS media paths are relinked by basename beside the `.veg`; all five referenced `Sv_intro75` media files relink in the supplied archive. |
| WMV / ASF media references | Working | Import recognition and Windows file dialog include WMV/ASF; FFmpeg remains the codec backend. |
| Empty VEGAS track slots | Preserved | The supplied project imports all eight track positions rather than collapsing tracks that contain no recovered Event. |
| Legacy Text | Structure preserved, rendering pending | Generated-media Event is retained with duration/timing and a native placeholder Media item; exact text generator rendering is not yet implemented. |
| VEGAS Event FX identities | Preserved opaque | Chroma Keyer, Color Corrector and Cookie Cutter identities are attached as disabled compatibility nodes; private parameter blobs are not guessed. |
| VEGAS audio Track FX identities | Preserved opaque | Noise Gate, Track EQ and Track Compressor names survive native `.kora` v11 save/load; DSP/parameter mapping is pending. |
| Fades/groups/envelopes from Sv_intro75 | Pending validation | No unverified private field is interpreted. |


## r20 delta — generated text / autosave / close safety

| Area | KoraLo r20 status | Notes |
| --- | --- | --- |
| Insert -> Text / Generated Media | Implemented | Creates native 5 s generated video event at the cursor; duration remains timeline-editable |
| Titles & Text editor | Implemented foundation | UTF-8 multiline text, Open Sans, size, bold/italic, alignment, color presets, scale, location, anchor, outline, shadow, fade animation |
| Direct text positioning | Implemented | Drag inside the editor preview; remains temporary until Apply |
| Text preview | Implemented | Native Open Sans raster overlay, including text-only projects on a black project frame |
| Text final render | Implemented | Transparent RGBA generated input participates in event/track opacity, fades, motion and FX graph |
| Legacy VEGAS generated text | Partial | Detected event becomes editable KoraLo text; private VEGAS text parameter payload is not guessed |
| Autosave | Implemented | Dirty project recovery copy every 30 s in KoraLo `autosave/` with temp fallback |
| Dirty close prompt | Implemented | Save / Discard / Cancel; cancelled save keeps application open |
| UI modernization | Improved | Updated native dark palette, buttons, pane chrome and project/preferences modal |


## Native UI / r21

| Area | Status | Notes |
|---|---|---|
| Modern dark visual system | Implemented | Shared native Canvas palette/surfaces; Open Sans retained |
| Rounded controls/cards/tabs | Implemented | Native raster geometry; no browser/GTK dependency |
| Main workspace panels | Implemented | Project Media, Preview and Timeline refreshed |
| Menus/dialogs | Implemented | Menus, context menus, properties, FX/Text/Pan-Crop/Render styling refreshed |

## r22 delta — Anime Restore native filter pack

| Feature | Status | Notes |
|---|---|---|
| Anime Line Restore | Functional native preview / render approximation | Separate Event/Track/Output FX with Strength, Detector Threshold, Line Width, Edge Hardness, Original Line Color and Max Luma. |
| Anime Vector Colors | Functional native preview / render approximation | Saturation, Contrast, Fill Smoothing, Color Levels and Original Blend. |
| Anime Text Restore | Functional native preview / render approximation | Local text-zone sharpening/boost with threshold and boost controls. |
| Anime Paper Texture | Functional | Deterministic texture + warmth in preview; FFmpeg noise/color-balance render mapping. |
| Anime Detail Sharpen | Functional | Independent post-sharpen stage. |
| Anime Deinterlace | Functional | Lightweight preview interpolation; final render uses `bwdif`. |
| Character isolation (rembg/u2net) | Planned analysis effect | Not faked; requires AnalysisJob/model/cache integration without Python runtime. |
| Temporal line confirmation | Planned temporal compositor stage | Needs previous-frame state/prefetch rather than a stateless event filter. |
| IVTC | Planned source-processing operation | Must update source timing/duration safely; not inserted as a timeline-desynchronizing Event FX. |


## r23 delta — still-image media path

| Area | r23 status |
|---|---|
| JPG/PNG stopped preview | Working without input seek on single-frame media |
| Odd-size stills | 1125x631 random-access preview regression covered |
| Anime Restore on stills | Real async preview path covered |
| Deinterlace on progressive photos | Supported but intentionally user-selected; not auto-applied |


## r24 delta — realtime FX optimization / OpenFX host foundation

| Area | KoraLo r24 status | Notes |
|---|---|---|
| Simple FX playback | Optimized | Consecutive pixel-only color operations are fused into one frame scan. |
| Heavy FX playback | Adaptive | Temporary lower realtime resolution keeps cadence; full selected quality returns on pause/stop. |
| FX slider editing | Optimized | Source cache retained; reusable work buffers reduce allocations; stale finished frames are discarded. |
| OpenFX discovery | Implemented beta | Scans OFX_PLUGIN_PATH and standard OS locations, dynamically identifies Image Effect plug-ins. |
| Boris FX detection | Implemented beta | Boris/GenArts/Sapphire/Continuum-style OFX identifiers are surfaced in the Video FX registry/diagnostics. |
| Boris FX / external OFX rendering | Not yet enabled | Full OFX host suites and real vendor plug-in validation are still required before third-party pixels are trusted. |
| OFX regression | Implemented | Synthetic OFX module is built/tested to validate bundle scanning and exported identity discovery. |

## r25 free editing FX

- [x] Glow
- [x] Hue Shift
- [x] Levels
- [x] Vignette
- [x] Pixelate / Mosaic
- [x] Lens Distortion
- [x] RGB Split
- [x] Letterbox
- [x] Scanlines
- [x] Native realtime preview for all nine
- [x] Combined FFmpeg final-render regression
- [x] No frei0r/Kdenlive GPL code linked or copied


| Native Open Sans UI / Titles atlas | Done (r26: user-supplied Regular/Italic source, Win32 fallback + X11 direct atlas) |

## r27 delta — audio recording / Audio FX

| Area | KoraLo r27 status | Notes |
|---|---|---|
| Record microphone/input in editor | Implemented | REC/STOP on timeline + Insert command; selected/first audio track target. |
| Recording file safety | Implemented | Capture writes raw PCM; native C++ finalizes PCM WAV on stop. |
| Windows input backend | Implemented, not host-validated | FFmpeg DirectShow enumeration/capture; this exact r27 was not built/tested on Windows. |
| Linux input backend | Implemented | FFmpeg PulseAudio default input with ALSA fallback; no physical device in headless test environment. |
| Event Audio FX | Implemented | Persisted in `.kora` v13 and used by realtime/final audio path. |
| Track Audio FX | Implemented | Persisted in `.kora` v13 and applied after Event Audio FX. |
| Echo / Reverb / Chorus / Flanger | Implemented | Editable native nodes backed by FFmpeg DSP for preview/render. |
| Dynamics | Implemented | Compressor, Limiter and Noise Gate. |
| EQ / filters | Implemented | 3-band EQ, High Pass and Low Pass. |
| Creative / utility | Implemented | Distortion, Tremolo, Stereo Width and Gain. |
| VEGAS audio-FX import | Partial/controlled | Noise Gate, Track EQ and Compressor identities map disabled; original unknown parameters are not guessed. |
| Native low-latency mixer/record monitor | Planned | Current realtime playback/FX continues through shell-free ffplay processes; dedicated mixer/master clock remains future work. |


## r29 Windows validation delta

| Area | r29 status | Notes |
|---|---|---|
| MSVC compile error from r27 | Fixed | r28 already produced all executables. |
| `koralo_media_tests` on user Windows host | Passed in r28 user run | 17.75 s in the supplied log. |
| Synthetic OFX fixture layout | Fixed in r29 | No Visual Studio `Release/Debug` nesting under `Contents/Win64`. |
| Native OFX regression isolation | Fixed in r29 | Test scans only the synthetic bundle, not real installed OFX plug-ins. |
| Opaque `0xc0000409` native-test failure | Hardened | Exceptions are now printed and return a normal test failure code. |
| Exact r29 Windows run | Pending | Must be confirmed on the user's Visual Studio host. |

## UI/render responsiveness (r31)

| Capability | Status |
|---|---|
| Anti-aliased native toolbar icons | Implemented |
| Smooth rounded native controls | Implemented |
| Hover/press easing | Implemented |
| Idle redraw-on-demand | Implemented |
| Win32 persistent paint resource cache | Implemented |
| X11 Pixmap backbuffer | Implemented |
| XRender translucent edges when available | Implemented |


## r33 UI idle/performance pass

- Windows backbuffer is now a persistent top-down 32-bit DIBSection with direct software compositing for KoraLo UI rectangles, rounded-corner alpha coverage, lines, borders, icons and the generated Open Sans atlas. This removes thousands of tiny GDI/AlphaBlend calls from a full UI repaint.
- The Windows message loop uses `MsgWaitForMultipleObjectsEx` instead of an unconditional sleep, so an animation/preview invalidation can be painted immediately while idle still sleeps efficiently.
- Empty timelines no longer enter a pointless playback/redraw loop; an empty/no-video preview is cleared only when its visual state actually changes.
- Preview/video `StretchDIBits` remains native and is synchronized before software overlays continue.
- Goal: smooth 60 Hz hover/press animation without a permanent repaint loop, and low CPU while completely idle.
