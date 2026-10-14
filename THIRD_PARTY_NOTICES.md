# Third-party / reference notices

## FFmpeg

KoraLo invokes FFmpeg and ffprobe as external codec/media tools. FFmpeg is a separate project with its own LGPL/GPL licensing depending on build configuration. KoraLo does not claim FFmpeg code as its own.

## Kdenlive

Kdenlive (KDE) is licensed under GPL terms. KoraLo does not contain or link Kdenlive or MLT code. Public Kdenlive developer documentation and source declarations were studied to understand mature NLE architecture and timeline interaction patterns such as backend-authorized editing, fake drag moves, snapping, and undo transactions.

Reference: https://github.com/KDE/kdenlive

## OpenCut

OpenCut has been used as another architectural reference for separating editor UI and media-engine responsibilities. KoraLo does not require OpenCut at runtime.

Reference: https://github.com/opencut-app/opencut

## VEGAS reference material

User-supplied VEGAS files/screenshots were treated as behavior/UI reference material. KoraLo does not load or redistribute VEGAS executables, DLLs, plug-ins, codec binaries or implementation code.

## Font Awesome Free icon reference pack

The user supplied a small icon pack derived from Font Awesome Free 6.4.0 names used by the UI mockup. The SVG files, manifest, and the supplied Font Awesome license are retained under `assets/icons/fontawesome/` for future native icon rendering. The SVG pack remains reference material; r41 uses the generated native icon atlas and existing utility fallback glyphs.

## Inter and historical Open Sans assets

r41 uses Inter by the Inter Project Authors, distributed under SIL Open Font License 1.1. Font binaries and the complete license are in `assets/fonts/inter/`; an additional license copy is in `licenses/Inter-OFL-1.1.txt`. Source: https://github.com/google/fonts/tree/main/ofl/inter . `scripts/generate_font_atlas.py` produces the embedded 16-level coverage atlas used on X11, Win32 and Titles & Text. Runtime use does not require an installed font or a font service.

Previous releases used the user-supplied Open Sans raster atlas (Copyright 2020 The Open Sans Project Authors, https://github.com/googlefonts/opensans). r41 retains that atlas for existing Titles & Text whose saved family is Open Sans; application UI and new titles use Inter. Its Apache 2.0 notice remains in `licenses/OpenSans-Apache-2.0.txt`.

## Generated r41 icon artwork

36 original toolbar pictograms were generated with the built-in image-generation service. The raw art, source PNG crops, grid manifest, prompts and development-time atlas compiler are under `assets/icons/generated/` and `scripts/generate_icon_atlas.py`. The generated coverage is embedded and tintable. Utility glyphs outside this set retain the existing geometric/Font Awesome fallback and its notices. No generation service is used at runtime.

## Boris FX / OpenFX discovery

KoraLo does not bundle Boris FX Continuum, Sapphire, Mocha, Boris FX libraries, plug-in binaries, presets, or license material. The r24 compatibility work implements an independent native OpenFX bundle/ABI discovery layer based on the public OpenFX specification. Actual third-party OFX rendering remains disabled until the required host suites are implemented and validated with a user-installed plug-in.

## frei0r / free-filter research references

Public frei0r/GStreamer/Kdenlive effect catalogs were consulted in r25 to identify common free editing-effect concepts such as Glow, Hue Shift, Lens Correction, Levels, Letterbox and RGB Split. KoraLo does not bundle, link, translate, or copy frei0r/Kdenlive GPL source code. The r25 effects are independent KoraLo implementations using generic image-processing math and the existing external FFmpeg render bridge.

## r38 portable runtimes

The optional offline package includes separate FFmpeg programs and their runtime libraries. Windows: FFmpeg 9.0.2 essentials build from https://www.gyan.dev/ffmpeg/builds/ (GPLv3); exact source commit and full build configuration are preserved in `licenses/ffmpeg/README.txt`. FFmpeg source: https://github.com/FFmpeg/FFmpeg/commit/946fcce07b . Linux: FFmpeg 6.1.1-3ubuntu5 and libraries from the packaging environment; distributor copyright files and common licenses are preserved under `licenses/linux-distribution/` and `licenses/common-licenses/`. FFmpeg upstream sources: https://ffmpeg.org/releases/ffmpeg-6.1.1.tar.xz ; Ubuntu package sources: https://launchpad.net/ubuntu/+source/ffmpeg/7:6.1.1-3ubuntu5 . These components retain their own licenses independently of KoraLo's MIT license.

The old utility icon atlas uses Font Awesome reference assets; its license is retained. Historical Open Sans artifacts retain the Apache 2.0 notice in `licenses/OpenSans-Apache-2.0.txt`; Inter has its own OFL 1.1 notice. No VEGAS DLLs, applications or proprietary plug-ins are included.

## Anime Line Restore V45 / user source

The user supplied `1.py` (Anime Line Restore V45). r40 adapts its spatial image-processing algorithm in `src/media/native_effects.cpp`. The Python environment bootstrap and process/file cleanup code are not included or run. CUDA, temporal gating and rembg are not included. The texture generator is a deterministic variant.

## Krita and Photopea

Krita brush/mask workflows were studied using https://docs.krita.org/en/reference_manual/tools/freehand_brush.html . Krita as a whole is GPLv3: https://krita.org/en/about/license/ . No Krita source is copied or linked in r40. The native mask stroke implementation is independent and does not require Qt/Krita. Photopea core source is not available in its public issue repository: https://github.com/photopea/photopea . No Photopea code is included.

## r42 interface graphics

The vector model, rasterizer, incremental mask and OpenGL Canvas are native KoraLo code. GLX/WGL and OpenGL entry points are supplied dynamically by the operating system graphics driver; no Mesa DRI modules or LLVM libraries downloaded for r42 QA were added to the portable package. The existing Inter, Open Sans and generated icon notices remain applicable. No Photopea or Krita source was copied into this implementation.

## AMD FidelityFX Super Resolution 1 (r45)

`src/media/fsr_upscaler.cpp` is a scalar float32 CPU adaptation of AMD's EASU and RCAS algorithms, not an independently invented upscale filter. The downloaded reference snapshot is retained at `third_party/fsr1/ffx_fsr1.h`. Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved. Distributed under the MIT license; see `licenses/AMD-FSR1-MIT.txt`.

Source: https://github.com/GPUOpen-Effects/FidelityFX-FSR/blob/master/ffx-fsr/ffx_fsr1.h
Documentation: https://gpuopen.com/fidelityfx-superresolution/

KoraLo uses scalar floating-point arithmetic in place of shader-specific approximate reciprocals/gathers and bilinear alpha scaling. This is a CPU adaptation, not an AMD-certified GPU implementation. FSR 2/3 temporal upscaling and frame generation are not included.
