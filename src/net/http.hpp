// ps5fwdgen - A tiny blocking HTTPS GET, for use on a worker thread.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app elevates, so sceHttp/sceSsl reject public certificates; this uses
// libcurl + OpenSSL through console_curl, as the boilerplate's CURL guide
// prescribes. Never call it on the render thread.
#pragma once

#include <string>
#include <vector>

namespace net
{

// Call once, from one thread, before any get().
void global_init();

struct Response
{
    long status = 0;   // HTTP status, or 0 on a transport error
    int curl_code = 0; // CURLcode (0 = ok)
    std::string error; // human-readable, when status == 0
    std::vector<unsigned char> body;
};

// A blocking HTTPS GET. bearer, when non-empty, is sent as
// "Authorization: Bearer <bearer>". Follows redirects (https only).
Response get(const std::string &url, const std::string &bearer = {}, long timeout_ms = 20000);

// A blocking HTTPS GET of bytes [offset, offset + length) of a file: it asks
// the server for just that range and stops reading at its end if the server
// sends the whole file anyway (status is then 200, else 206). Made for large
// downloads in chunks: rather than a fixed deadline, it gives up when the
// transfer stalls (under 1 KB/s for stall_ms) or after timeout_ms in all.
Response get_range(const std::string &url, std::size_t offset, std::size_t length,
                   long timeout_ms = 120000, long stall_ms = 20000);

// A multipart/form-data POST of one file field. field is the form field name,
// filename the upload filename, data the file bytes. For the forwarder site's
// /api/convert-at9 endpoint. Blocking; worker thread only.
Response post_file(const std::string &url, const std::string &field, const std::string &filename,
                   const std::vector<unsigned char> &data, long timeout_ms = 60000);

// An HTTPS POST of a JSON body ("Content-Type: application/json"), as
// YouTube's InnerTube API takes. Blocking; worker thread only.
Response post_json(const std::string &url, const std::string &body, long timeout_ms = 20000);

// A raw POST of a byte body (http or https). Dev-only: used to ship debug
// screenshots to the build host, which has no TLS. Blocking; worker/dev use.
Response post_bytes(const std::string &url, const std::vector<unsigned char> &data,
                    const std::string &content_type = "application/octet-stream",
                    long timeout_ms = 15000);

} // namespace net
