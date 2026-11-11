#pragma once

#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <string>

namespace persona1 {

// Movie playback: the sceMpeg library. The game feeds PSMF (MPEG-PS) data through a ring buffer
// whose refill callback is a guest function; this module demuxes it, decodes H.264 video and
// ATRAC3+ audio with FFmpeg and hands frames back through sceMpegAvcDecodeYCbCr / AvcCsc and
// sceMpegAtracDecode. Needs FFmpeg at build time; without it movies are skipped silently.
//
// PERSONA1_MOVIELOG=1 logs every sceMpeg call (first few of each) to stderr.
void install_movie(psprecomp::Runtime &rt);
bool movie_handles(const std::string &library, std::uint32_t nid);

} // namespace persona1
