#include <iiSocialIntegration/Publishing.h>
#include "CurlRuntime.h"
#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <memory>

namespace iiSocialIntegration {
namespace {
constexpr std::uint64_t maxBufferedResponse = 8 * 1024 * 1024;
constexpr std::uint64_t maxStreamedResponse = 5ULL * 1024 * 1024 * 1024;
struct Reader {
    std::ifstream stream;
    std::uint64_t start = 0, length = 0, position = 0;
    explicit Reader(const FileSlice& file) : stream(file.path, std::ios::binary), start(file.offset) {
        std::error_code ec;
        auto total = std::filesystem::file_size(file.path, ec);
        if (ec || start > total) throw std::runtime_error("Invalid file range");
        length = file.length.value_or(total - start);
        if (length > total - start || length > std::uint64_t(std::numeric_limits<curl_off_t>::max())) throw std::runtime_error("Invalid file length");
        stream.seekg(static_cast<std::streamoff>(start));
        if (!stream) throw std::runtime_error("Unreadable file");
    }
};
size_t read(char* buffer, size_t unit, size_t count, void* opaque) noexcept {
    auto& reader = *static_cast<Reader*>(opaque);
    if (unit && count > std::numeric_limits<size_t>::max() / unit) return CURL_READFUNC_ABORT;
    const auto bytes = std::min<std::uint64_t>(unit * count, reader.length - reader.position);
    reader.stream.read(buffer, static_cast<std::streamsize>(bytes));
    const auto got = reader.stream.gcount();
    if (std::uint64_t(got) != bytes) return CURL_READFUNC_ABORT;
    reader.position += got;
    return static_cast<size_t>(got);
}
int seek(void* opaque, curl_off_t offset, int whence) noexcept {
    auto& reader = *static_cast<Reader*>(opaque);
    if (whence != SEEK_SET || offset < 0 || std::uint64_t(offset) > reader.length) return CURL_SEEKFUNC_CANTSEEK;
    reader.stream.clear(); reader.stream.seekg(static_cast<std::streamoff>(reader.start + offset));
    if (!reader.stream) return CURL_SEEKFUNC_FAIL;
    reader.position = static_cast<std::uint64_t>(offset);
    return CURL_SEEKFUNC_OK;
}
struct Io {
    Response response;
    std::stop_token stop;
    std::size_t headerBytes = 0;
    std::uint64_t responseLimit = 0;
    const std::function<bool(std::string_view)>* responseSink = nullptr;
};
size_t write(char* data, size_t unit, size_t count, void* opaque) noexcept {
    try {
        auto& io = *static_cast<Io*>(opaque);
        if (unit && count > std::numeric_limits<size_t>::max() / unit) return 0;
        auto bytes = unit * count;
        if (io.responseSink && *io.responseSink && (io.response.status < 200 || io.response.status >= 300)) {
            // Keep redirect/error bodies separate from the final media budget.
            constexpr std::uint64_t discardedBodyLimit = 64 * 1024;
            if (bytes > discardedBodyLimit - io.response.responseBytes) { io.response.responseLimitExceeded = true; return 0; }
            io.response.responseBytes += bytes;
            return bytes;
        }
        if (bytes > io.responseLimit - io.response.responseBytes) { io.response.responseLimitExceeded = true; return 0; }
        if (io.responseSink && *io.responseSink) {
            try {
                if (!(*io.responseSink)(std::string_view(data, bytes))) { io.response.responseSinkFailed = true; return 0; }
            } catch (...) { io.response.responseSinkFailed = true; return 0; }
        } else io.response.body.append(data, bytes);
        io.response.responseBytes += bytes;
        return bytes;
    } catch (...) { return 0; }
}
size_t headers(char* data, size_t unit, size_t count, void* opaque) noexcept {
    try {
        auto& io = *static_cast<Io*>(opaque);
        if (unit && count > std::numeric_limits<size_t>::max() / unit) return 0;
        const auto bytes = unit * count;
        if (bytes > 65536 - io.headerBytes) return 0;
        io.headerBytes += bytes;
        std::string_view line(data, bytes);
        if (line.starts_with("HTTP/")) {
            io.response.headers.clear();
            const auto space = line.find(' ');
            if (space != line.npos && space + 3 < line.size() &&
                std::isdigit(static_cast<unsigned char>(line[space + 1])) &&
                std::isdigit(static_cast<unsigned char>(line[space + 2])) &&
                std::isdigit(static_cast<unsigned char>(line[space + 3])))
                io.response.status = (line[space + 1] - '0') * 100 + (line[space + 2] - '0') * 10 + (line[space + 3] - '0');
        }
        auto colon = line.find(':');
        if (colon != line.npos) {
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
            while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) value.remove_suffix(1);
            io.response.headers.emplace_back(line.substr(0, colon), value);
        }
        return bytes;
    } catch (...) { return 0; }
}
int progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return static_cast<Io*>(opaque)->stop.stop_requested() ? 1 : 0;
}
bool safeText(std::string_view value) {
    return std::ranges::none_of(value, [](unsigned char c) { return c < 32 || c == 127; });
}
} // namespace

Response CurlTransport::send(const Request& request, std::stop_token stop) {
    Response failed; failed.transportFailed = true; failed.cancelled = stop.stop_requested();
    if (failed.cancelled) return failed;
    if (!detail::initializeCurl()) return failed;
    try {
        using Url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
        Url url(curl_url(), curl_url_cleanup);
        if (!url || curl_url_set(url.get(), CURLUPART_URL, request.url.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) return failed;
        char* scheme = nullptr;
        if (curl_url_get(url.get(), CURLUPART_SCHEME, &scheme, 0) != CURLUE_OK) return failed;
        const bool secure = std::string_view(scheme) == "https"; curl_free(scheme);
        if (!secure || !safeText(request.url)) return failed;
        const auto limit = request.responseSink ? maxStreamedResponse : maxBufferedResponse;
        if (request.maxResponseBytes == 0 || request.maxResponseBytes > limit) return failed;
        if (request.method != "GET" && request.method != "POST" && request.method != "PUT") return failed;
        if ((!request.body.empty() && (request.file || !request.parts.empty())) || (request.file && !request.parts.empty())) return failed;
        using Easy = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
        Easy easy(curl_easy_init(), curl_easy_cleanup);
        if (!easy) return failed;
        using List = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;
        List list(nullptr, curl_slist_free_all);
        for (const auto& [key, value] : request.headers) {
            if (key.empty() || key.find(':') != key.npos || !safeText(key) || !safeText(value)) return failed;
            auto* next = curl_slist_append(list.get(), (key + ": " + value).c_str());
            if (!next) return failed;
            list.release(); list.reset(next);
        }
        Io io{{}, stop, 0, request.maxResponseBytes, &request.responseSink};
        CURLcode setup = CURLE_OK;
        auto set = [&](CURLoption option, auto value) {
            auto result = curl_easy_setopt(easy.get(), option, value);
            if (setup == CURLE_OK) setup = result;
        };
        set(CURLOPT_URL, request.url.c_str());
        set(CURLOPT_FOLLOWLOCATION, 0L);
        set(CURLOPT_SSL_VERIFYPEER, 1L); set(CURLOPT_SSL_VERIFYHOST, 2L);
        set(CURLOPT_SSLVERSION, long(CURL_SSLVERSION_TLSv1_2));
        const auto ca = certificateAuthority_.string();
        if (!ca.empty()) set(CURLOPT_CAINFO, ca.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
        set(CURLOPT_PROTOCOLS_STR, "https"); set(CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
        set(CURLOPT_PROTOCOLS, long(CURLPROTO_HTTPS)); set(CURLOPT_REDIR_PROTOCOLS, long(CURLPROTO_HTTPS));
#endif
        set(CURLOPT_NETRC, long(CURL_NETRC_IGNORED));
        set(CURLOPT_NOSIGNAL, 1L);
        set(CURLOPT_CONNECTTIMEOUT_MS, 10000L); set(CURLOPT_TIMEOUT_MS, 300000L);
        set(CURLOPT_LOW_SPEED_LIMIT, 64L); set(CURLOPT_LOW_SPEED_TIME, 60L);
        set(CURLOPT_USERAGENT, "iiSocialIntegration/0.1");
        set(CURLOPT_HTTPHEADER, list.get());
        set(CURLOPT_WRITEFUNCTION, &write); set(CURLOPT_WRITEDATA, &io);
        set(CURLOPT_HEADERFUNCTION, &headers); set(CURLOPT_HEADERDATA, &io);
        set(CURLOPT_XFERINFOFUNCTION, &progress); set(CURLOPT_XFERINFODATA, &io); set(CURLOPT_NOPROGRESS, 0L);
        using Mime = std::unique_ptr<curl_mime, decltype(&curl_mime_free)>;
        Mime mime(nullptr, curl_mime_free);
        std::vector<std::unique_ptr<Reader>> readers;
        if (!request.parts.empty()) {
            mime.reset(curl_mime_init(easy.get()));
            if (!mime) return failed;
            for (auto& part : request.parts) {
                if (part.name.empty() || !safeText(part.name) || !safeText(part.mimeType)) return failed;
                auto* item = curl_mime_addpart(mime.get());
                if (!item || curl_mime_name(item, part.name.c_str()) != CURLE_OK) return failed;
                if (!part.mimeType.empty() && curl_mime_type(item, part.mimeType.c_str()) != CURLE_OK) return failed;
                if (part.file) {
                    readers.push_back(std::make_unique<Reader>(*part.file));
                    auto* reader = readers.back().get();
                    // A fixed transport filename avoids leaking local paths and
                    // avoids platform-specific UTF-8 filename conversion.
                    if (curl_mime_filename(item, "media") != CURLE_OK || curl_mime_data_cb(item, static_cast<curl_off_t>(reader->length), &read, &seek, nullptr, reader) != CURLE_OK) return failed;
                } else if (curl_mime_data(item, part.value.data(), part.value.size()) != CURLE_OK) return failed;
            }
            set(CURLOPT_MIMEPOST, mime.get());
        } else if (request.file) {
            readers.push_back(std::make_unique<Reader>(*request.file));
            set(CURLOPT_UPLOAD, 1L); set(CURLOPT_READFUNCTION, &read); set(CURLOPT_READDATA, readers.front().get());
            set(CURLOPT_SEEKFUNCTION, &seek); set(CURLOPT_SEEKDATA, readers.front().get());
            set(CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(readers.front()->length));
        } else if (request.method != "GET") {
            set(CURLOPT_POSTFIELDS, request.body.data()); set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
        }
        set(CURLOPT_CUSTOMREQUEST, request.method.c_str());
        if (setup != CURLE_OK) return failed;
        const auto code = curl_easy_perform(easy.get());
        long status = 0, requestBytes = 0;
        curl_off_t uploaded = 0;
        curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(easy.get(), CURLINFO_REQUEST_SIZE, &requestBytes);
        curl_easy_getinfo(easy.get(), CURLINFO_SIZE_UPLOAD_T, &uploaded);
        io.response.status = static_cast<int>(status);
        io.response.transportFailed = code != CURLE_OK;
        io.response.cancelled = stop.stop_requested();
        io.response.mayHaveBeenSent = requestBytes > 0 || uploaded > 0;
        return std::move(io.response);
    } catch (...) { return failed; }
}
} // namespace iiSocialIntegration
