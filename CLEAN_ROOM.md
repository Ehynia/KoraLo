# KoraLo clean-room / independent implementation policy

KoraLo Native is an independent implementation.

The project may use public documentation, observable behavior, user-provided screenshots/reference packages, public file-format concepts and high-level architecture of other editors to determine expected workflows. Implementation code is written specifically for KoraLo.

KoraLo Native:

- does not use VEGAS program binaries as a runtime dependency;
- does not copy VEGAS implementation code;
- does not copy or link GPL Kdenlive source code;
- does not use MLT as its editing engine;
- does not depend on OpenCut at runtime;
- uses FFmpeg/ffprobe only as external decode/encode/probe backends;
- owns its project model, timeline semantics, drag transaction system, snap logic, preview scheduler and render graph generation.

The active application is C++20. There is no Python runtime in the current KoraLo source tree.

In r40 the spatial Anime restoration algorithm is adapted from the user-provided `1.py`, with separate provenance in THIRD_PARTY_NOTICES.md. The independent implementation policy above applies to external editor code; it does not imply that this user-provided algorithm was invented by KoraLo.

In r45, FSR 1 EASU/RCAS is explicitly adapted from AMD's MIT-licensed source. Its provenance and license are retained in THIRD_PARTY_NOTICES.md and licenses/AMD-FSR1-MIT.txt. The independent-editor implementation policy does not claim ownership of AMD's algorithm.
