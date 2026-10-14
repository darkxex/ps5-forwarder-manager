// ps5fwdgen - A tiny blocking HTTPS GET, for use on a worker thread.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "net/http.hpp"

#include "net/console_curl.h"

#include <curl/curl.h>

#include <mutex>

namespace net
{

namespace
{

std::once_flag g_once;

std::size_t on_body(char *data, std::size_t size, std::size_t count, void *user)
{
    const std::size_t total = size * count;
    auto *out = static_cast<std::vector<unsigned char> *>(user);
    out->insert(out->end(), reinterpret_cast<unsigned char *>(data),
                reinterpret_cast<unsigned char *>(data) + total);
    return total;
}

// Appends up to a limit, then stops the transfer (a write error curl reports,
// which perform_get treats as success once the limit is reached).
struct LimitedBody
{
    std::vector<unsigned char> *out = nullptr;
    std::size_t max = 0;
    bool full = false;
};

std::size_t on_limited_body(char *data, std::size_t size, std::size_t count, void *user)
{
    const std::size_t total = size * count;
    auto *body = static_cast<LimitedBody *>(user);
    const std::size_t room = body->max - body->out->size();
    const std::size_t take = total < room ? total : room;
    body->out->insert(body->out->end(), reinterpret_cast<unsigned char *>(data),
                      reinterpret_cast<unsigned char *>(data) + take);
    if (body->out->size() >= body->max)
    {
        body->full = true;
        return take == total ? total : 0;
    }
    return total;
}

Response perform_get(const std::string &url, const std::string &bearer, long timeout_ms,
                     std::size_t max_bytes, std::size_t offset = 0, long stall_ms = 0)
{
    global_init();
    Response response;
    CURL *easy = curl_easy_init();
    if (easy == nullptr)
    {
        response.error = "curl_easy_init failed";
        return response;
    }
    console_curl_setup(easy);
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "ps5fwdgen/1.0");

    LimitedBody limited{&response.body, max_bytes, false};
    std::string range;
    if (max_bytes > 0)
    {
        range = std::to_string(offset) + "-" + std::to_string(offset + max_bytes - 1);
        curl_easy_setopt(easy, CURLOPT_RANGE, range.c_str());
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_limited_body);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &limited);
    }
    else
    {
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response.body);
    }

    if (stall_ms > 0)
    {
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, (stall_ms + 999) / 1000);
    }

    struct curl_slist *headers = nullptr;
    std::string auth;
    if (!bearer.empty())
    {
        auth = "Authorization: Bearer " + bearer;
        headers = curl_slist_append(headers, auth.c_str());
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    }

    char error_buffer[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error_buffer);

    const CURLcode code = curl_easy_perform(easy);
    const bool cut_at_limit = code == CURLE_WRITE_ERROR && limited.full;
    response.curl_code = cut_at_limit ? 0 : static_cast<int>(code);
    if (code == CURLE_OK || cut_at_limit)
    {
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response.status);
    }
    else
    {
        response.status = 0;
        response.error = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(code);
        // The address the name resolved to: a refused 0.0.0.0 or 127.0.0.1
        // means a DNS blocklist is in the way, not the server.
        char *ip = nullptr;
        if (curl_easy_getinfo(easy, CURLINFO_PRIMARY_IP, &ip) == CURLE_OK && ip != nullptr &&
            ip[0] != '\0')
            response.error += std::string(" [") + ip + "]";
    }

    if (headers != nullptr)
        curl_slist_free_all(headers);
    curl_easy_cleanup(easy);
    return response;
}

} // namespace

void global_init()
{
    std::call_once(g_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

Response get(const std::string &url, const std::string &bearer, long timeout_ms)
{
    return perform_get(url, bearer, timeout_ms, 0);
}

Response get_range(const std::string &url, std::size_t offset, std::size_t length, long timeout_ms,
                   long stall_ms)
{
    if (length == 0)
        return {};
    return perform_get(url, {}, timeout_ms, length, offset, stall_ms);
}

Response post_file(const std::string &url, const std::string &field, const std::string &filename,
                   const std::vector<unsigned char> &data, long timeout_ms)
{
    global_init();
    Response response;
    CURL *easy = curl_easy_init();
    if (easy == nullptr)
    {
        response.error = "curl_easy_init failed";
        return response;
    }
    console_curl_setup(easy);
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "ps5fwdgen/1.0");
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response.body);

    curl_mime *mime = curl_mime_init(easy);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, field.c_str());
    curl_mime_filename(part, filename.c_str());
    curl_mime_data(part, reinterpret_cast<const char *>(data.data()), data.size());
    curl_easy_setopt(easy, CURLOPT_MIMEPOST, mime);

    char error_buffer[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error_buffer);

    const CURLcode code = curl_easy_perform(easy);
    response.curl_code = static_cast<int>(code);
    if (code == CURLE_OK)
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response.status);
    else
    {
        response.status = 0;
        response.error = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(code);
        // The address the name resolved to: a refused 0.0.0.0 or 127.0.0.1
        // means a DNS blocklist is in the way, not the server.
        char *ip = nullptr;
        if (curl_easy_getinfo(easy, CURLINFO_PRIMARY_IP, &ip) == CURLE_OK && ip != nullptr &&
            ip[0] != '\0')
            response.error += std::string(" [") + ip + "]";
    }
    curl_mime_free(mime);
    curl_easy_cleanup(easy);
    return response;
}

namespace
{

Response perform_post(const std::string &url, const unsigned char *data, std::size_t size,
                      const std::string &content_type, long timeout_ms, const char *protocols)
{
    global_init();
    Response response;
    CURL *easy = curl_easy_init();
    if (easy == nullptr)
    {
        response.error = "curl_easy_init failed";
        return response;
    }
    console_curl_setup(easy);
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, protocols);
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "ps5fwdgen/1.0");
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, reinterpret_cast<const char *>(data));
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(size));

    struct curl_slist *headers = nullptr;
    const std::string ctype = "Content-Type: " + content_type;
    headers = curl_slist_append(headers, ctype.c_str());
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);

    char error_buffer[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error_buffer);

    const CURLcode code = curl_easy_perform(easy);
    response.curl_code = static_cast<int>(code);
    if (code == CURLE_OK)
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response.status);
    else
    {
        response.status = 0;
        response.error = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(code);
        // The address the name resolved to: a refused 0.0.0.0 or 127.0.0.1
        // means a DNS blocklist is in the way, not the server.
        char *ip = nullptr;
        if (curl_easy_getinfo(easy, CURLINFO_PRIMARY_IP, &ip) == CURLE_OK && ip != nullptr &&
            ip[0] != '\0')
            response.error += std::string(" [") + ip + "]";
    }
    if (headers != nullptr)
        curl_slist_free_all(headers);
    curl_easy_cleanup(easy);
    return response;
}

} // namespace

Response post_json(const std::string &url, const std::string &body, long timeout_ms)
{
    return perform_post(url, reinterpret_cast<const unsigned char *>(body.data()), body.size(),
                        "application/json", timeout_ms, "https");
}

Response post_bytes(const std::string &url, const std::vector<unsigned char> &data,
                    const std::string &content_type, long timeout_ms)
{
    return perform_post(url, data.data(), data.size(), content_type, timeout_ms, "http,https");
}

} // namespace net
