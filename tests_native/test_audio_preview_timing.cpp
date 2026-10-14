#include "media/audio_preview_timing.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace koralo;
static void require(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
static bool near(double a,double b){return std::abs(a-b)<1e-5;}
int main(){
    MediaItem m;m.has_audio=true;m.duration=10;m.path="music.wav";
    Clip clip;clip.start=5.0;clip.duration=12.0;clip.rate=1.0;
    clip.takes={{1,2.0,"Audio take"}};clip.active_take=0;
    auto first=audio_preview_window(clip,m,5.0);
    require(first&&near(first->source_offset,2)&&near(first->timeline_length,8),"source bounds cap preview length");
    auto late=audio_preview_window(clip,m,14.0);
    require(!late,"do not replay exhausted nonlooping audio");
    clip.rate=2.0;
    auto fast=audio_preview_window(clip,m,6.0);
    require(fast&&near(fast->source_offset,4)&&near(fast->source_length,6)&&near(fast->timeline_length,3),"rate-aware audio bound");
    clip.loop=true;auto looped=audio_preview_window(clip,m,11.0);
    require(looped&&near(looped->source_offset,4)&&near(looped->source_length,12),"loop wraps at source boundary");
    require(!audio_preview_window(clip,m,4.0),"audio not active before clip");
    require(!audio_preview_window(clip,m,17.0),"audio not active after clip");
    std::cout<<"Audio preview bounds regression passed\n";
}
