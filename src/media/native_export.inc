// Included inside namespace koralo; shares the preview compositor helpers.
namespace {
unsigned native_worker_threads() {
  const unsigned hc = std::thread::hardware_concurrency();
  return std::clamp(hc ? hc : 4u, 2u, 16u);
}
bool exact_chain(const std::vector<EffectInstance> &effects) {
  return std::any_of(effects.begin(), effects.end(), is_native_exact_effect);
}
bool needs_native_export(const Project &p) {
  if (exact_chain(p.video_output_effects))
    return true;
  for (const auto &c : p.clips) {
    const auto *t = p.track_by_id(c.track_id);
    const auto *m = p.media_by_id(c.media_id());
    if (t && m && t->kind == TrackKind::Video &&
        (m->generated_vector.enabled || exact_chain(c.video_effects) || exact_chain(t->video_effects) ||
         exact_chain(m->video_effects)))
      return true;
  }
  return false;
}
bool bake_native_video(const std::string &ffmpeg, const std::string &source,
                       const std::string &target, int width, int height,
                       double fps, double start, double duration, double rate,
                       bool loop, const VideoMotion &motion,
                       const std::vector<EffectInstance> &effects,
                       const std::atomic<bool> *cancel, std::string *error,
                       const std::function<void(double)> &progress,
                       bool flatten = false, const VectorScene* vector_source=nullptr, double source_duration=0, int fsr_source_width=0, int fsr_source_height=0, float fsr_sharpness=.5f, RenderControlSession* control=nullptr, bool enable_cuda_fsr=false) {
  auto fail = [&](const std::string &message) {
    if (error)
      *error = message;
    return false;
  };
  auto cancelled = [&] { return cancel && cancel->load(); };
  if (cancelled())
    return fail("Render cancelled");
  if (width < 1 || height < 1 || !std::isfinite(duration) || duration <= 0 ||
      !std::isfinite(rate) || rate <= 0 || rate > 100 ||
      !std::isfinite(start) || start < 0)
    return fail("Invalid native effect segment");
  const std::size_t pixels = std::size_t(width) * height;
  if (pixels > 16 * 1024 * 1024)
    return fail("Native effects are limited to 16 megapixels per frame");
  const std::size_t frame_bytes = pixels * 4;
  const std::uint64_t count = std::uint64_t(std::ceil(duration * fps - 1e-7));
  if (!count)
    return fail("Empty native effect segment");
  const std::string worker_threads = std::to_string(native_worker_threads());
  std::vector<std::string> args{"-nostdin", "-v", "error", "-threads", worker_threads};
  if (is_still_image_path(source))
    args.insert(args.end(), {"-loop", "1", "-framerate", number_expr(fps)});
  else {
    if (loop)
      args.insert(args.end(), {"-stream_loop", "-1"});
    if (start > 0)
      args.insert(args.end(), {"-ss", number_expr(start)});
  }
  const int decode_width=fsr_source_width>0?fsr_source_width:width;
  const int decode_height=fsr_source_height>0?fsr_source_height:height;
  const std::size_t decode_pixels=std::size_t(decode_width)*decode_height;
  std::ostringstream vf;
  vf << "format=rgba,scale=" << decode_width << ":" << decode_height
     << ":force_original_aspect_ratio=decrease,pad=" << decode_width << ":" << decode_height
     << ":(ow-iw)/2:(oh-ih)/2:color=black@0,setpts=(PTS-STARTPTS)/" << rate
     << ",fps=" << fps << ",tpad=stop_mode=clone:stop=-1";
  args.insert(args.end(), {"-i", source, "-map", "0:V:0", "-an", "-filter_threads", worker_threads, "-vf",
                           vf.str(), "-frames:v", std::to_string(count),
                           "-pix_fmt", "rgba", "-c:v", "rawvideo", "-threads:v", worker_threads, "-f", "rawvideo", "pipe:1"});
  ReadPipeProcess decoder;
  if (!vector_source && !decoder.start(ffmpeg, args, error))
    return false;
  TemporaryFile pending_encoded(path_from_utf8(target).parent_path(), ".mkv");
  if(!pending_encoded.valid())return fail("Cannot reserve native encoder output");
  // One encoder process consumes frames in order while the worker pool renders
  // the next frames. This removes every raw temp batch, FFV1 segment and concat.
  ReadPipeProcess encoder;
  if(!encoder.start(ffmpeg,
      {"-nostdin","-v","error","-y","-f","rawvideo",
       "-pixel_format","rgba","-video_size",
       std::to_string(width)+"x"+std::to_string(height),
       "-framerate",number_expr(fps),"-i","pipe:0","-an",
       "-c:v","ffv1","-level","3","-threads",worker_threads,
       "-pix_fmt","bgra",path_to_utf8(pending_encoded.path())},
      error,false,true))return fail("Could not start continuous native FFV1 encoder: "+(error?*error:std::string{}));
  const unsigned frame_workers=export_frame_worker_count(frame_bytes);
  const unsigned inner_workers=export_inner_worker_count(frame_workers);
  AdaptiveRenderWorkers adaptive(frame_workers,frame_bytes,export_fixed_worker_override());
  OrderedFrameWorkers frame_pool(frame_workers);
  frame_pool.set_active_limit(adaptive.active());
  if(control){
    if(fsr_source_width>0)control->record("fsr_backend",enable_cuda_fsr ?
        "CUDA requested (per-frame CPU fallback on failure)" : "CPU");
    else if(!effects.empty())control->record("native_spatial_backend",enable_cuda_fsr ?
        "CUDA gaussian/morph where eligible; remaining effects on CPU" : "CPU");
    control->record("native_stage","source="+source+" frames="+std::to_string(count));
    control->update_workers(adaptive.active(),frame_workers,0,0);
  }
  std::deque<std::future<OrderedFrameWorkers::Pixels>> pending;
  std::uint64_t written = 0;
  const auto write_next = [&]() -> bool {
    std::vector<std::uint8_t> rgba;
    try {
        rgba = pending.front().get();
      } catch (const std::exception& ex) {
        pending.pop_front();
        return fail(cancelled() ? "Render cancelled" :
                  std::string("Native frame processing failed: ") + ex.what());
      }
      pending.pop_front();
      if (cancelled()) return fail("Render cancelled");
      if (rgba.size() != frame_bytes)
        return fail("Native frame worker returned incomplete pixels");
      if(!encoder.write_stdin_chunk(
          std::string_view(reinterpret_cast<const char*>(rgba.data()),rgba.size()),
          error,cancel))
        return fail(cancelled()?"Render cancelled":
                    "Native encoder stdin write failed: "+(error?*error:std::string{}));
      ++written;
      if (progress) progress(double(written) / count);
      if (adaptive.sample()) {
        frame_pool.set_active_limit(adaptive.active());
        if (control) control->update_workers(adaptive.active(),frame_workers,
                                           adaptive.cpu_percent(),adaptive.available_memory_mb());
      }
      return true;
    };
  for (std::uint64_t j = 0; j < count; ++j) {
      if (cancelled()) return fail("Render cancelled");
      // Read the one FFmpeg pipe on the producer thread only. Frame workers
      // never share decoder state, source pixels, or scratch buffers.
    std::vector<std::uint8_t> decoded;
      if (!vector_source) {
        decoded.resize(decode_pixels * 4);
        std::string decode_error;
        if (!decoder.read_exact(decoded.data(), decoded.size(), &decode_error, cancel, 30000))
          return fail(cancelled() ? "Render cancelled" :
                      "FSR/native source decode failed at frame " + std::to_string(j) +
                      "/" + std::to_string(count) +
                      " (" + std::to_string(decode_width) + "x" + std::to_string(decode_height) +
                      ", FFmpeg raw RGBA): " + decode_error +
                      ". Check input video and decoder settings.");
      }
      const double sample_time = start + (j) / fps * rate;
      const double effect_time = (j) / fps;
      pending.push_back(frame_pool.submit(
          [input = std::move(decoded), sample_time, effect_time, inner_workers,
           vector_source, source_duration, loop, width, height, decode_width,
           decode_height, decode_pixels, pixels, frame_bytes, fsr_source_width,
           fsr_sharpness, enable_cuda_fsr, &motion, &effects, cancel, flatten]() mutable {
            ExportInnerThreadBudget thread_budget(inner_workers);
            NativeCudaScope native_cuda_scope(enable_cuda_fsr);
            if (cancel && cancel->load())
              throw std::runtime_error("Render cancelled");
            Frame frame;
            if (vector_source) {
              const double vector_time = loop && source_duration > 0
                  ? std::fmod(sample_time, source_duration) : sample_time;
              frame = render_vector_scene(*vector_source, width, height, vector_time);
            } else {
              frame.width = decode_width;
              frame.height = decode_height;
              frame.rgb.resize(decode_pixels * 3);
              frame.alpha.resize(decode_pixels);
              for (std::size_t i = 0; i < decode_pixels; ++i) {
                frame.rgb[i * 3] = input[i * 4];
                frame.rgb[i * 3 + 1] = input[i * 4 + 1];
                frame.rgb[i * 3 + 2] = input[i * 4 + 2];
                frame.alpha[i] = input[i * 4 + 3];
              }
            }
            if (fsr_source_width > 0) {
              frame = upscale_fsr1(frame, width, height, fsr_sharpness, cancel, enable_cuda_fsr);
              if (frame.rgb.empty()) throw std::runtime_error("Render cancelled");
            }
            if (motion.enabled)
              apply_motion_cpu(frame, sample_motion(motion, effect_time));
            apply_effects_cpu(frame, effects, effect_time);
            if (flatten) flatten_alpha_over_black(frame);
            if (cancel && cancel->load())
              throw std::runtime_error("Render cancelled");
            if (frame.rgb.size() != pixels * 3 ||
                (!frame.alpha.empty() && frame.alpha.size() != pixels))
              throw std::runtime_error("Invalid processed frame size");
            std::vector<std::uint8_t> rgba(frame_bytes);
            for (std::size_t i = 0; i < pixels; ++i) {
              rgba[i * 4] = frame.rgb[i * 3];
              rgba[i * 4 + 1] = frame.rgb[i * 3 + 1];
              rgba[i * 4 + 2] = frame.rgb[i * 3 + 2];
              rgba[i * 4 + 3] = frame.alpha.empty() ? 255 : frame.alpha[i];
            }
            return rgba;
          }));
      // An at-most-N deep queue controls memory; drain in submitted order.
      if (pending.size() >= adaptive.active() && !write_next()) return false;
    }
    while (!pending.empty()) if (!write_next()) return false;
  decoder.terminate();
  encoder.close_stdin();
  while(encoder.running()) {
    if(cancelled()){encoder.terminate();return fail("Render cancelled");}
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if(encoder.exit_code()!=0)return fail("Continuous native FFV1 encoder failed (exit " +
                                      std::to_string(encoder.exit_code())+")");
  std::ifstream check(pending_encoded.path(),std::ios::binary);
  std::array<unsigned char,4> header{};
  check.read(reinterpret_cast<char*>(header.data()),header.size());
  if(!check || header!=std::array<unsigned char,4>{0x1a,0x45,0xdf,0xa3})
    return fail("Continuous native FFV1 output is invalid");
  check.close();
  if(cancelled())return fail("Render cancelled");
  if(control) control->record("native_stage_complete","frames="+std::to_string(written));
  return atomic_replace_file(pending_encoded.path(),path_from_utf8(target),error);
}
} // namespace
bool MediaEngine::render_native_project(const Project &p,
                                        const std::string &output,
                                        std::atomic<double> *progress,
                                        std::string *error,
                                        const RenderOptions *options,
                                        const std::atomic<bool> *cancel) const {
  const int width = options && options->width > 0 ? options->width
                                                  : p.settings.width,
            height = options && options->height > 0 ? options->height
                                                    : p.settings.height;
  const double fps =
      options && options->fps > 0 ? options->fps : p.settings.fps;
  Project prepared = p;
  std::vector<std::unique_ptr<TemporaryFile>> assets;
  const auto output_effects = p.video_output_effects;
  const bool native_output = exact_chain(output_effects);
  if (native_output)
    prepared.video_output_effects.clear();
  bool solo = false;
  for (const auto &t : p.tracks)
    if (t.kind == TrackKind::Video && t.solo)
      solo = true;
  std::size_t active = 0, finished = 0;
  for (const auto &c : p.clips) {
    auto *t = p.track_by_id(c.track_id);
    if (t && t->kind == TrackKind::Video && !t->mute && !c.muted &&
        (!solo || t->solo))
      ++active;
  }
  for (auto &c : prepared.clips) {
    const auto *track = p.track_by_id(c.track_id);
    const auto *media = p.media_by_id(c.media_id());
    if (!track || track->kind != TrackKind::Video || track->mute || c.muted ||
        (solo && !track->solo))
      continue;
    if (!media) {
      if (error)
        *error = "Missing event media";
      return false;
    }
    std::vector<EffectInstance> chain = media->video_effects;
    chain.insert(chain.end(), c.video_effects.begin(), c.video_effects.end());
    chain.insert(chain.end(), track->video_effects.begin(),
                 track->video_effects.end());
    if (chain.empty() && !media->generated_vector.enabled) {
      ++finished;
      continue;
    }
    std::string source = media->path;
    if (media->generated_text.enabled) {
      auto text =
          std::make_unique<TemporaryFile>(temporary_directory(), ".pam");
      if (!text->valid() ||
          !write_text_pam(path_to_utf8(text->path()), media->generated_text,
                          width, height)) {
        if (error)
          *error = "Cannot rasterize native text";
        return false;
      }
      source = path_to_utf8(text->path());
      assets.push_back(std::move(text));
    }
    auto video = std::make_unique<TemporaryFile>(temporary_directory(), ".mkv");
    if (!video->valid()) {
      if (error)
        *error = "Cannot reserve native clip";
      return false;
    }
    auto update = [&](double v) {
      if (progress)
        *progress = .6 * (finished + v) / std::max<std::size_t>(1, active);
    };
    if (!bake_native_video(ffmpeg_, source, path_to_utf8(video->path()), width,
                           height, fps, c.source_offset(), c.duration, c.rate,
                           c.loop, c.video_motion, chain, cancel, error,
                           update, false, media->generated_vector.enabled?&media->generated_vector:nullptr, media->duration,0,0,.5f,options?options->control:nullptr,
                           options?options->use_gpu&&gpu_enabled_flag_.load():gpu_enabled_flag_.load()))
      return false;
    MediaItem derived = *media;
    derived.id = 0;
    derived.path = path_to_utf8(video->path());
    derived.proxy_path.clear();
    derived.generated_text.enabled = false;
    derived.generated_vector = {};
    derived.video_effects.clear();
    derived.has_audio = false;
    derived.width = width;
    derived.height = height;
    derived.fps = fps;
    derived.duration = c.duration;
    derived.video_codec = "ffv1";
    int mid = prepared.add_media(std::move(derived)).id;
    c.takes = {{mid, 0, "Native effects"}};
    c.active_take = 0;
    c.rate = 1;
    c.loop = false;
    c.video_motion = {};
    c.video_effects.clear();
    assets.push_back(std::move(video));
    ++finished;
  }
  for (auto &t : prepared.tracks)
    if (t.kind == TrackKind::Video)
      t.video_effects.clear();
  RenderOptions nested = options ? *options : RenderOptions{};
  nested.native_prepared = true;
  if (!native_output)
    return render_project(prepared, output, progress, error, &nested, cancel);
  TemporaryFile base(temporary_directory(), ".mkv"),
      filtered(temporary_directory(), ".mkv");
  if (!base.valid() || !filtered.valid()) {
    if (error)
      *error = "Cannot reserve native master";
    return false;
  }
  nested.lossless_intermediate = true;
  nested.verify_output = false;
  std::atomic<double> compose_progress{0};
  auto composing=std::async(std::launch::async,[&]{return render_project(prepared,path_to_utf8(base.path()),&compose_progress,error,&nested,cancel);});
  while(composing.wait_for(std::chrono::milliseconds(30))!=std::future_status::ready)
    if(progress)progress->store(std::max(progress->load(),.6+.05*std::clamp(compose_progress.load(),0.0,1.0)));
  if(!composing.get())return false;
  if (!bake_native_video(
          ffmpeg_, path_to_utf8(base.path()), path_to_utf8(filtered.path()),
          width, height, fps, 0, p.duration(), 1, false, {}, output_effects,
          cancel, error,
          [&](double v) {
            if (progress)
              *progress = .65 + .3 * v;
          },
          true,nullptr,0,0,0,.5f,options?options->control:nullptr,
          options?options->use_gpu&&gpu_enabled_flag_.load():gpu_enabled_flag_.load()))
    return false;
  std::error_code ec;
  auto destination = fs::absolute(path_from_utf8(output), ec);
  TemporaryFile encoded(destination.parent_path(),
                        path_to_utf8(destination.extension()));
  if (!encoded.valid()) {
    if (error)
      *error = "Cannot reserve native output";
    return false;
  }
  ProcessOptions execution;
  execution.timeout_ms = 0;
  execution.cancel = cancel;
  std::string pending;
  execution.on_output=[&](std::string_view chunk){
    pending.append(chunk);std::size_t newline;
    while((newline=pending.find('\n'))!=std::string::npos){
      const auto line=pending.substr(0,newline);pending.erase(0,newline+1);
      if(progress&&line.rfind("out_time_us=",0)==0)try{
        progress->store(std::max(progress->load(),.95+.049*std::clamp(std::stod(line.substr(12))/1000000/std::max(.001,p.duration()),0.0,1.0)));
      }catch(...){}
    }
    if(pending.size()>65536)pending.erase(0,pending.size()-65536);
  };
  const auto run_final_encode = [&](bool hardware) {
    std::vector<std::string> encode_args={"-nostdin","-v","error","-nostats","-progress","pipe:1","-y",
      "-i",path_to_utf8(filtered.path()),"-i",path_to_utf8(base.path()),"-map","0:v:0","-map","1:a?"};
    if(options&&options->lossless_intermediate)
      encode_args.insert(encode_args.end(),{"-c:v","ffv1","-level","3","-threads",std::to_string(native_worker_threads()),"-pix_fmt","bgra"});
    else if(hardware && nested.use_gpu && gpu_enabled_flag_.load() && gpu_.encode_available)
      encode_args.insert(encode_args.end(),{"-c:v",gpu_.encoder,"-b:v",std::to_string(std::clamp(nested.video_bitrate_kbps,500,200000))+"k","-pix_fmt","yuv420p","-movflags","+faststart"});
    else
      encode_args.insert(encode_args.end(),{"-c:v","libx264","-preset","veryfast","-threads",std::to_string(native_worker_threads()),"-b:v",std::to_string(std::clamp(nested.video_bitrate_kbps,500,200000))+"k","-pix_fmt","yuv420p","-movflags","+faststart"});
    encode_args.insert(encode_args.end(),{"-c:a","copy","-t",number_expr(p.duration()),path_to_utf8(encoded.path())});
    return run_process(ffmpeg_,encode_args,execution);
  };
  if(nested.control)nested.control->record("encoder_selected",
      nested.use_gpu&&gpu_enabled_flag_.load()&&gpu_.encode_available?gpu_.encoder:"libx264");
  auto result=run_final_encode(true);
  if ((!result.started || result.exit_code != 0) && !result.cancelled && nested.use_gpu && gpu_enabled_flag_.load() && gpu_.encode_available){
    if(nested.control)nested.control->record("encoder_fallback","Hardware encode failed, retry libx264");
    result=run_final_encode(false);
  }
  if (!result.started || result.exit_code != 0) {
    if (error)
      *error = result.cancelled ? "Render cancelled" : result.output;
    return false;
  }
  if ((options?options->verify_output:true) && !verify_rendered_video(ffprobe_,ffmpeg_,encoded.path(),
         width,height,p.duration(),cancel,error,
         [&]{std::string e;const auto video=probe(path_to_utf8(base.path()),&e);return video&&video->has_audio;}(),
         nested.control)) return false;
  if (!atomic_replace_file(encoded.path(), destination, error))
    return false;
  if (progress)
    *progress = 1;
  return true;
}

// FSR operates on the composed project-resolution image, never on a previously
// bilinear-upscaled image. Lossless intermediates preserve the original sources.
bool MediaEngine::render_fsr_project(const Project& p,const std::string& output,
    std::atomic<double>* progress,std::string* error,const RenderOptions& options,const std::atomic<bool>* cancel)const{
    TemporaryFile base(temporary_directory(),".mkv"),scaled(temporary_directory(),".mkv");
    if(!base.valid()||!scaled.valid()){if(error)*error="Cannot reserve FSR intermediate files";return false;}
    auto stage=[&](double begin,double span,auto action){
        std::atomic<double> current{0};
        auto task=std::async(std::launch::async,[&]{return action(&current);});
        while(task.wait_for(std::chrono::milliseconds(30))!=std::future_status::ready)
            if(progress)progress->store(std::max(progress->load(),begin+span*std::clamp(current.load(),0.0,.999)));
        const bool ok=task.get();if(ok&&progress)progress->store(begin+span);return ok;
    };
    RenderOptions source=options;source.fsr_upscale=false;source.width=p.settings.width;source.height=p.settings.height;
    source.lossless_intermediate=true;source.use_gpu=false;source.verify_output=false;
    if(!stage(0,.35,[&](auto* value){return render_project(p,path_to_utf8(base.path()),value,error,&source,cancel);}))return false;
    const int width=options.width>0?options.width:p.settings.width,height=options.height>0?options.height:p.settings.height;
    const double fps=options.fps>0?options.fps:p.settings.fps;
    if(!bake_native_video(ffmpeg_,path_to_utf8(base.path()),path_to_utf8(scaled.path()),width,height,fps,0,p.duration(),1,false,{}, {},cancel,error,
        [&](double value){if(progress)progress->store(.35+.5*value);},true,nullptr,0,p.settings.width,p.settings.height,options.fsr_sharpness,options.control,options.use_gpu&&gpu_enabled_flag_.load()))return false;
    Project final_project;final_project.settings=p.settings;
    MediaItem video;video.path=path_to_utf8(scaled.path());video.has_video=true;video.duration=p.duration();video.width=width;video.height=height;video.fps=fps;
    const int mid=final_project.add_media(video).id,vt=final_project.add_track(TrackKind::Video).id;
    final_project.add_clip(mid,vt,0,0,p.duration());
    std::string probe_error;const auto mixed=probe(path_to_utf8(base.path()),&probe_error);
    if(!mixed){if(error)*error=probe_error;return false;}
    if(mixed->has_audio){const int aid=final_project.add_media(*mixed).id,at=final_project.add_track(TrackKind::Audio).id;final_project.add_clip(aid,at,0,0,p.duration());}
    RenderOptions final_options=options;final_options.fsr_upscale=false;
    return stage(.85,.15,[&](auto* value){return render_project(final_project,output,value,error,&final_options,cancel);});
}
