#pragma once

#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <string>

namespace persona1 {

// Sound for the native port: sceAudio (real output to the Windows audio device),
// sceSasCore (software voice mixer: PSP-ADPCM / PCM voices, envelopes) and
// sceAtrac3plus (music, decoded with FFmpeg when it was found at build time).
//
// Environment switches:
//   PERSONA1_NOAUDIO=1     no sound device (game is paced by the clock instead)
//   PERSONA1_VOLUME=0..200 master volume in percent (default 100)
//   PERSONA1_AUDIOLOG=1    log SAS / ATRAC calls (first few of each) to stderr
void install_audio(psprecomp::Runtime &rt);

// True if this module implements the import, so the host must not install a stub over it.
bool audio_handles(const std::string &library, std::uint32_t nid);

// Stops the audio device thread. Safe to call more than once.
void audio_shutdown();

} // namespace persona1
