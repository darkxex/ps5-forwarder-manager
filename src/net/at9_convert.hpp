// ps5fwdgen - Convert audio to ATRAC9 through the forwarder site.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console has no ATRAC9 encoder the app can use, so audio is uploaded to
// the site's /api/convert-at9 endpoint, as ps5-forwarder.mph.am does. The
// endpoint takes WAV, MP3, OGG, FLAC and M4A/AAC (not WebM) up to about
// 2 MiB, and answers with a RIFF ATRAC9 file. Blocking; worker thread only.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace net
{

// The largest upload the endpoint accepts, with some room under its limit
// (2,071,415 bytes converted and 2,136,061 were refused when measured).
constexpr std::size_t kAt9UploadLimit = 2000000;

// Converts audio (filename's extension tells the format) to ATRAC9. On
// success fills at9 and returns true; otherwise sets error.
bool convert_to_at9(const std::vector<unsigned char> &audio, const std::string &filename,
                    std::vector<unsigned char> &at9, std::string &error);

} // namespace net
