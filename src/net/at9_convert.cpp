// ps5fwdgen - Convert audio to ATRAC9 through the forwarder site.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "net/at9_convert.hpp"

#include "net/http.hpp"

namespace net
{

namespace
{

constexpr const char *kConvertUrl = "https://ps5-forwarder.mph.am/api/convert-at9";

} // namespace

bool convert_to_at9(const std::vector<unsigned char> &audio, const std::string &filename,
                    std::vector<unsigned char> &at9, std::string &error)
{
    if (audio.size() > kAt9UploadLimit)
    {
        error = "the file is too large to convert (2 MB at most)";
        return false;
    }
    // The site converts before it answers, which takes a while for a long clip.
    const Response response = post_file(kConvertUrl, "file", filename, audio, 180000);
    const bool looks_at9 = response.body.size() > 12 && response.body[0] == 'R' &&
                           response.body[1] == 'I' && response.body[2] == 'F' &&
                           response.body[3] == 'F';
    if (response.status == 200 && looks_at9)
    {
        at9 = response.body;
        return true;
    }
    if (response.status == 0)
        error = response.error.empty() ? "conversion failed" : response.error;
    else
        error = "conversion HTTP " + std::to_string(response.status);
    return false;
}

} // namespace net
