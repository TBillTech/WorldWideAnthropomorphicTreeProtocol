#include <fstream>
#include <ev.h>
#include <sys/mman.h>

#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/mman.h>
#include <libgen.h>
#include <netinet/udp.h>

#include <urlparse.h>


#include "config_base.h"
#include "server_base.h"
#include "server.h"
#include "quic_listener.h"
#include "network.h"
#include "debug.h"
#include "util.h"
#include "shared.h"
#include "http.h"
#include "template.h"
#include "urlparse.h"
#include "../../../libraries/ngtcp2/lib/ngtcp2_pkt.h"

using namespace ngtcp2;
using namespace std::literals;

// TODO: Fix LEGACY ISSUE (s)

namespace
{
    constexpr size_t NGTCP2_SV_SCIDLEN = 18;
} // namespace

namespace
{
    constexpr size_t max_preferred_versionslen = 4;
} // namespace

namespace
{
    constexpr size_t NGTCP2_STATELESS_RESET_BURST = 100;
} // namespace

namespace
{
    auto randgen = util::make_mt19937();
} // namespace

Stream::Stream(int64_t stream_id, Handler *handler)
    : stream_id(stream_id),
      handler(handler),
      data(nullptr),
      datalen(0),
      dynresp(false),
      dyndataleft(0),
      dynbuflen(0),
      live_stream(false),
      partial_chunk(global_no_chunk_header, false) {}

namespace
{
    constexpr auto NGTCP2_SERVER = "nghttp3/ngtcp2 server"sv;
} // namespace

namespace
{
    std::string make_status_body(unsigned int status_code)
    {
        auto status_string = util::format_uint(status_code);
        auto reason_phrase = http::get_reason_phrase(status_code);

        std::string body;
        body = "<html><head><title>";
        body += status_string;
        body += ' ';
        body += reason_phrase;
        body += "</title></head><body><h1>";
        body += status_string;
        body += ' ';
        body += reason_phrase;
        body += "</h1><hr><address>";
        body += NGTCP2_SERVER;
        body += " at port ";
        body += util::format_uint(config.port);
        body += "</address>";
        body += "</body></html>";
        return body;
    }
} // namespace

namespace
{
    Request request_path(const std::string_view &uri, bool is_connect)
    {
        bool is_wwatp = uri.find("wwatp/") != std::string_view::npos;
        string default_static = "/index.html";
        if (is_wwatp)
        {
            default_static = "/";
        }
        urlparse_url u;
        Request req{
            .scheme = "",
            .authority = "",
            .path = "",
            .pri =
                {
                    .urgency = -1,
                    .inc = -1,
                },
        };

        if (auto rv = urlparse_parse_url(uri.data(), uri.size(), is_connect, &u);
            rv != 0)
        {
            return req;
        }

        if (u.field_set & (1 << URLPARSE_PATH))
        {
            req.path = std::string(uri.data() + u.field_data[URLPARSE_PATH].off,
                                   u.field_data[URLPARSE_PATH].len);
            if (req.path.find('%') != std::string::npos)
            {
                req.path = util::percent_decode(std::begin(req.path), std::end(req.path));
            }
            if (!req.path.empty() && req.path.back() == '/')
            {
                req.path += default_static.substr(1);
            }
        }
        else
        {
            req.path = default_static;
        }

        req.path = util::normalize_path(req.path);
        if (req.path == "/")
        {
            req.path = default_static;
        }

        if (u.field_set & (1 << URLPARSE_QUERY))
        {
            static constexpr auto urgency_prefix = "u="sv;
            static constexpr auto inc_prefix = "i="sv;
            auto q = std::string(uri.data() + u.field_data[URLPARSE_QUERY].off,
                                 u.field_data[URLPARSE_QUERY].len);
            for (auto p = std::begin(q); p != std::end(q);)
            {
                if (util::istarts_with(p, std::end(q), std::begin(urgency_prefix),
                                       std::end(urgency_prefix)))
                {
                    auto urgency_start = p + urgency_prefix.size();
                    auto urgency_end = std::find(urgency_start, std::end(q), '&');
                    if (urgency_start + 1 == urgency_end && '0' <= *urgency_start &&
                        *urgency_start <= '7')
                    {
                        req.pri.urgency = *urgency_start - '0';
                    }
                    if (urgency_end == std::end(q))
                    {
                        break;
                    }
                    p = urgency_end + 1;
                    continue;
                }
                if (util::istarts_with(p, std::end(q), std::begin(inc_prefix),
                                       std::end(inc_prefix)))
                {
                    auto inc_start = p + inc_prefix.size();
                    auto inc_end = std::find(inc_start, std::end(q), '&');
                    if (inc_start + 1 == inc_end &&
                        (*inc_start == '0' || *inc_start == '1'))
                    {
                        req.pri.inc = *inc_start - '0';
                    }
                    if (inc_end == std::end(q))
                    {
                        break;
                    }
                    p = inc_end + 1;
                    continue;
                }

                p = std::find(p, std::end(q), '&');
                if (p == std::end(q))
                {
                    break;
                }
                ++p;
            }
        }
        return req;
    }
} // namespace

enum FileEntryFlag
{
    FILE_ENTRY_TYPE_DIR = 0x1,
};

struct FileEntry
{
    uint64_t len;
    void *map;
    int fd;
    uint8_t flags;
};

namespace
{
    std::unordered_map<std::string, FileEntry> file_cache;
} // namespace

std::pair<FileEntry, int> Stream::open_file(const std::string &path)
{
    auto it = file_cache.find(path);
    if (it != std::end(file_cache))
    {
        return {(*it).second, 0};
    }

    auto fd = open(path.c_str(), O_RDONLY);
    if (fd == -1)
    {
        return {{}, -1};
    }

    struct stat st{};
    if (fstat(fd, &st) != 0)
    {
        close(fd);
        return {{}, -1};
    }

    FileEntry fe{};
    if (st.st_mode & S_IFDIR)
    {
        fe.flags |= FILE_ENTRY_TYPE_DIR;
        fe.fd = -1;
        close(fd);
    }
    else
    {
        fe.fd = fd;
        fe.len = st.st_size;
        if (fe.len)
        {
            fe.map = mmap(nullptr, fe.len, PROT_READ, MAP_SHARED, fd, 0);
            if (fe.map == MAP_FAILED)
            {
                std::cerr << "Server mmap: " << strerror(errno) << std::endl;
                close(fd);
                return {{}, -1};
            }
        }
    }

    file_cache.emplace(path, fe);

    return {std::move(fe), 0};
}

void Stream::map_file(const FileEntry &fe)
{
    data = static_cast<uint8_t *>(fe.map);
    datalen = fe.len;
}

int64_t Stream::find_dyn_length(const Request &req)
{
    // TODONE: Initialize the stream callbacks using the listener prepareHandler function
    QuicListener &listener = handler->server()->listener();
    auto response = listener.prepareInfo(req);
    if (!response.can_handle)
    {
        // If nothing can handle it, then pass it along to the file handler
        return -1;
    }
    if (response.is_file)
    {
        return -1;
    }
    if (response.is_live_stream)
    {
        live_stream = true;
        return -1;
    }
    return response.dyn_length;
}

void Stream::append_data(std::span<const uint8_t> data) {
    if (data.empty()) {
        return;
    }
    std::span<const uint8_t> remaining_span = data;
    // Note: if empty partial_chunk, stored_size() and get_wire_size() are both 0, 
    //       while get_signal_size() and get_wire_size() are also both 0, so it will test as not in progress.
    // Check if a prior chunk is already in progress, by examining the in memory size so far with the size read from the wire
    // The partial header condition is detected by checking if the wire size is less than the signal size, which is by definition in progress.
    // (But the stored_size and wire_size will be the same for incomplete headers)
    // For complete headers, wire size >= signal size, and stored_size == wire_size.
    if (partial_chunk.stored_size() != partial_chunk.get_wire_size() || partial_chunk.get_wire_size() < partial_chunk.get_signal_size())
    {
        // If so, append the as much data as necessary to the partial chunk
        size_t copy_bytes = min(partial_chunk.get_wire_size() - partial_chunk.stored_size(), data.size());
        if (partial_chunk.get_wire_size() < partial_chunk.get_signal_size()) {
            copy_bytes = partial_chunk.get_signal_size() - partial_chunk.stored_size();
        }
        auto next_start = partial_chunk.expand_use(copy_bytes);
        auto data_span = data.subspan(0, copy_bytes);
        partial_chunk.copy_span(data_span, {true, next_start});
        remaining_span = data.subspan(copy_bytes);
    } else {
        // Otherwise, create a new chunk and copy the data into it
        uint8_t signal_type = data[0];
        if (signal_type == signal_chunk_header::GLOBAL_SIGNAL_TYPE)
        {
            auto signal_span = data.subspan(0, min(sizeof(signal_chunk_header), data.size())); 
            partial_chunk = shared_span<>(signal_span);
            remaining_span = data.subspan(signal_span.size());
        } else if (signal_type == payload_chunk_header::GLOBAL_SIGNAL_TYPE)
        {
            if (data.size() < sizeof(payload_chunk_header)) {
                partial_chunk = shared_span<>(data);
                remaining_span = data.subspan(data.size());
            }
            else {
                auto header = reinterpret_cast<payload_chunk_header const*>(data.subspan(0, sizeof(payload_chunk_header)).data());
                auto data_span = data.subspan(0, min(header->get_wire_size(), data.size()));
                partial_chunk = shared_span<>(data_span);
                remaining_span = data.subspan(data_span.size());
            }
        } else {
            throw std::invalid_argument("Unhandled signal type " + std::to_string(signal_type) + " in Stream::append_data");
        }
    }
    // Check if the chunk is complete
    if (partial_chunk.stored_size() == partial_chunk.get_wire_size() && partial_chunk.get_wire_size() >= partial_chunk.get_signal_size()) {
        // If so, push it onto the incoming queue
        handler->push_incoming_chunk(req, move(partial_chunk));
        partial_chunk = shared_span<>(global_no_chunk_header, false);
    }
    append_data(remaining_span);
}


namespace
{
    nghttp3_ssize read_data(nghttp3_conn *conn, int64_t stream_id, nghttp3_vec *vec,
                            size_t veccnt, uint32_t *pflags, void *,
                            void *stream_user_data)
    {
        // First zero out the vec, for various cases where not all the data space is
        std::fill(vec, vec + veccnt, nghttp3_vec{nullptr, 0});
        // TODONE: Use the outgoingChunks queue instead of dyn_buf
        auto stream = static_cast<Stream *>(stream_user_data);
        bool is_closing = false;

        ngtcp2_conn_info ci;
        ngtcp2_conn_get_conn_info(stream->handler->conn(), &ci);
        auto pending = stream->handler->get_pending_chunks_size(stream_id, veccnt);
        if (pending.first > ci.cwnd)
        {
            return NGHTTP3_ERR_WOULDBLOCK;
        }
        if (pending.first == 0) {
            is_closing = true;
        } else {
            is_closing = stream->handler->lock_outgoing_chunks(pending.second, vec, veccnt);
        }

        if (is_closing) {
            *pflags |= NGHTTP3_DATA_FLAG_EOF;
        } else {
            *pflags |= NGHTTP3_DATA_FLAG_NO_END_STREAM;
        }
        if (is_closing && config.send_trailers)
        {
            auto stream_id_str = util::format_uint(stream_id);
            auto trailers = std::to_array({
                util::make_nv_nc("x-ngtcp2-stream-id"sv, stream_id_str),
            });

            if (auto rv = nghttp3_conn_submit_trailers(
                    conn, stream_id, trailers.data(), trailers.size());
                rv != 0)
            {
                std::cerr << "Server nghttp3_conn_submit_trailers: " << nghttp3_strerror(rv)
                          << std::endl;
                return NGHTTP3_ERR_CALLBACK_FAILURE;
            }
        } 

        return std::count_if(vec, vec + veccnt, [](const nghttp3_vec &v) {
            return v.base != nullptr; // Check if the base pointer is not nullptr
        });
    }
} // namespace

bool Handler::lock_outgoing_chunks(vector<StreamIdentifier> const &sids, nghttp3_vec *vec, size_t veccnt)
{
    // Unlike the unlock_chunks function, this function has to actually do the buisness logic
    // bit where it pops the chunks from the outgoingChunks queue
    // But first zero out the vec, in case the outgoingChunks queue cannot fill it up
    std::fill(vec, vec + veccnt, nghttp3_vec{nullptr, 0});

    // Ask the quic_listener_ for veccnt chunks
    chunks to_send = server()->listener().popNOutgoingChunks(sids, veccnt);
    bool signal_closed = server()->listener().noMoreChunks(sids);
    size_t vec_index = 0;

    for (auto chunk: to_send)
    {
        // We assume that the chunk is a shared_span<> and we can just use the base pointer
        // to lock it in the locked_chunks_ map
        auto locked_ptr = reinterpret_cast<uint8_t*>(chunk.use_chunk());
        vec[vec_index].base = locked_ptr;
        vec[vec_index].len = chunk.get_signal_size() + chunk.size();
        vec_index++;

        // Call shared_span_incr_rc to increment the reference count
        shared_span_incr_rc(locked_ptr, std::move(chunk));
    }
    return signal_closed;
}

pair<size_t, vector<StreamIdentifier>> Handler::get_pending_chunks_size(int64_t, size_t veccnt)
{
    // This logic relies on the server receiving one or more chunks from the client.
    // However, it does not really matter how many or what request_ids the client sent for this purpose:  
    //     All scid matching chunks are candidates for sending.
    // In other words, setting up the correct RequestHandlers is the job for the receiving side of the process.
    return server()->listener().planForNOutgoingChunks(get_scid(), veccnt);
}

void Handler::unlock_chunks(nghttp3_vec *vec, size_t veccnt)
{
    // Simply loop through the vec (up to veccnt limit) and call shared_span_decr_rc
    // on each vec[i].base
    for (size_t i = 0; i < veccnt; ++i)
    {
        auto it = locked_chunks_.find(vec[i].base);
        if (it != locked_chunks_.end())
        {
            shared_span_decr_rc(vec[i].base);
        }
    }
}
void Handler::shared_span_incr_rc(uint8_t *locked_ptr, shared_span<> &&to_lock)
{
    // use move semantics to put the to_lock into the locked_chunks_ map at the key == locked_ptr
    // we assume that the caller of shared_span_incr_rc will never use the same locked_ptr twice, which should be a solid assumption 
    // with respect to reading and writing with this class
    locked_chunks_.emplace(locked_ptr, std::move(to_lock));
}
void Handler::shared_span_decr_rc(uint8_t *locked_ptr)
{
    locked_chunks_.erase(locked_ptr);
}

void Handler::push_incoming_chunk(const Request& req, shared_span<> &&chunk)
{
    // Push the chunk into the quic_connector_ requestResolutionQueue
    server()->listener().pushIncomingChunk(req, get_scid(), move(chunk));
}


auto dyn_buf = std::make_unique<std::array<uint8_t, 16_k>>();

namespace
{
    nghttp3_ssize dyn_read_data(nghttp3_conn *conn, int64_t stream_id,
                                nghttp3_vec *vec, size_t veccnt, uint32_t *pflags,
                                void *, void *stream_user_data)
    {
        // First zero out the vec, for various cases where not all the data space is
        std::fill(vec, vec + veccnt, nghttp3_vec{nullptr, 0});
        // Notice that live_stream essentially means the stream length is not defined (see find_dyn_length).
        // TODONE: Use the outgoingChunks queue
        auto stream = static_cast<Stream *>(stream_user_data);
        bool is_closing = false;

        ngtcp2_conn_info ci;
        ngtcp2_conn_get_conn_info(stream->handler->conn(), &ci);
        // auto stream_window = ngtcp2_conn_get_max_stream_data_left(stream->handler->conn(), stream_id);
        // auto connection_window = ngtcp2_conn_get_max_data_left(stream->handler->conn());
        
        // std::cout << "Stream flow control window: " << stream_window << std::endl;
        // std::cout << "Connection flow control window: " << connection_window << std::endl;

        auto pending = stream->handler->get_pending_chunks_size(stream_id, veccnt);
        if (pending.first > ci.cwnd)
        {
            return NGHTTP3_ERR_WOULDBLOCK;
        }
        if (pending.first == 0)
        {
            is_closing = true;
        } else {
            is_closing = stream->handler->lock_outgoing_chunks(pending.second, vec, veccnt);
        }

        // This looks wrong if dyndataleft == -1, but note that it will get cast to infinity, and turn out correct.
        auto len = std::min(pending.first, static_cast<size_t>(stream->dyndataleft));

        stream->dynbuflen += len;
        if (!stream->live_stream)
        {
            stream->dyndataleft -= len;
        }
        if (stream->dyndataleft == 0)
        {
            is_closing = true;
        }

        if (is_closing) {
            *pflags |= NGHTTP3_DATA_FLAG_EOF;
        } else {
            *pflags |= NGHTTP3_DATA_FLAG_NO_END_STREAM;
        }
        if (is_closing && config.send_trailers)
        {
            auto stream_id_str = util::format_uint(stream_id);
            auto trailers = std::to_array({
                util::make_nv_nc("x-ngtcp2-stream-id"sv, stream_id_str),
            });

            if (auto rv = nghttp3_conn_submit_trailers(
                    conn, stream_id, trailers.data(), trailers.size());
                rv != 0)
            {
                std::cerr << "Server nghttp3_conn_submit_trailers: " << nghttp3_strerror(rv)
                            << std::endl;
                return NGHTTP3_ERR_CALLBACK_FAILURE;
            }
        }

        return std::count_if(vec, vec + veccnt, [](const nghttp3_vec &v) {
            return v.base != nullptr; // Check if the base pointer is not nullptr
        });
    }
} // namespace

void Stream::http_acked_stream_data(uint64_t datalen)
{
    if (!dynresp)
    {
        return;
    }

    assert(dynbuflen >= datalen);

    dynbuflen -= datalen;
}

int Stream::send_status_response(nghttp3_conn *httpconn,
                                 unsigned int status_code,
                                 const std::vector<HTTPHeader> &extra_headers)
{
    status_resp_body = make_status_body(status_code);

    auto status_code_str = util::format_uint(status_code);
    auto content_length_str = util::format_uint(status_resp_body.size());

    std::vector<nghttp3_nv> nva(4 + extra_headers.size());
    nva[0] = util::make_nv_nc(":status"sv, status_code_str);
    nva[1] = util::make_nv_nn("server"sv, NGTCP2_SERVER);
    nva[2] = util::make_nv_nn("content-type"sv, "text/html; charset=utf-8");
    nva[3] = util::make_nv_nc("content-length"sv, content_length_str);
    for (size_t i = 0; i < extra_headers.size(); ++i)
    {
        auto &hdr = extra_headers[i];
        auto &nv = nva[4 + i];
        nv = util::make_nv_cc(hdr.name, hdr.value);
    }

    data = (uint8_t *)status_resp_body.data();
    datalen = status_resp_body.size();

    nghttp3_data_reader dr{
        .read_data = read_data,
    };

    if (auto rv = nghttp3_conn_submit_response(httpconn, stream_id, nva.data(),
                                               nva.size(), &dr);
        rv != 0)
    {
        std::cerr << "Server nghttp3_conn_submit_response: " << nghttp3_strerror(rv)
                  << std::endl;
        return -1;
    }

    if (config.send_trailers)
    {
        auto stream_id_str = util::format_uint(stream_id);
        auto trailers = std::to_array({
            util::make_nv_nc("x-ngtcp2-stream-id"sv, stream_id_str),
        });

        if (auto rv = nghttp3_conn_submit_trailers(
                httpconn, stream_id, trailers.data(), trailers.size());
            rv != 0)
        {
            std::cerr << "Server nghttp3_conn_submit_trailers: " << nghttp3_strerror(rv)
                      << std::endl;
            return -1;
        }
    }

    handler->shutdown_read(stream_id, NGHTTP3_H3_NO_ERROR);

    return 0;
}

int Stream::send_redirect_response(nghttp3_conn *httpconn,
                                   unsigned int status_code,
                                   const std::string_view &path)
{
    return send_status_response(httpconn, status_code, {{"location", path}});
}

int Stream::start_response(nghttp3_conn *httpconn)
{
    // LEGACY ISSUE This should be handled by nghttp3
    if (uri.empty() || method.empty())
    {
        return send_status_response(httpconn, 400);
    }

    auto req = request_path(uri, method == "CONNECT");
    if (req.path.empty())
    {
        return send_status_response(httpconn, 400);
    }

    auto dyn_len = find_dyn_length(req);

    int64_t content_length = -1;
    nghttp3_data_reader dr{};
    auto content_type = "text/plain"sv;

    if ((!live_stream) && (dyn_len == -1))
    {
        auto path = config.htdocs + req.path;
        auto [fe, rv] = open_file(path);
        if (rv != 0)
        {
            send_status_response(httpconn, 404);
            return 0;
        }

        if (fe.flags & FILE_ENTRY_TYPE_DIR)
        {
            send_redirect_response(httpconn, 308,
                                   path.substr(config.htdocs.size() - 1) + '/');
            return 0;
        }

        content_length = fe.len;

        if (method != "HEAD")
        {
            map_file(fe);
        }

        dr.read_data = read_data;

        auto ext = std::end(req.path) - 1;
        for (; ext != std::begin(req.path) && *ext != '.' && *ext != '/'; --ext)
            ;
        if (*ext == '.')
        {
            ++ext;
            auto it = config.mime_types.find(std::string{ext, std::end(req.path)});
            if (it != std::end(config.mime_types))
            {
                content_type = (*it).second;
            }
        }
    }
    else
    {
        content_length = dyn_len;
        dynresp = true;
        dr.read_data = dyn_read_data;

        if (method != "HEAD")
        {
            datalen = dyn_len;
            dyndataleft = dyn_len;
        }

        content_type = "application/octet-stream"sv;
    }

    auto content_length_str = util::format_uint(content_length);

    std::array<nghttp3_nv, 5> nva{
        util::make_nv_nn(":status"sv, "200"sv),
        util::make_nv_nn("server"sv, NGTCP2_SERVER),
        util::make_nv_nn("content-type"sv, content_type),
        util::make_nv_nc("content-length"sv, content_length_str),
    };

    size_t nvlen = 4;
    if (live_stream) {
        // Don't send content-length for live streams
        nvlen = 3;
    }

    std::string prival;

    if (req.pri.urgency != -1 || req.pri.inc != -1)
    {
        nghttp3_pri pri;

        if (auto rv = nghttp3_conn_get_stream_priority(httpconn, &pri, stream_id);
            rv != 0)
        {
            std::cerr << "Server nghttp3_conn_get_stream_priority: " << nghttp3_strerror(rv)
                      << std::endl;
            return -1;
        }

        if (req.pri.urgency != -1)
        {
            pri.urgency = req.pri.urgency;
        }
        if (req.pri.inc != -1)
        {
            pri.inc = req.pri.inc;
        }

        if (auto rv =
                nghttp3_conn_set_server_stream_priority(httpconn, stream_id, &pri);
            rv != 0)
        {
            std::cerr << "Server nghttp3_conn_set_stream_priority: " << nghttp3_strerror(rv)
                      << std::endl;
            return -1;
        }

        prival = "u=";
        prival += pri.urgency + '0';
        prival += ",i";
        if (!pri.inc)
        {
            prival += "=?0";
        }

        nva[nvlen++] = util::make_nv_nc("priority"sv, prival);
    }

    if (!config.quiet)
    {
        debug::print_http_response_headers(stream_id, nva.data(), nvlen);
    }

    if (auto rv = nghttp3_conn_submit_response(httpconn, stream_id, nva.data(),
                                               nvlen, &dr);
        rv != 0)
    {
        std::cerr << "Server nghttp3_conn_submit_response: " << nghttp3_strerror(rv)
                  << std::endl;
        return -1;
    }

    if (config.send_trailers && dyn_len == -1)
    {
        auto stream_id_str = util::format_uint(stream_id);
        auto trailers = std::to_array({
            util::make_nv_nc("x-ngtcp2-stream-id"sv, stream_id_str),
        });

        if (auto rv = nghttp3_conn_submit_trailers(
                httpconn, stream_id, trailers.data(), trailers.size());
            rv != 0)
        {
            std::cerr << "Server nghttp3_conn_submit_trailers: " << nghttp3_strerror(rv)
                      << std::endl;
            return -1;
        }
    }

    return 0;
}

void Handler::writecb_start()
{
    ev_io_start(loop_, &wev_);
}

namespace
{
    void writecb(struct ev_loop * /* loop */, ev_io *w, int /* revents */)
    {
        auto h = static_cast<Handler *>(w->data);
        auto s = h->server();

        switch (h->on_write())
        {
        case 0:
        case NETWORK_ERR_CLOSE_WAIT:
            return;
        default:
            s->remove(h);
        }
    }
} // namespace

namespace
{
    void close_waitcb(struct ev_loop * /* loop */, ev_timer *w, int /* revents */)
    {
        auto h = static_cast<Handler *>(w->data);
        auto s = h->server();
        auto conn = h->conn();

        if (ngtcp2_conn_in_closing_period(conn))
        {
            if (!config.quiet)
            {
                std::cerr << "Server Closing Period is over" << std::endl;
            }

            s->remove(h);
            return;
        }
        if (ngtcp2_conn_in_draining_period(conn))
        {
            if (!config.quiet)
            {
                std::cerr << "Server Draining Period is over" << std::endl;
            }

            s->remove(h);
            return;
        }

        assert(0);
    }
} // namespace

namespace
{
    void timeoutcb(struct ev_loop *loop, ev_timer *w, int /* revents */)
    {
        int rv;

        auto h = static_cast<Handler *>(w->data);
        auto s = h->server();

        if (!config.quiet)
        {
            std::cerr << "Server Timer expired" << std::endl;
        }

        rv = h->handle_expiry();
        if (rv != 0)
        {
            goto fail;
        }

        rv = h->on_write();
        if (rv != 0)
        {
            goto fail;
        }

        return;

    fail:
        switch (rv)
        {
        case NETWORK_ERR_CLOSE_WAIT:
            ev_timer_stop(loop, w);
            return;
        default:
            s->remove(h);
            return;
        }
    }
} // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
Handler::Handler(struct ev_loop *loop, Server *server)
    : loop_(loop),
      server_(server),
      qlog_(nullptr),
      scid_{},
      httpconn_{nullptr},
      nkey_update_(0),
      no_gso_{
#ifdef UDP_SEGMENT
          false
#else  // !defined(UDP_SEGMENT)
          true
#endif // !defined(UDP_SEGMENT)
      },
      tx_{
          .data = std::unique_ptr<uint8_t[]>(new uint8_t[64_k]),
      }
{
    ev_io_init(&wev_, writecb, 0, EV_WRITE);
    wev_.data = this;
    ev_timer_init(&timer_, timeoutcb, 0., 0.);
    timer_.data = this;
}
#pragma GCC diagnostic pop

Handler::~Handler()
{
    if (!config.quiet)
    {
        std::cerr << "Server " << scid_ << " Closing QUIC connection " << std::endl;
    }

    ev_timer_stop(loop_, &timer_);
    ev_io_stop(loop_, &wev_);

    if (httpconn_)
    {
        nghttp3_conn_del(httpconn_);
    }

    if (qlog_)
    {
        fclose(qlog_);
    }
}

namespace
{
    int handshake_completed(ngtcp2_conn *conn, void *user_data)
    {
        auto h = static_cast<Handler *>(user_data);

        if (!config.quiet)
        {
            debug::handshake_completed(conn, user_data);
        }

        if (h->handshake_completed() != 0)
        {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }

        return 0;
    }
} // namespace

int Handler::handshake_completed()
{
    if (!config.quiet)
    {
        std::cerr << "Server Negotiated cipher suite is " << tls_session_.get_cipher_name()
                  << std::endl;
        if (auto group = tls_session_.get_negotiated_group(); !group.empty())
        {
            std::cerr << "Server Negotiated group is " << group << std::endl;
        }
        std::cerr << "Server Negotiated ALPN is " << tls_session_.get_selected_alpn()
                  << std::endl;
    }

    if (tls_session_.send_session_ticket() != 0)
    {
        std::cerr << "Server Unable to send session ticket" << std::endl;
    }

    std::array<uint8_t, NGTCP2_CRYPTO_MAX_REGULAR_TOKENLEN> token;

    auto path = ngtcp2_conn_get_path(conn_);
    auto t = std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();

    auto tokenlen = ngtcp2_crypto_generate_regular_token(
        token.data(), config.static_secret.data(), config.static_secret.size(),
        path->remote.addr, path->remote.addrlen, t);
    if (tokenlen < 0)
    {
        if (!config.quiet)
        {
            std::cerr << "Server Unable to generate token" << std::endl;
        }
        return 0;
    }

    if (auto rv = ngtcp2_conn_submit_new_token(conn_, token.data(), tokenlen);
        rv != 0)
    {
        if (!config.quiet)
        {
            std::cerr << "Server ngtcp2_conn_submit_new_token: " << ngtcp2_strerror(rv)
                      << std::endl;
        }
        return -1;
    }

    return 0;
}

namespace
{
    int do_hp_mask(uint8_t *dest, const ngtcp2_crypto_cipher *hp,
                   const ngtcp2_crypto_cipher_ctx *hp_ctx, const uint8_t *sample)
    {
        if (ngtcp2_crypto_hp_mask(dest, hp, hp_ctx, sample) != 0)
        {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }

        if (!config.quiet && config.show_secret)
        {
            debug::print_hp_mask({dest, NGTCP2_HP_MASKLEN},
                                 {sample, NGTCP2_HP_SAMPLELEN});
        }

        return 0;
    }
} // namespace

namespace
{
    int recv_crypto_data(ngtcp2_conn *conn,
                         ngtcp2_encryption_level encryption_level, uint64_t offset,
                         const uint8_t *data, size_t datalen, void *user_data)
    {
        if (!config.quiet && !config.no_quic_dump)
        {
            debug::print_crypto_data(encryption_level, {data, datalen});
        }

        return ngtcp2_crypto_recv_crypto_data_cb(conn, encryption_level, offset, data,
                                                 datalen, user_data);
    }
} // namespace

namespace
{
    int recv_stream_data(ngtcp2_conn *, uint32_t flags, int64_t stream_id,
                         uint64_t /* offset */, const uint8_t *data, size_t datalen,
                         void *user_data, void * /* stream_user_data */)
    {
        auto h = static_cast<Handler *>(user_data);

        if (h->recv_stream_data(flags, stream_id, {data, datalen}) != 0)
        {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }

        return 0;
    }
} // namespace

namespace
{
    int acked_stream_data_offset(ngtcp2_conn *, int64_t stream_id,
                                 uint64_t /* offset */, uint64_t datalen, void *user_data,
                                 void * /* stream_user_data */)
    {
        auto h = static_cast<Handler *>(user_data);
        if (h->acked_stream_data_offset(stream_id, datalen) != 0)
        {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        return 0;
    }
} // namespace

int Handler::acked_stream_data_offset(int64_t stream_id, uint64_t datalen)
{
    if (!httpconn_)
    {
        return 0;
    }

    if (auto rv = nghttp3_conn_add_ack_offset(httpconn_, stream_id, datalen);
        rv != 0)
    {
        std::cerr << "Server nghttp3_conn_add_ack_offset: " << nghttp3_strerror(rv)
                  << std::endl;
        return -1;
    }

    return 0;
}

namespace
{
    int stream_open(ngtcp2_conn *, int64_t stream_id, void *user_data)
    {
        auto h = static_cast<Handler *>(user_data);
        h->on_stream_open(stream_id);
        return 0;
    }
} // namespace

void Handler::on_stream_open(int64_t stream_id)
{
    if (!ngtcp2_is_bidi_stream(stream_id))
    {
        return;
    }
    auto it = streams_.find(stream_id);
    (void)it;
    assert(it == std::end(streams_));
    streams_.emplace(stream_id, std::make_unique<Stream>(stream_id, this));
}

namespace
{
    int stream_close(ngtcp2_conn *, uint32_t flags, int64_t stream_id,
                     uint64_t app_error_code, void *user_data,
                     void */* stream_user_data */)
    {
        auto h = static_cast<Handler *>(user_data);

        if (!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET))
        {
            app_error_code = NGHTTP3_H3_NO_ERROR;
        }

        if (h->on_stream_close(stream_id, app_error_code) != 0)
        {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        return 0;
    }
} // namespace

namespace
{
    int stream_reset(ngtcp2_conn *, int64_t stream_id, uint64_t /* final_size */,
                     uint64_t /* app_error_code */, void * user_data,
                     void */* stream_user_data */)
    {
        auto h = static_cast<Handler *>(user_data);
        if (h->on_stream_reset(stream_id) != 0)
        {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        return 0;
    }
} // namespace

int Handler::on_stream_reset(int64_t stream_id)
{
    if (httpconn_)
    {
        if (auto rv = nghttp3_conn_shutdown_stream_read(httpconn_, stream_id);
            rv != 0)
        {
            std::cerr << "Server nghttp3_conn_shutdown_stream_read: " << nghttp3_strerror(rv)
                      << std::endl;
            return -1;
        }
    }
    return 0;
}

namespace
{
    int stream_stop_sending(ngtcp2_conn *, int64_t stream_id,
                            uint64_t /* app_error_code */, void *user_data,
                            void */* stream_user_data */)
    {
        auto h = static_cast<Handler *>(user_data);
        if (h->on_stream_stop_sending(stream_id) != 0)
        {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        return 0;
    }
} // namespace

int Handler::on_stream_stop_sending(int64_t stream_id)
{
    if (!httpconn_)
    {
        return 0;
    }

    if (auto rv = nghttp3_conn_shutdown_stream_read(httpconn_, stream_id);
        rv != 0)
    {
        std::cerr << "Server nghttp3_conn_shutdown_stream_read: " << nghttp3_strerror(rv)
                  << std::endl;
        return -1;
    }

    return 0;
}

namespace
{
    void rand(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx */* rand_ctx */)
    {
        auto dis = std::uniform_int_distribution<uint8_t>();
        std::generate(dest, dest + destlen, [&dis]()
                      { return dis(randgen); });
    }
} // namespace

namespace
{
    int get_new_connection_id(ngtcp2_conn *, ngtcp2_cid *cid, uint8_t *token,
                              size_t cidlen, void *user_data)
    {
        if (util::generate_secure_random({cid->data, cidlen}) != 0)
        {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }

        cid->datalen = cidlen;
        if (ngtcp2_crypto_generate_stateless_reset_token(
                token, config.static_secret.data(), config.static_secret.size(), cid) !=
            0)
        {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }

        auto h = static_cast<Handler *>(user_data);
        h->server()->associate_cid(cid, h);

        return 0;
    }
} // namespace

namespace
{
    int remove_connection_id(ngtcp2_conn *, const ngtcp2_cid *cid,
                             void *user_data)
    {
        auto h = static_cast<Handler *>(user_data);
        h->server()->dissociate_cid(cid);
        return 0;
    }
} // namespace

namespace
{
    int update_key(ngtcp2_conn *, uint8_t *rx_secret, uint8_t *tx_secret,
                   ngtcp2_crypto_aead_ctx *rx_aead_ctx, uint8_t *rx_iv,
                   ngtcp2_crypto_aead_ctx *tx_aead_ctx, uint8_t *tx_iv,
                   const uint8_t *current_rx_secret,
                   const uint8_t *current_tx_secret, size_t secretlen,
                   void *user_data)
    {
        auto h = static_cast<Handler *>(user_data);
        if (h->update_key(rx_secret, tx_secret, rx_aead_ctx, rx_iv, tx_aead_ctx,
                          tx_iv, current_rx_secret, current_tx_secret,
                          secretlen) != 0)
        {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        return 0;
    }
} // namespace

namespace
{
    int path_validation(ngtcp2_conn *conn, uint32_t flags, const ngtcp2_path *path,
                        const ngtcp2_path */*old_path*/,
                        ngtcp2_path_validation_result res, void */*user_data*/)
    {
        if (!config.quiet)
        {
            debug::path_validation(path, res);
        }

        if (res != NGTCP2_PATH_VALIDATION_RESULT_SUCCESS ||
            !(flags & NGTCP2_PATH_VALIDATION_FLAG_NEW_TOKEN))
        {
            return 0;
        }

        std::array<uint8_t, NGTCP2_CRYPTO_MAX_REGULAR_TOKENLEN> token;
        auto t = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();

        auto tokenlen = ngtcp2_crypto_generate_regular_token(
            token.data(), config.static_secret.data(), config.static_secret.size(),
            path->remote.addr, path->remote.addrlen, t);
        if (tokenlen < 0)
        {
            if (!config.quiet)
            {
                std::cerr << "Server Unable to generate token" << std::endl;
            }

            return 0;
        }

        if (auto rv = ngtcp2_conn_submit_new_token(conn, token.data(), tokenlen);
            rv != 0)
        {
            if (!config.quiet)
            {
                std::cerr << "Server ngtcp2_conn_submit_new_token: " << ngtcp2_strerror(rv)
                          << std::endl;
            }

            return NGTCP2_ERR_CALLBACK_FAILURE;
        }

        return 0;
    }
} // namespace

namespace
{
    int extend_max_remote_streams_bidi(ngtcp2_conn *, uint64_t max_streams,
                                       void *user_data)
    {
        auto h = static_cast<Handler *>(user_data);
        h->extend_max_remote_streams_bidi(max_streams);
        return 0;
    }
} // namespace

void Handler::extend_max_remote_streams_bidi(uint64_t max_streams)
{
    if (!httpconn_)
    {
        return;
    }

    nghttp3_conn_set_max_client_streams_bidi(httpconn_, max_streams);
}

namespace
{
    int http_recv_data(nghttp3_conn *, int64_t stream_id, const uint8_t *data,
                       size_t datalen, void *user_data, void *stream_user_data)
    {
        // TODONE: Add this to the incoming chunks queue, instead of just printing it out
        auto stream = static_cast<Stream *>(stream_user_data);

        if (!config.quiet && !config.no_http_dump)
        {
            debug::print_http_data(stream_id, {data, datalen});
        }
        auto h = static_cast<Handler *>(user_data);
        h->http_consume(stream_id, datalen);
        // Then we have a full chunk, so we need to push it into the quic_connector_
        auto data_span = std::span<uint8_t>(const_cast<uint8_t*>(data), datalen);
        stream->append_data(data_span);
    
        return 0;
    }
} // namespace

namespace
{
    int http_deferred_consume(nghttp3_conn *, int64_t stream_id,
                              size_t nconsumed, void *user_data,
                              void */* stream_user_data */)
    {
        auto h = static_cast<Handler *>(user_data);
        h->http_consume(stream_id, nconsumed);
        return 0;
    }
} // namespace

void Handler::http_consume(int64_t stream_id, size_t nconsumed)
{
    ngtcp2_conn_extend_max_stream_offset(conn_, stream_id, nconsumed);
    ngtcp2_conn_extend_max_offset(conn_, nconsumed);
}

namespace
{
    int http_begin_request_headers(nghttp3_conn *, int64_t stream_id,
                                   void *user_data, void */* stream_user_data */)
    {
        if (!config.quiet)
        {
            debug::print_http_begin_request_headers(stream_id);
        }

        auto h = static_cast<Handler *>(user_data);
        h->http_begin_request_headers(stream_id);
        return 0;
    }
} // namespace

void Handler::http_begin_request_headers(int64_t stream_id)
{
    auto it = streams_.find(stream_id);
    assert(it != std::end(streams_));
    auto &stream = (*it).second;

    nghttp3_conn_set_stream_user_data(httpconn_, stream_id, stream.get());
}

namespace
{
    int http_recv_request_header(nghttp3_conn *, int64_t stream_id,
                                 int32_t token, nghttp3_rcbuf *name,
                                 nghttp3_rcbuf *value, uint8_t flags,
                                 void *user_data, void *stream_user_data)
    {
        if (!config.quiet)
        {
            debug::print_http_header(stream_id, name, value, flags);
        }

        auto h = static_cast<Handler *>(user_data);
        auto stream = static_cast<Stream *>(stream_user_data);
        h->http_recv_request_header(stream, token, name, value);
        return 0;
    }
} // namespace

void Handler::http_recv_request_header(Stream *stream, int32_t token,
                                       nghttp3_rcbuf */* name */,
                                       nghttp3_rcbuf *value)
{
    auto v = nghttp3_rcbuf_get_buf(value);

    switch (token)
    {
    case NGHTTP3_QPACK_TOKEN__PATH:
        stream->uri = std::string{v.base, v.base + v.len};
        stream->req = request_path(stream->uri, false);
        break;
    case NGHTTP3_QPACK_TOKEN__METHOD:
        stream->method = std::string{v.base, v.base + v.len};
        break;
    case NGHTTP3_QPACK_TOKEN__AUTHORITY:
        stream->authority = std::string{v.base, v.base + v.len};
        break;
    }
}

namespace
{
    int http_end_request_headers(nghttp3_conn *, int64_t stream_id, int /* fin */,
                                 void *user_data, void *stream_user_data)
    {
        if (!config.quiet)
        {
            debug::print_http_end_headers(stream_id);
        }

        auto h = static_cast<Handler *>(user_data);
        auto stream = static_cast<Stream *>(stream_user_data);
        if (h->http_end_request_headers(stream) != 0)
        {
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        return 0;
    }
} // namespace

int Handler::http_end_request_headers(Stream *stream)
{
    if (config.early_response)
    {
        if (start_response(stream) != 0)
        {
            return -1;
        }

        shutdown_read(stream->stream_id, NGHTTP3_H3_NO_ERROR);
    }
    return 0;
}

namespace
{
    int http_end_stream(nghttp3_conn *, int64_t /* stream_id */, void *user_data,
                        void *stream_user_data)
    {
        auto h = static_cast<Handler *>(user_data);
        auto stream = static_cast<Stream *>(stream_user_data);
        if (h->http_end_stream(stream) != 0)
        {
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        return 0;
    }
} // namespace

int Handler::http_end_stream(Stream *stream)
{
    if (!config.early_response)
    {
        return start_response(stream);
    }
    return 0;
}

int Handler::start_response(Stream *stream)
{
    return stream->start_response(httpconn_);
}

namespace
{
    int http_acked_stream_data(nghttp3_conn *, int64_t /* stream_id */,
                               uint64_t datalen, void *user_data,
                               void *stream_user_data)
    {
        auto h = static_cast<Handler *>(user_data);
        auto stream = static_cast<Stream *>(stream_user_data);
        h->http_acked_stream_data(stream, datalen);
        return 0;
    }
} // namespace

void Handler::http_acked_stream_data(Stream *stream, uint64_t datalen)
{
    stream->http_acked_stream_data(datalen);

    ngtcp2_conn_info ci;

    ngtcp2_conn_get_conn_info(stream->handler->conn(), &ci);

    if (stream->dynresp && stream->dynbuflen < ci.cwnd)
    {
        if (auto rv = nghttp3_conn_resume_stream(httpconn_, stream->stream_id);
            rv != 0)
        {
            // LEGACY ISSUE Handle error
            std::cerr << "Server nghttp3_conn_resume_stream: " << nghttp3_strerror(rv)
                      << std::endl;
        }
    }
}

namespace
{
    int http_stream_close(nghttp3_conn *, int64_t stream_id,
                          uint64_t app_error_code, void *conn_user_data,
                          void */* stream_user_data */)
    {
        auto h = static_cast<Handler *>(conn_user_data);
        h->http_stream_close(stream_id, app_error_code);
        return 0;
    }
} // namespace

void Handler::http_stream_close(int64_t stream_id, uint64_t app_error_code)
{
    auto it = streams_.find(stream_id);
    if (it == std::end(streams_))
    {
        return;
    }

    if (!config.quiet)
    {
        std::cerr << "Server HTTP stream " << stream_id << " closed with error code "
                  << app_error_code << std::endl;
    }

    streams_.erase(it);

    if (ngtcp2_is_bidi_stream(stream_id))
    {
        assert(!ngtcp2_conn_is_local_stream(conn_, stream_id));
        ngtcp2_conn_extend_max_streams_bidi(conn_, 1);
    }
}

namespace
{
    int http_stop_sending(nghttp3_conn *, int64_t stream_id,
                          uint64_t app_error_code, void *user_data,
                          void */* stream_user_data */)
    {
        auto h = static_cast<Handler *>(user_data);
        if (h->http_stop_sending(stream_id, app_error_code) != 0)
        {
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        return 0;
    }
} // namespace

int Handler::http_stop_sending(int64_t stream_id, uint64_t app_error_code)
{
    if (auto rv =
            ngtcp2_conn_shutdown_stream_read(conn_, 0, stream_id, app_error_code);
        rv != 0)
    {
        std::cerr << "Server ngtcp2_conn_shutdown_stream_read: " << ngtcp2_strerror(rv)
                  << std::endl;
        return -1;
    }
    return 0;
}

namespace
{
    int http_reset_stream(nghttp3_conn *, int64_t stream_id,
                          uint64_t app_error_code, void *user_data,
                          void */* stream_user_data */)
    {
        auto h = static_cast<Handler *>(user_data);
        if (h->http_reset_stream(stream_id, app_error_code) != 0)
        {
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        return 0;
    }
} // namespace

int Handler::http_reset_stream(int64_t stream_id, uint64_t app_error_code)
{
    if (auto rv =
            ngtcp2_conn_shutdown_stream_write(conn_, 0, stream_id, app_error_code);
        rv != 0)
    {
        std::cerr << "Server ngtcp2_conn_shutdown_stream_write: " << ngtcp2_strerror(rv)
                  << std::endl;
        return -1;
    }
    return 0;
}

namespace
{
    int http_recv_settings(nghttp3_conn *, const nghttp3_settings *settings,
                           void */* conn_user_data */)
    {
        if (!config.quiet)
        {
            debug::print_http_settings(settings);
        }

        return 0;
    }
} // namespace

int Handler::setup_httpconn()
{
    if (httpconn_)
    {
        return 0;
    }

    if (ngtcp2_conn_get_streams_uni_left(conn_) < 3)
    {
        std::cerr << "Server peer does not allow at least 3 unidirectional streams."
                  << std::endl;
        return -1;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    nghttp3_callbacks callbacks{
        .acked_stream_data = ::http_acked_stream_data,
        .stream_close = ::http_stream_close,
        .recv_data = ::http_recv_data,
        .deferred_consume = ::http_deferred_consume,
        .begin_headers = ::http_begin_request_headers,
        .recv_header = ::http_recv_request_header,
        .end_headers = ::http_end_request_headers,
        .stop_sending = ::http_stop_sending,
        .end_stream = ::http_end_stream,
        .reset_stream = ::http_reset_stream,
        .recv_settings = ::http_recv_settings,
    };
#pragma GCC diagnostic pop
    nghttp3_settings settings;
    nghttp3_settings_default(&settings);
    settings.qpack_max_dtable_capacity = 4096;
    settings.qpack_blocked_streams = 100;

    auto mem = nghttp3_mem_default();

    if (auto rv =
            nghttp3_conn_server_new(&httpconn_, &callbacks, &settings, mem, this);
        rv != 0)
    {
        std::cerr << "Server nghttp3_conn_server_new: " << nghttp3_strerror(rv)
                  << std::endl;
        return -1;
    }

    auto params = ngtcp2_conn_get_local_transport_params(conn_);

    nghttp3_conn_set_max_client_streams_bidi(httpconn_,
                                             params->initial_max_streams_bidi);

    int64_t ctrl_stream_id;

    if (auto rv = ngtcp2_conn_open_uni_stream(conn_, &ctrl_stream_id, nullptr);
        rv != 0)
    {
        std::cerr << "Server ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv)
                  << std::endl;
        return -1;
    }

    if (auto rv = nghttp3_conn_bind_control_stream(httpconn_, ctrl_stream_id);
        rv != 0)
    {
        std::cerr << "Server nghttp3_conn_bind_control_stream: " << nghttp3_strerror(rv)
                  << std::endl;
        return -1;
    }

    if (!config.quiet)
    {
        fprintf(stderr, "Server http: control stream=%" PRIx64 "\n", ctrl_stream_id);
    }

    int64_t qpack_enc_stream_id, qpack_dec_stream_id;

    if (auto rv =
            ngtcp2_conn_open_uni_stream(conn_, &qpack_enc_stream_id, nullptr);
        rv != 0)
    {
        std::cerr << "Server ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv)
                  << std::endl;
        return -1;
    }

    if (auto rv =
            ngtcp2_conn_open_uni_stream(conn_, &qpack_dec_stream_id, nullptr);
        rv != 0)
    {
        std::cerr << "Server ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv)
                  << std::endl;
        return -1;
    }

    if (auto rv = nghttp3_conn_bind_qpack_streams(httpconn_, qpack_enc_stream_id,
                                                  qpack_dec_stream_id);
        rv != 0)
    {
        std::cerr << "Server nghttp3_conn_bind_qpack_streams: " << nghttp3_strerror(rv)
                  << std::endl;
        return -1;
    }

    if (!config.quiet)
    {
        fprintf(stderr,
                "Server http: QPACK streams encoder=%" PRIx64 " decoder=%" PRIx64 "\n",
                qpack_enc_stream_id, qpack_dec_stream_id);
    }

    return 0;
}

namespace
{
    int extend_max_stream_data(ngtcp2_conn *, int64_t stream_id,
                               uint64_t max_data, void *user_data,
                               void */* stream_user_data */)
    {
        auto h = static_cast<Handler *>(user_data);
        if (h->extend_max_stream_data(stream_id, max_data) != 0)
        {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        return 0;
    }
} // namespace

int Handler::extend_max_stream_data(int64_t stream_id, uint64_t /* max_data */)
{
    if (auto rv = nghttp3_conn_unblock_stream(httpconn_, stream_id); rv != 0)
    {
        std::cerr << "Server nghttp3_conn_unblock_stream: " << nghttp3_strerror(rv)
                  << std::endl;
        return -1;
    }
    return 0;
}

namespace
{
    int recv_tx_key(ngtcp2_conn *, ngtcp2_encryption_level level,
                    void *user_data)
    {
        if (level != NGTCP2_ENCRYPTION_LEVEL_1RTT)
        {
            return 0;
        }

        auto h = static_cast<Handler *>(user_data);
        if (h->setup_httpconn() != 0)
        {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }

        return 0;
    }
} // namespace

namespace
{
    void write_qlog(void *user_data, uint32_t /* flags */, const void *data,
                    size_t datalen)
    {
        auto h = static_cast<Handler *>(user_data);
        h->write_qlog(data, datalen);
    }
} // namespace

void Handler::write_qlog(const void *data, size_t datalen)
{
    assert(qlog_);
    fwrite(data, 1, datalen, qlog_);
}

int Handler::init(const Endpoint &ep, const Address &local_addr,
                  const sockaddr *sa, socklen_t salen, const ngtcp2_cid *dcid,
                  const ngtcp2_cid *scid, const ngtcp2_cid *ocid,
                  std::span<const uint8_t> token, ngtcp2_token_type token_type,
                  uint32_t version, TLSServerContext &tls_ctx)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    auto callbacks = ngtcp2_callbacks{
        .recv_client_initial = ngtcp2_crypto_recv_client_initial_cb,
        .recv_crypto_data = ::recv_crypto_data,
        .handshake_completed = ::handshake_completed,
        .encrypt = ngtcp2_crypto_encrypt_cb,
        .decrypt = ngtcp2_crypto_decrypt_cb,
        .hp_mask = do_hp_mask,
        .recv_stream_data = ::recv_stream_data,
        .acked_stream_data_offset = ::acked_stream_data_offset,
        .stream_open = stream_open,
        .stream_close = stream_close,
        .rand = rand,
        .get_new_connection_id = get_new_connection_id,
        .remove_connection_id = remove_connection_id,
        .update_key = ::update_key,
        .path_validation = path_validation,
        .stream_reset = ::stream_reset,
        .extend_max_remote_streams_bidi = ::extend_max_remote_streams_bidi,
        .extend_max_stream_data = ::extend_max_stream_data,
        .delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
        .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
        .get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb,
        .stream_stop_sending = stream_stop_sending,
        .version_negotiation = ngtcp2_crypto_version_negotiation_cb,
        .recv_tx_key = ::recv_tx_key,
    };
#pragma GCC diagnostic pop

    scid_.datalen = NGTCP2_SV_SCIDLEN;
    if (util::generate_secure_random({scid_.data, scid_.datalen}) != 0)
    {
        std::cerr << "Server Could not generate connection ID" << std::endl;
        return -1;
    }

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.log_printf = config.quiet ? nullptr : debug::log_printf;
    settings.initial_ts = util::timestamp();
    settings.token = token.data();
    settings.tokenlen = token.size();
    settings.token_type = token_type;
    settings.cc_algo = config.cc_algo;
    settings.initial_rtt = config.initial_rtt;
    settings.max_window = config.max_window;
    settings.max_stream_window = config.max_stream_window;
    settings.handshake_timeout = config.handshake_timeout;
    settings.no_pmtud = config.no_pmtud;
    settings.ack_thresh = config.ack_thresh;
    if (config.max_udp_payload_size)
    {
        settings.max_tx_udp_payload_size = config.max_udp_payload_size;
        settings.no_tx_udp_payload_size_shaping = 1;
    }
    if (!config.qlog_dir.empty())
    {
        auto path = std::string{config.qlog_dir};
        path += '/';
        path += util::format_hex({scid_.data, scid_.datalen});
        path += ".sqlog";
        qlog_ = fopen(path.c_str(), "w");
        if (qlog_ == nullptr)
        {
            std::cerr << "Server Could not open qlog file " << std::quoted(path) << ": "
                      << strerror(errno) << std::endl;
            return -1;
        }
        settings.qlog_write = ::write_qlog;
    }
    if (!config.preferred_versions.empty())
    {
        settings.preferred_versions = config.preferred_versions.data();
        settings.preferred_versionslen = config.preferred_versions.size();
    }
    if (!config.available_versions.empty())
    {
        settings.available_versions = config.available_versions.data();
        settings.available_versionslen = config.available_versions.size();
    }
    if (config.initial_pkt_num == UINT32_MAX)
    {
        auto dis = std::uniform_int_distribution<uint32_t>(0, INT32_MAX);
        settings.initial_pkt_num = dis(randgen);
    }
    else
    {
        settings.initial_pkt_num = config.initial_pkt_num;
    }

    if (!config.pmtud_probes.empty())
    {
        settings.pmtud_probes = config.pmtud_probes.data();
        settings.pmtud_probeslen = config.pmtud_probes.size();

        if (!config.max_udp_payload_size)
        {
            settings.max_tx_udp_payload_size = *std::max_element(
                std::begin(config.pmtud_probes), std::end(config.pmtud_probes));
        }
    }

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = config.max_stream_data_bidi_local;
    params.initial_max_stream_data_bidi_remote =
        config.max_stream_data_bidi_remote;
    params.initial_max_stream_data_uni = config.max_stream_data_uni;
    params.initial_max_data = config.max_data;
    params.initial_max_streams_bidi = config.max_streams_bidi;
    params.initial_max_streams_uni = config.max_streams_uni;
    params.max_idle_timeout = config.timeout;
    params.stateless_reset_token_present = 1;
    params.active_connection_id_limit = 7;
    params.grease_quic_bit = 1;

    if (ocid)
    {
        params.original_dcid = *ocid;
        params.retry_scid = *scid;
        params.retry_scid_present = 1;
    }
    else
    {
        params.original_dcid = *scid;
    }

    params.original_dcid_present = 1;

    if (ngtcp2_crypto_generate_stateless_reset_token(
            params.stateless_reset_token, config.static_secret.data(),
            config.static_secret.size(), &scid_) != 0)
    {
        return -1;
    }

    if (config.preferred_ipv4_addr.len || config.preferred_ipv6_addr.len)
    {
        params.preferred_addr_present = 1;

        if (config.preferred_ipv4_addr.len)
        {
            params.preferred_addr.ipv4 = config.preferred_ipv4_addr.su.in;
            params.preferred_addr.ipv4_present = 1;
        }

        if (config.preferred_ipv6_addr.len)
        {
            params.preferred_addr.ipv6 = config.preferred_ipv6_addr.su.in6;
            params.preferred_addr.ipv6_present = 1;
        }

        if (util::generate_secure_random(
                params.preferred_addr.stateless_reset_token) != 0)
        {
            std::cerr << "Server Could not generate preferred address stateless reset token"
                      << std::endl;
            return -1;
        }

        params.preferred_addr.cid.datalen = NGTCP2_SV_SCIDLEN;
        if (util::generate_secure_random({params.preferred_addr.cid.data,
                                          params.preferred_addr.cid.datalen}) !=
            0)
        {
            std::cerr << "Server Could not generate preferred address connection ID"
                      << std::endl;
            return -1;
        }
    }

    auto path = ngtcp2_path{
        .local =
            {
                .addr = const_cast<sockaddr *>(&local_addr.su.sa),
                .addrlen = local_addr.len,
            },
        .remote =
            {
                .addr = const_cast<sockaddr *>(sa),
                .addrlen = salen,
            },
        .user_data = const_cast<Endpoint *>(&ep),
    };
    if (auto rv =
            ngtcp2_conn_server_new(&conn_, dcid, &scid_, &path, version, &callbacks,
                                   &settings, &params, nullptr, this);
        rv != 0)
    {
        std::cerr << "Server ngtcp2_conn_server_new: " << ngtcp2_strerror(rv) << std::endl;
        return -1;
    }

    if (tls_session_.init(tls_ctx, this) != 0)
    {
        return -1;
    }

    tls_session_.enable_keylog();

    ngtcp2_conn_set_tls_native_handle(conn_, tls_session_.get_native_handle());

    ev_io_set(&wev_, ep.fd, EV_WRITE);

    return 0;
}

int Handler::feed_data(const Endpoint &ep, const Address &local_addr,
                       const sockaddr *sa, socklen_t salen,
                       const ngtcp2_pkt_info *pi,
                       std::span<const uint8_t> data)
{
    auto path = ngtcp2_path{
        .local =
            {
                .addr = const_cast<sockaddr *>(&local_addr.su.sa),
                .addrlen = local_addr.len,
            },
        .remote =
            {
                .addr = const_cast<sockaddr *>(sa),
                .addrlen = salen,
            },
        .user_data = const_cast<Endpoint *>(&ep),
    };

    if (auto rv = ngtcp2_conn_read_pkt(conn_, &path, pi, data.data(), data.size(),
                                       util::timestamp());
        rv != 0)
    {
        std::cerr << "Server ngtcp2_conn_read_pkt: " << ngtcp2_strerror(rv) << std::endl;
        switch (rv)
        {
        case NGTCP2_ERR_DRAINING:
            start_draining_period();
            return NETWORK_ERR_CLOSE_WAIT;
        case NGTCP2_ERR_RETRY:
            return NETWORK_ERR_RETRY;
        case NGTCP2_ERR_DROP_CONN:
            return NETWORK_ERR_DROP_CONN;
        case NGTCP2_ERR_CRYPTO:
            if (!last_error_.error_code)
            {
                ngtcp2_ccerr_set_tls_alert(
                    &last_error_, ngtcp2_conn_get_tls_alert(conn_), nullptr, 0);
            }
            break;
        default:
            if (!last_error_.error_code)
            {
                ngtcp2_ccerr_set_liberr(&last_error_, rv, nullptr, 0);
            }
        }
        return handle_error();
    }

    return 0;
}

int Handler::on_read(const Endpoint &ep, const Address &local_addr,
                     const sockaddr *sa, socklen_t salen,
                     const ngtcp2_pkt_info *pi, std::span<const uint8_t> data)
{
    if (auto rv = feed_data(ep, local_addr, sa, salen, pi, data); rv != 0)
    {
        return rv;
    }

    update_timer();

    return 0;
}

int Handler::handle_expiry()
{
    auto now = util::timestamp();
    if (auto rv = ngtcp2_conn_handle_expiry(conn_, now); rv != 0)
    {
        std::cerr << "Server ngtcp2_conn_handle_expiry: " << ngtcp2_strerror(rv)
                  << std::endl;
        ngtcp2_ccerr_set_liberr(&last_error_, rv, nullptr, 0);
        return handle_error();
    }

    return 0;
}

int Handler::on_write()
{
    if (ngtcp2_conn_in_closing_period(conn_) ||
        ngtcp2_conn_in_draining_period(conn_))
    {
        return 0;
    }

    if (tx_.send_blocked)
    {
        if (auto rv = send_blocked_packet(); rv != 0)
        {
            return rv;
        }

        if (tx_.send_blocked)
        {
            return 0;
        }
    }

    ev_io_stop(loop_, &wev_);

    if (auto rv = write_streams(); rv != 0)
    {
        return rv;
    }

    update_timer();

    return 0;
}

int Handler::write_streams()
{
    std::array<nghttp3_vec, 16> vec;
    ngtcp2_path_storage ps, prev_ps;
    uint32_t prev_ecn = 0;
    auto max_udp_payload_size = ngtcp2_conn_get_max_tx_udp_payload_size(conn_);
    auto path_max_udp_payload_size =
        ngtcp2_conn_get_path_max_tx_udp_payload_size(conn_);
    ngtcp2_pkt_info pi;
    size_t gso_size = 0;
    auto ts = util::timestamp();
    auto txbuf =
        std::span{tx_.data.get(), std::max(ngtcp2_conn_get_send_quantum(conn_),
                                           path_max_udp_payload_size)};
    auto buf = txbuf;

    ngtcp2_path_storage_zero(&ps);
    ngtcp2_path_storage_zero(&prev_ps);

    for (;;)
    {
        int64_t stream_id = -1;
        int fin = 0;
        nghttp3_ssize sveccnt = 0;

        if (httpconn_ && ngtcp2_conn_get_max_data_left(conn_))
        {
            sveccnt = nghttp3_conn_writev_stream(httpconn_, &stream_id, &fin,
                                                 vec.data(), vec.size());
            if (sveccnt < 0)
            {
                std::cerr << "Server nghttp3_conn_writev_stream: " << nghttp3_strerror(sveccnt)
                          << std::endl;
                ngtcp2_ccerr_set_application_error(
                    &last_error_, nghttp3_err_infer_quic_app_error_code(sveccnt), nullptr,
                    0);
                return handle_error();
            }
        }

        ngtcp2_ssize ndatalen;
        auto v = vec.data();
        auto vcnt = static_cast<size_t>(sveccnt);

        uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
        if (fin)
        {
            flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
        }

        auto buflen = buf.size() >= max_udp_payload_size
                          ? max_udp_payload_size
                          : path_max_udp_payload_size;

        auto nwrite = ngtcp2_conn_writev_stream(
            conn_, &ps.path, &pi, buf.data(), buflen, &ndatalen, flags, stream_id,
            reinterpret_cast<const ngtcp2_vec *>(v), vcnt, ts);
        if (nwrite < 0)
        {
            switch (nwrite)
            {
            case NGTCP2_ERR_STREAM_DATA_BLOCKED:
                assert(ndatalen == -1);
                nghttp3_conn_block_stream(httpconn_, stream_id);
                continue;
            case NGTCP2_ERR_STREAM_SHUT_WR:
                assert(ndatalen == -1);
                nghttp3_conn_shutdown_stream_write(httpconn_, stream_id);
                continue;
            case NGTCP2_ERR_WRITE_MORE:
                assert(ndatalen >= 0);
                if (auto rv =
                        nghttp3_conn_add_write_offset(httpconn_, stream_id, ndatalen);
                    rv != 0)
                {
                    std::cerr << "Server nghttp3_conn_add_write_offset: " << nghttp3_strerror(rv)
                              << std::endl;
                    ngtcp2_ccerr_set_application_error(
                        &last_error_, nghttp3_err_infer_quic_app_error_code(rv), nullptr,
                        0);
                    return handle_error();
                }
                continue;
            }

            assert(ndatalen == -1);

            std::cerr << "Server ngtcp2_conn_writev_stream: " << ngtcp2_strerror(nwrite)
                      << std::endl;
            ngtcp2_ccerr_set_liberr(&last_error_, nwrite, nullptr, 0);
            return handle_error();
        }
        else if (ndatalen >= 0)
        {
            if (auto rv =
                    nghttp3_conn_add_write_offset(httpconn_, stream_id, ndatalen);
                rv != 0)
            {
                std::cerr << "Server nghttp3_conn_add_write_offset: " << nghttp3_strerror(rv)
                          << std::endl;
                ngtcp2_ccerr_set_application_error(
                    &last_error_, nghttp3_err_infer_quic_app_error_code(rv), nullptr, 0);
                return handle_error();
            }
        }

        if (nwrite == 0)
        {
            auto data = std::span{std::begin(txbuf), std::begin(buf)};
            if (!data.empty())
            {
                auto &ep = *static_cast<Endpoint *>(prev_ps.path.user_data);

                if (auto [rest, rv] = server_->send_packet(
                        ep, no_gso_, prev_ps.path.local, prev_ps.path.remote, prev_ecn,
                        data, gso_size);
                    rv != NETWORK_ERR_OK)
                {
                    assert(NETWORK_ERR_SEND_BLOCKED == rv);

                    on_send_blocked(ep, prev_ps.path.local, prev_ps.path.remote, prev_ecn,
                                    rest, gso_size);

                    start_wev_endpoint(ep);
                }
            }

            // We are congestion limited.
            ngtcp2_conn_update_pkt_tx_time(conn_, ts);
            return 0;
        }

        auto last_pkt = std::begin(buf);

        buf = buf.subspan(nwrite);

        if (last_pkt == std::begin(txbuf))
        {
            ngtcp2_path_copy(&prev_ps.path, &ps.path);
            prev_ecn = pi.ecn;
            gso_size = nwrite;
        }
        else if (!ngtcp2_path_eq(&prev_ps.path, &ps.path) || prev_ecn != pi.ecn ||
                 static_cast<size_t>(nwrite) > gso_size ||
                 (gso_size > path_max_udp_payload_size &&
                  static_cast<size_t>(nwrite) != gso_size))
        {
            auto &ep = *static_cast<Endpoint *>(prev_ps.path.user_data);
            auto data = std::span{std::begin(txbuf), last_pkt};

            if (auto [rest, rv] =
                    server_->send_packet(ep, no_gso_, prev_ps.path.local,
                                         prev_ps.path.remote, prev_ecn, data, gso_size);
                rv != 0)
            {
                assert(NETWORK_ERR_SEND_BLOCKED == rv);

                on_send_blocked(ep, prev_ps.path.local, prev_ps.path.remote, prev_ecn,
                                rest, gso_size);

                data = std::span{last_pkt, std::begin(buf)};
                on_send_blocked(*static_cast<Endpoint *>(ps.path.user_data),
                                ps.path.local, ps.path.remote, pi.ecn, data,
                                data.size());

                start_wev_endpoint(ep);
            }
            else
            {
                auto &ep = *static_cast<Endpoint *>(ps.path.user_data);
                auto data = std::span{last_pkt, std::begin(buf)};

                if (auto [rest, rv] =
                        server_->send_packet(ep, no_gso_, ps.path.local, ps.path.remote,
                                             pi.ecn, data, data.size());
                    rv != 0)
                {
                    assert(rest.size() == data.size());
                    assert(NETWORK_ERR_SEND_BLOCKED == rv);

                    on_send_blocked(ep, ps.path.local, ps.path.remote, pi.ecn, rest,
                                    rest.size());

                    start_wev_endpoint(ep);
                }
            }

            ngtcp2_conn_update_pkt_tx_time(conn_, ts);
            return 0;
        }

        if (buf.size() < path_max_udp_payload_size ||
            static_cast<size_t>(nwrite) < gso_size)
        {
            auto &ep = *static_cast<Endpoint *>(ps.path.user_data);
            auto data = std::span{std::begin(txbuf), std::begin(buf)};

            if (auto [rest, rv] = server_->send_packet(
                    ep, no_gso_, ps.path.local, ps.path.remote, pi.ecn, data, gso_size);
                rv != 0)
            {
                assert(NETWORK_ERR_SEND_BLOCKED == rv);

                on_send_blocked(ep, ps.path.local, ps.path.remote, pi.ecn, rest,
                                gso_size);

                start_wev_endpoint(ep);
            }

            ngtcp2_conn_update_pkt_tx_time(conn_, ts);
            return 0;
        }
    }
}

void Handler::on_send_blocked(Endpoint &ep, const ngtcp2_addr &local_addr,
                              const ngtcp2_addr &remote_addr, unsigned int ecn,
                              std::span<const uint8_t> data, size_t gso_size)
{
    assert(tx_.num_blocked || !tx_.send_blocked);
    assert(tx_.num_blocked < 2);
    assert(gso_size);

    tx_.send_blocked = true;

    auto &p = tx_.blocked[tx_.num_blocked++];

    memcpy(&p.local_addr.su, local_addr.addr, local_addr.addrlen);
    memcpy(&p.remote_addr.su, remote_addr.addr, remote_addr.addrlen);

    p.local_addr.len = local_addr.addrlen;
    p.remote_addr.len = remote_addr.addrlen;
    p.endpoint = &ep;
    p.ecn = ecn;
    p.data = data;
    p.gso_size = gso_size;
}

void Handler::start_wev_endpoint(const Endpoint &ep)
{
    // We do not close ep.fd, so we can expect that each Endpoint has
    // unique fd.
    if (ep.fd != wev_.fd)
    {
        if (ev_is_active(&wev_))
        {
            ev_io_stop(loop_, &wev_);
        }

        ev_io_set(&wev_, ep.fd, EV_WRITE);
    }

    ev_io_start(loop_, &wev_);
}

int Handler::send_blocked_packet()
{
    assert(tx_.send_blocked);

    for (; tx_.num_blocked_sent < tx_.num_blocked; ++tx_.num_blocked_sent)
    {
        auto &p = tx_.blocked[tx_.num_blocked_sent];

        ngtcp2_addr local_addr{
            .addr = &p.local_addr.su.sa,
            .addrlen = p.local_addr.len,
        };
        ngtcp2_addr remote_addr{
            .addr = &p.remote_addr.su.sa,
            .addrlen = p.remote_addr.len,
        };

        auto [rest, rv] = server_->send_packet(
            *p.endpoint, no_gso_, local_addr, remote_addr, p.ecn, p.data, p.gso_size);
        if (rv != 0)
        {
            assert(NETWORK_ERR_SEND_BLOCKED == rv);

            p.data = rest;

            start_wev_endpoint(*p.endpoint);

            return 0;
        }
    }

    tx_.send_blocked = false;
    tx_.num_blocked = 0;
    tx_.num_blocked_sent = 0;

    return 0;
}

void Handler::signal_write() { ev_io_start(loop_, &wev_); }

void Handler::start_draining_period()
{
    ev_io_stop(loop_, &wev_);

    ev_set_cb(&timer_, close_waitcb);
    timer_.repeat =
        static_cast<ev_tstamp>(ngtcp2_conn_get_pto(conn_)) / NGTCP2_SECONDS * 3;
    ev_timer_again(loop_, &timer_);

    if (!config.quiet)
    {
        std::cerr << "Server Draining period has started (" << timer_.repeat << " seconds)"
                  << std::endl;
    }
}

int Handler::start_closing_period()
{
    if (!conn_ || ngtcp2_conn_in_closing_period(conn_) ||
        ngtcp2_conn_in_draining_period(conn_))
    {
        return 0;
    }

    ev_io_stop(loop_, &wev_);

    ev_set_cb(&timer_, close_waitcb);
    timer_.repeat =
        static_cast<ev_tstamp>(ngtcp2_conn_get_pto(conn_)) / NGTCP2_SECONDS * 3;
    ev_timer_again(loop_, &timer_);

    if (!config.quiet)
    {
        std::cerr << "Server Closing period has started (" << timer_.repeat << " seconds)"
                  << std::endl;
    }

    conn_closebuf_ = std::make_unique<Buffer>(NGTCP2_MAX_UDP_PAYLOAD_SIZE);

    ngtcp2_path_storage ps;

    ngtcp2_path_storage_zero(&ps);

    ngtcp2_pkt_info pi;
    auto n = ngtcp2_conn_write_connection_close(
        conn_, &ps.path, &pi, conn_closebuf_->wpos(), conn_closebuf_->left(),
        &last_error_, util::timestamp());
    if (n < 0)
    {
        std::cerr << "Server ngtcp2_conn_write_connection_close: " << ngtcp2_strerror(n)
                  << std::endl;
        return -1;
    }

    if (n == 0)
    {
        return 0;
    }

    conn_closebuf_->push(n);

    return 0;
}

int Handler::handle_error()
{
    if (last_error_.type == NGTCP2_CCERR_TYPE_IDLE_CLOSE)
    {
        return -1;
    }

    if (start_closing_period() != 0)
    {
        return -1;
    }

    if (ngtcp2_conn_in_draining_period(conn_))
    {
        return NETWORK_ERR_CLOSE_WAIT;
    }

    if (auto rv = send_conn_close(); rv != NETWORK_ERR_OK)
    {
        return rv;
    }

    return NETWORK_ERR_CLOSE_WAIT;
}

int Handler::send_conn_close()
{
    if (!config.quiet)
    {
        std::cerr << "Server Closing Period: TX CONNECTION_CLOSE" << std::endl;
    }

    assert(conn_closebuf_ && conn_closebuf_->size());
    assert(conn_);
    assert(!ngtcp2_conn_in_draining_period(conn_));

    auto path = ngtcp2_conn_get_path(conn_);

    return server_->send_packet(*static_cast<Endpoint *>(path->user_data),
                                path->local, path->remote,
                                /* ecn = */ 0, conn_closebuf_->data());
}

void Handler::update_timer()
{
    auto expiry = ngtcp2_conn_get_expiry(conn_);
    auto now = util::timestamp();

    if (expiry <= now)
    {
        if (!config.quiet)
        {
            auto t = static_cast<ev_tstamp>(now - expiry) / NGTCP2_SECONDS;
            std::cerr << "Server Timer has already expired: " << std::fixed << t << "s"
                      << std::defaultfloat << std::endl;
        }

        ev_feed_event(loop_, &timer_, EV_TIMER);

        return;
    }

    auto t = static_cast<ev_tstamp>(expiry - now) / NGTCP2_SECONDS;
    if (!config.quiet)
    {
        std::cerr << "Server Set timer=" << std::fixed << t << "s" << std::defaultfloat
                  << std::endl;
    }
    timer_.repeat = t;
    ev_timer_again(loop_, &timer_);
}

int Handler::recv_stream_data(uint32_t flags, int64_t stream_id,
                              std::span<const uint8_t> data)
{
    if (!config.quiet && !config.no_quic_dump)
    {
        debug::print_stream_data(stream_id, data);
    }

    if (!httpconn_)
    {
        return 0;
    }

    if (ngtcp2_is_bidi_stream(stream_id))
    {
        auto it = streams_.find(stream_id);
        assert(it != std::end(streams_));
        //auto &stream = (*it).second;
    }
    auto nconsumed =
        nghttp3_conn_read_stream(httpconn_, stream_id, data.data(), data.size(),
                                 flags & NGTCP2_STREAM_DATA_FLAG_FIN);
    if (nconsumed < 0)
    {
        std::cerr << "Server nghttp3_conn_read_stream: " << nghttp3_strerror(nconsumed)
                  << std::endl;
        ngtcp2_ccerr_set_application_error(
            &last_error_, nghttp3_err_infer_quic_app_error_code(nconsumed), nullptr,
            0);
        return -1;
    }

    ngtcp2_conn_extend_max_stream_offset(conn_, stream_id, nconsumed);
    ngtcp2_conn_extend_max_offset(conn_, nconsumed);

    return 0;
}

int Handler::update_key(uint8_t *rx_secret, uint8_t *tx_secret,
                        ngtcp2_crypto_aead_ctx *rx_aead_ctx, uint8_t *rx_iv,
                        ngtcp2_crypto_aead_ctx *tx_aead_ctx, uint8_t *tx_iv,
                        const uint8_t *current_rx_secret,
                        const uint8_t *current_tx_secret, size_t secretlen)
{
    auto crypto_ctx = ngtcp2_conn_get_crypto_ctx(conn_);
    auto aead = &crypto_ctx->aead;
    auto keylen = ngtcp2_crypto_aead_keylen(aead);
    auto ivlen = ngtcp2_crypto_packet_protection_ivlen(aead);

    ++nkey_update_;

    std::array<uint8_t, 64> rx_key, tx_key;

    if (ngtcp2_crypto_update_key(conn_, rx_secret, tx_secret, rx_aead_ctx,
                                 rx_key.data(), rx_iv, tx_aead_ctx, tx_key.data(),
                                 tx_iv, current_rx_secret, current_tx_secret,
                                 secretlen) != 0)
    {
        return -1;
    }

    if (!config.quiet && config.show_secret)
    {
        std::cerr << "Server application_traffic rx secret " << nkey_update_ << std::endl;
        debug::print_secrets({rx_secret, secretlen}, {rx_key.data(), keylen},
                             {rx_iv, ivlen});
        std::cerr << "Server application_traffic tx secret " << nkey_update_ << std::endl;
        debug::print_secrets({tx_secret, secretlen}, {tx_key.data(), keylen},
                             {tx_iv, ivlen});
    }

    return 0;
}

Server *Handler::server() { return server_; }

int Handler::on_stream_close(int64_t stream_id, uint64_t app_error_code)
{
    if (!config.quiet)
    {
        std::cerr << "Server QUIC stream " << stream_id << " closed" << std::endl;
    }

    if (httpconn_)
    {
        if (app_error_code == 0)
        {
            app_error_code = NGHTTP3_H3_NO_ERROR;
        }
        auto rv = nghttp3_conn_close_stream(httpconn_, stream_id, app_error_code);
        switch (rv)
        {
        case 0:
            break;
        case NGHTTP3_ERR_STREAM_NOT_FOUND:
            if (ngtcp2_is_bidi_stream(stream_id))
            {
                assert(!ngtcp2_conn_is_local_stream(conn_, stream_id));
                ngtcp2_conn_extend_max_streams_bidi(conn_, 1);
            }
            break;
        default:
            std::cerr << "Server nghttp3_conn_close_stream: " << nghttp3_strerror(rv)
                      << std::endl;
            ngtcp2_ccerr_set_application_error(
                &last_error_, nghttp3_err_infer_quic_app_error_code(rv), nullptr, 0);
            return -1;
        }
    }

    return 0;
}

void Handler::shutdown_read(int64_t stream_id, int app_error_code)
{
    ngtcp2_conn_shutdown_stream_read(conn_, 0, stream_id, app_error_code);
}

namespace
{
    void sreadcb(struct ev_loop */* loop */, ev_io *w, int /* revents */)
    {
        auto ep = static_cast<Endpoint *>(w->data);

        ep->server->on_read(*ep);
    }
} // namespace

void sigterminatehandler(struct ev_loop *loop, ev_async */* watcher */, int /* revents */)
{
    ev_break(loop, EVBREAK_ALL);
}

Server::Server(struct ev_loop *loop, TLSServerContext &tls_ctx, QuicListener &listener)
    : loop_(loop),
      tls_ctx_(tls_ctx),
      stateless_reset_bucket_(NGTCP2_STATELESS_RESET_BURST),
      listener_(listener)
{
    ev_timer_init(
        &stateless_reset_regen_timer_,
        [](struct ev_loop */* loop */, ev_timer *w, int /* revents */)
        {
            auto server = static_cast<Server *>(w->data);

            server->on_stateless_reset_regen();
        },
        0., 1.);
    stateless_reset_regen_timer_.data = this;
}

Server::~Server()
{
    disconnect();
    close();
}

void Server::disconnect()
{
    config.tx_loss_prob = 0;

    for (auto &ep : endpoints_)
    {
        ev_io_stop(loop_, &ep.rev);
    }

    ev_timer_stop(loop_, &stateless_reset_regen_timer_);
    //ev_signal_stop(loop_, &sigintev_);

    while (!handlers_.empty())
    {
        auto it = std::begin(handlers_);
        auto &h = (*it).second;

        h->handle_error();

        remove(h);
    }
}

void Server::close()
{
    for (auto &ep : endpoints_)
    {
        ::close(ep.fd);
    }

    endpoints_.clear();
}

namespace
{
    int create_sock(Address &local_addr, const char *addr, const char *port,
                    int family)
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
        addrinfo hints{
            .ai_flags = AI_PASSIVE,
            .ai_family = family,
            .ai_socktype = SOCK_DGRAM,
        };
#pragma GCC diagnostic pop
        addrinfo *res, *rp;
        int val = 1;

        if (strcmp(addr, "*") == 0)
        {
            addr = nullptr;
        }

        if (auto rv = getaddrinfo(addr, port, &hints, &res); rv != 0)
        {
            std::cerr << "Server getaddrinfo: " << gai_strerror(rv) << std::endl;
            return -1;
        }

        auto res_d = defer(freeaddrinfo, res);

        int fd = -1;

        for (rp = res; rp; rp = rp->ai_next)
        {
            if (rp->ai_protocol != IPPROTO_UDP)
            {
                std::cerr << "Server is creating non UDP socket" << std::endl;
            }
            fd = create_flagged_nonblock_socket(rp->ai_family, rp->ai_socktype,
                                              rp->ai_protocol);
            if (fd == -1)
            {
                continue;
            }

            if (rp->ai_family == AF_INET6)
            {
                if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val,
                               static_cast<socklen_t>(sizeof(val))) == -1)
                {
                    close(fd);
                    continue;
                }

                if (setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &val,
                               static_cast<socklen_t>(sizeof(val))) == -1)
                {
                    close(fd);
                    continue;
                }
            }
            else if (setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &val,
                                static_cast<socklen_t>(sizeof(val))) == -1)
            {
                close(fd);
                continue;
            }

            if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val,
                           static_cast<socklen_t>(sizeof(val))) == -1)
            {
                close(fd);
                continue;
            }

            fd_set_recv_ecn(fd, rp->ai_family);
            fd_set_ip_mtu_discover(fd, rp->ai_family);
            fd_set_ip_dontfrag(fd, family);

            if (bind(fd, rp->ai_addr, rp->ai_addrlen) != -1)
            {
                break;
            }

            close(fd);
        }

        if (!rp)
        {
            std::cerr << "Server Could not bind" << std::endl;
            return -1;
        }

        socklen_t len = sizeof(local_addr.su.storage);
        if (getsockname(fd, &local_addr.su.sa, &len) == -1)
        {
            std::cerr << "Server getsockname: " << strerror(errno) << std::endl;
            close(fd);
            return -1;
        }
        local_addr.len = len;
        local_addr.ifindex = 0;

        return fd;
    }

} // namespace

namespace
{
    int add_endpoint(std::vector<Endpoint> &endpoints, const char *addr,
                     const char *port, int af)
    {
        Address dest;
        auto fd = create_sock(dest, addr, port, af);
        if (fd == -1)
        {
            return -1;
        }

        endpoints.emplace_back();
        auto &ep = endpoints.back();
        ep.addr = dest;
        ep.fd = fd;
        ev_io_init(&ep.rev, sreadcb, 0, EV_READ);

        return 0;
    }
} // namespace

namespace
{
    int add_endpoint(std::vector<Endpoint> &endpoints, const Address &addr)
    {
        auto fd = create_flagged_nonblock_socket(addr.su.sa.sa_family, SOCK_DGRAM, IPPROTO_UDP);
        if (fd == -1)
        {
            std::cerr << "Server socket: " << strerror(errno) << std::endl;
            return -1;
        }

        int val = 1;
        if (addr.su.sa.sa_family == AF_INET6)
        {
            if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val,
                           static_cast<socklen_t>(sizeof(val))) == -1)
            {
                std::cerr << "Server setsockopt: " << strerror(errno) << std::endl;
                close(fd);
                return -1;
            }

            if (setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &val,
                           static_cast<socklen_t>(sizeof(val))) == -1)
            {
                std::cerr << "Server setsockopt: " << strerror(errno) << std::endl;
                close(fd);
                return -1;
            }
        }
        else if (setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &val,
                            static_cast<socklen_t>(sizeof(val))) == -1)
        {
            std::cerr << "Server setsockopt: " << strerror(errno) << std::endl;
            close(fd);
            return -1;
        }

        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val,
                       static_cast<socklen_t>(sizeof(val))) == -1)
        {
            close(fd);
            return -1;
        }

        fd_set_recv_ecn(fd, addr.su.sa.sa_family);
        fd_set_ip_mtu_discover(fd, addr.su.sa.sa_family);
        fd_set_ip_dontfrag(fd, addr.su.sa.sa_family);

        if (bind(fd, &addr.su.sa, addr.len) == -1)
        {
            std::cerr << "Server bind: " << strerror(errno) << std::endl;
            close(fd);
            return -1;
        }

        endpoints.emplace_back(Endpoint{});
        auto &ep = endpoints.back();
        ep.addr = addr;
        ep.fd = fd;
        ev_io_init(&ep.rev, sreadcb, 0, EV_READ);

        return 0;
    }
} // namespace

int Server::init(const char *addr, const char *port)
{
    endpoints_.reserve(4);

    auto ready = false;
    if (!util::numeric_host(addr, AF_INET6) &&
        add_endpoint(endpoints_, addr, port, AF_INET) == 0)
    {
        ready = true;
    }
    if (!util::numeric_host(addr, AF_INET) &&
        add_endpoint(endpoints_, addr, port, AF_INET6) == 0)
    {
        ready = true;
    }
    if (!ready)
    {
        return -1;
    }

    if (config.preferred_ipv4_addr.len &&
        add_endpoint(endpoints_, config.preferred_ipv4_addr) != 0)
    {
        return -1;
    }
    if (config.preferred_ipv6_addr.len &&
        add_endpoint(endpoints_, config.preferred_ipv6_addr) != 0)
    {
        return -1;
    }

    for (auto &ep : endpoints_)
    {
        ep.server = this;
        ep.rev.data = &ep;

        ev_io_set(&ep.rev, ep.fd, EV_READ);

        ev_io_start(loop_, &ep.rev);
    }

    //ev_signal_start(loop_, &sigintev_);

    return 0;
}

int Server::on_read(Endpoint &ep)
{
    sockaddr_union su;
    std::array<uint8_t, 64_k> buf;
    size_t pktcnt = 0;
    ngtcp2_pkt_info pi;

    iovec msg_iov{
        .iov_base = buf.data(),
        .iov_len = buf.size(),
    };

    uint8_t msg_ctrl[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(in6_pktinfo)) +
                     CMSG_SPACE(sizeof(int))];

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    msghdr msg{
        .msg_name = &su,
        .msg_iov = &msg_iov,
        .msg_iovlen = 1,
        .msg_control = msg_ctrl,
    };
#pragma GCC diagnostic pop

    for (; pktcnt < 10;)
    {
        msg.msg_namelen = sizeof(su);
        msg.msg_controllen = sizeof(msg_ctrl);

        auto nread = recvmsg(ep.fd, &msg, 0);
        if (nread == -1)
        {
            if (!(errno == EAGAIN || errno == ENOTCONN))
            {
                std::cerr << "Server recvmsg: " << strerror(errno) << std::endl;
            }
            return 0;
        }

        // Packets less than 21 bytes never be a valid QUIC packet.
        if (nread < 21)
        {
            ++pktcnt;

            continue;
        }

        if (util::prohibited_port(util::port(&su)))
        {
            ++pktcnt;

            continue;
        }

        pi.ecn = msghdr_get_ecn(&msg, su.storage.ss_family);
        auto local_addr = msghdr_get_local_addr(&msg, su.storage.ss_family);
        if (!local_addr)
        {
            ++pktcnt;
            std::cerr << "Server Unable to obtain local address" << std::endl;
            continue;
        }

        auto gso_size = msghdr_get_udp_gro(&msg);
        if (gso_size == 0)
        {
            // This seems strange, but the client and server DO NOT send gso_size in the message header if data.size <= gso_size.
            // So this is not actually gso_size, but the size of the data under those circumstances.
            gso_size = static_cast<size_t>(nread);
        }

        set_port(*local_addr, ep.addr);

        auto data = std::span{buf.data(), static_cast<size_t>(nread)};

        for (; !data.empty();)
        {
            auto datalen = std::min(data.size(), gso_size);

            ++pktcnt;

            if (!config.quiet)
            {
                std::array<char, IF_NAMESIZE> ifname;
                std::cerr << "Server Received packet: local="
                          << util::straddr(&local_addr->su.sa, local_addr->len)
                          << " remote=" << util::straddr(&su.sa, msg.msg_namelen)
                          << " if="
                          << if_indextoname(local_addr->ifindex, ifname.data())
                          << " ecn=0x" << std::hex << static_cast<uint32_t>(pi.ecn)
                          << std::dec << " " << datalen << " bytes" << std::endl;
            }

            // Packets less than 21 bytes never be a valid QUIC packet.
            if (datalen < 21)
            {
                break;
            }

            if (debug::packet_lost(config.rx_loss_prob))
            {
                if (!config.quiet)
                {
                    std::cerr << "Server ** Simulated incoming packet loss **" << std::endl;
                }
            }
            else
            {
                read_pkt(ep, *local_addr, &su.sa, msg.msg_namelen, &pi,
                         {data.data(), datalen});
            }

            data = data.subspan(datalen);
        }
    }

    return 0;
}

int debug_ngtcp2_accept(ngtcp2_pkt_hd *dest, const uint8_t *pkt, size_t pktlen) {
    ngtcp2_ssize nread;
    ngtcp2_pkt_hd hd, *p;
  
    if (dest) {
      p = dest;
    } else {
      p = &hd;
    }
  
    if (pktlen == 0 || (pkt[0] & NGTCP2_HEADER_FORM_BIT) == 0) {
      if (pktlen == 0)
      {std::cerr << "Server returning invalid argument because of pktlen: " << pktlen << std::endl;}
        else {
            std::cerr << "Server returning invalid argument because of pkt[0]: " << pkt[0] << " vs " << NGTCP2_HEADER_FORM_BIT << std::endl;
        }
      
      return NGTCP2_ERR_INVALID_ARGUMENT;
    }
  
    nread = ngtcp2_pkt_decode_hd_long(p, pkt, pktlen);
    if (nread < 0) {
      std::cerr << "Server returning invalid argument because of nread: " << nread << std::endl;
      return (int)nread;
    }
  
    switch (p->type) {
    case NGTCP2_PKT_INITIAL:
      break;
    case NGTCP2_PKT_0RTT:
      /* 0-RTT packet may arrive before Initial packet due to
         re-ordering.  ngtcp2 does not buffer 0RTT packet unless the
         very first Initial packet is received or token is received.
         Previously, we returned NGTCP2_ERR_RETRY here, so that client
         can resend 0RTT data.  But it incurs 1RTT already and
         diminishes the value of 0RTT.  Therefore, we just discard the
         packet here for now. */
    case NGTCP2_PKT_VERSION_NEGOTIATION:
        std::cerr << "Server returning invalid argument because of p->type: NGTCP2_PKT_VERSION_NEGOTIATION" << std::endl;
        return NGTCP2_ERR_INVALID_ARGUMENT;
    default:
        std::cerr << "Server returning invalid argument because of p->type: " << uint64_t(p->type) << std::endl;
      return NGTCP2_ERR_INVALID_ARGUMENT;
    }
  
    if (pktlen < NGTCP2_MAX_UDP_PAYLOAD_SIZE ||
        (p->tokenlen == 0 && p->dcid.datalen < NGTCP2_MIN_INITIAL_DCIDLEN)) {
      if (pktlen < NGTCP2_MIN_LONG_HEADERLEN) {
        std::cerr << "Server returning invalid argument because of pktlen: " << pktlen << std::endl;}
      else {
        std::cerr << "Server returning invalid argument because of tokenlen: " << p->tokenlen << " and dcid.datalen: " << p->dcid.datalen << std::endl;
      }
      return NGTCP2_ERR_INVALID_ARGUMENT;
    }
  
    return 0;
  }
  

void Server::read_pkt(Endpoint &ep, const Address &local_addr,
                      const sockaddr *sa, socklen_t salen,
                      const ngtcp2_pkt_info *pi,
                      std::span<const uint8_t> data)
{
    ngtcp2_version_cid vc;

    switch (auto rv = ngtcp2_pkt_decode_version_cid(&vc, data.data(), data.size(),
                                                    NGTCP2_SV_SCIDLEN);
            rv)
    {
    case 0:
        break;
    case NGTCP2_ERR_VERSION_NEGOTIATION:
        send_version_negotiation(vc.version, {vc.scid, vc.scidlen},
                                 {vc.dcid, vc.dcidlen}, ep, local_addr, sa, salen);
        return;
    default:
        std::cerr << "Server Could not decode version and CID from QUIC packet header: "
                  << ngtcp2_strerror(rv) << std::endl;
        return;
    }

    auto dcid_key = util::make_cid_key({vc.dcid, vc.dcidlen});

    auto handler_it = handlers_.find(dcid_key);
    if (handler_it == std::end(handlers_))
    {
        ngtcp2_pkt_hd hd;

        if (auto rv = debug_ngtcp2_accept(&hd, data.data(), data.size()); rv != 0)
        {
            if (!config.quiet)
            {
                std::cerr << "Server got unexpected packet received: length=" << data.size()
                          << std::endl;
            }

            if (!(data[0] & 0x80) && data.size() >= NGTCP2_SV_SCIDLEN + 21)
            {
                send_stateless_reset(data.size(), {vc.dcid, vc.dcidlen}, ep, local_addr,
                                     sa, salen);
            }

            return;
        }

        ngtcp2_cid ocid;
        ngtcp2_cid *pocid = nullptr;
        ngtcp2_token_type token_type = NGTCP2_TOKEN_TYPE_UNKNOWN;

        assert(hd.type == NGTCP2_PKT_INITIAL);

        if (config.validate_addr || hd.tokenlen)
        {
            std::cerr << "Server Perform stateless address validation" << std::endl;
            if (hd.tokenlen == 0)
            {
                send_retry(&hd, ep, local_addr, sa, salen, data.size() * 3);
                return;
            }

            if (hd.token[0] != NGTCP2_CRYPTO_TOKEN_MAGIC_RETRY2 &&
                hd.dcid.datalen < NGTCP2_MIN_INITIAL_DCIDLEN)
            {
                send_stateless_connection_close(&hd, ep, local_addr, sa, salen);
                return;
            }

            switch (hd.token[0])
            {
            case NGTCP2_CRYPTO_TOKEN_MAGIC_RETRY2:
                switch (verify_retry_token(&ocid, &hd, sa, salen))
                {
                case 0:
                    pocid = &ocid;
                    token_type = NGTCP2_TOKEN_TYPE_RETRY;
                    break;
                case -1:
                    send_stateless_connection_close(&hd, ep, local_addr, sa, salen);
                    return;
                case 1:
                    hd.token = nullptr;
                    hd.tokenlen = 0;
                    break;
                }

                break;
            case NGTCP2_CRYPTO_TOKEN_MAGIC_REGULAR:
                if (verify_token(&hd, sa, salen) != 0)
                {
                    if (config.validate_addr)
                    {
                        send_retry(&hd, ep, local_addr, sa, salen, data.size() * 3);
                        return;
                    }

                    hd.token = nullptr;
                    hd.tokenlen = 0;
                }
                else
                {
                    token_type = NGTCP2_TOKEN_TYPE_NEW_TOKEN;
                }
                break;
            default:
                if (!config.quiet)
                {
                    std::cerr << "Server Ignore unrecognized token" << std::endl;
                }
                if (config.validate_addr)
                {
                    send_retry(&hd, ep, local_addr, sa, salen, data.size() * 3);
                    return;
                }

                hd.token = nullptr;
                hd.tokenlen = 0;
                break;
            }
        }

        auto h = std::make_unique<Handler>(loop_, this);
        if (h->init(ep, local_addr, sa, salen, &hd.scid, &hd.dcid, pocid,
                    {hd.token, hd.tokenlen}, token_type, hd.version,
                    tls_ctx_) != 0)
        {
            return;
        }

        switch (h->on_read(ep, local_addr, sa, salen, pi, data))
        {
        case 0:
            break;
        case NETWORK_ERR_RETRY:
            send_retry(&hd, ep, local_addr, sa, salen, data.size() * 3);
            return;
        default:
            return;
        }

        if (h->on_write() != 0)
        {
            return;
        }

        std::array<ngtcp2_cid, 8> scids;
        auto conn = h->conn();

        auto num_scid = ngtcp2_conn_get_scid(conn, nullptr);

        assert(num_scid <= scids.size());

        ngtcp2_conn_get_scid(conn, scids.data());

        for (size_t i = 0; i < num_scid; ++i)
        {
            associate_cid(&scids[i], h.get());
        }

        handlers_.emplace(dcid_key, h.release());

        return;
    }

    auto h = (*handler_it).second;
    auto conn = h->conn();
    if (ngtcp2_conn_in_closing_period(conn))
    {
        // LEGACY ISSUE do exponential backoff.
        if (h->send_conn_close() != 0)
        {
            remove(h);
        }
        return;
    }
    if (ngtcp2_conn_in_draining_period(conn))
    {
        return;
    }

    if (auto rv = h->on_read(ep, local_addr, sa, salen, pi, data); rv != 0)
    {
        if (rv != NETWORK_ERR_CLOSE_WAIT)
        {
            remove(h);
        }
        return;
    }

    h->signal_write();
}

namespace
{
    uint32_t generate_reserved_version(const sockaddr *sa, socklen_t salen,
                                       uint32_t version)
    {
        uint32_t h = 0x811C9DC5u;
        const uint8_t *p = (const uint8_t *)sa;
        const uint8_t *ep = p + salen;
        for (; p != ep; ++p)
        {
            h ^= *p;
            h *= 0x01000193u;
        }
        version = htonl(version);
        p = (const uint8_t *)&version;
        ep = p + sizeof(version);
        for (; p != ep; ++p)
        {
            h ^= *p;
            h *= 0x01000193u;
        }
        h &= 0xf0f0f0f0u;
        h |= 0x0a0a0a0au;
        return h;
    }
} // namespace

int Server::send_version_negotiation(uint32_t version,
                                     std::span<const uint8_t> dcid,
                                     std::span<const uint8_t> scid,
                                     Endpoint &ep, const Address &local_addr,
                                     const sockaddr *sa, socklen_t salen)
{
    Buffer buf{NGTCP2_MAX_UDP_PAYLOAD_SIZE};
    std::array<uint32_t, 1 + max_preferred_versionslen> sv;

    auto p = std::begin(sv);

    *p++ = generate_reserved_version(sa, salen, version);

    if (config.preferred_versions.empty())
    {
        *p++ = NGTCP2_PROTO_VER_V1;
    }
    else
    {
        for (auto v : config.preferred_versions)
        {
            *p++ = v;
        }
    }

    auto nwrite = ngtcp2_pkt_write_version_negotiation(
        buf.wpos(), buf.left(), std::uniform_int_distribution<uint8_t>()(randgen),
        dcid.data(), dcid.size(), scid.data(), scid.size(), sv.data(),
        p - std::begin(sv));
    if (nwrite < 0)
    {
        std::cerr << "Server ngtcp2_pkt_write_version_negotiation: "
                  << ngtcp2_strerror(nwrite) << std::endl;
        return -1;
    }

    buf.push(nwrite);

    ngtcp2_addr laddr{
        .addr = const_cast<sockaddr *>(&local_addr.su.sa),
        .addrlen = local_addr.len,
    };
    ngtcp2_addr raddr{
        .addr = const_cast<sockaddr *>(sa),
        .addrlen = salen,
    };

    if (send_packet(ep, laddr, raddr, /* ecn = */ 0, buf.data()) !=
        NETWORK_ERR_OK)
    {
        return -1;
    }

    return 0;
}

int Server::send_retry(const ngtcp2_pkt_hd *chd, Endpoint &ep,
                       const Address &local_addr, const sockaddr *sa,
                       socklen_t salen, size_t max_pktlen)
{
    std::array<char, NI_MAXHOST> host;
    std::array<char, NI_MAXSERV> port;

    if (auto rv = getnameinfo(sa, salen, host.data(), host.size(), port.data(),
                              port.size(), NI_NUMERICHOST | NI_NUMERICSERV);
        rv != 0)
    {
        std::cerr << "Server getnameinfo: " << gai_strerror(rv) << std::endl;
        return -1;
    }

    if (!config.quiet)
    {
        std::cerr << "Server Sending Retry packet to [" << host.data()
                  << "]:" << port.data() << std::endl;
    }

    ngtcp2_cid scid;

    scid.datalen = NGTCP2_SV_SCIDLEN;
    if (util::generate_secure_random({scid.data, scid.datalen}) != 0)
    {
        return -1;
    }

    std::array<uint8_t, NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN2> token;

    auto t = std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();

    auto tokenlen = ngtcp2_crypto_generate_retry_token2(
        token.data(), config.static_secret.data(), config.static_secret.size(),
        chd->version, sa, salen, &scid, &chd->dcid, t);
    if (tokenlen < 0)
    {
        return -1;
    }

    if (!config.quiet)
    {
        std::cerr << "Server Generated address validation token:" << std::endl;
        util::hexdump(stderr, token.data(), tokenlen);
    }

    Buffer buf{
        std::min(static_cast<size_t>(NGTCP2_MAX_UDP_PAYLOAD_SIZE), max_pktlen)};

    auto nwrite =
        ngtcp2_crypto_write_retry(buf.wpos(), buf.left(), chd->version, &chd->scid,
                                  &scid, &chd->dcid, token.data(), tokenlen);
    if (nwrite < 0)
    {
        std::cerr << "Server ngtcp2_crypto_write_retry failed" << std::endl;
        return -1;
    }

    buf.push(nwrite);

    ngtcp2_addr laddr{
        .addr = const_cast<sockaddr *>(&local_addr.su.sa),
        .addrlen = local_addr.len,
    };
    ngtcp2_addr raddr{
        .addr = const_cast<sockaddr *>(sa),
        .addrlen = salen,
    };

    if (send_packet(ep, laddr, raddr, /* ecn = */ 0, buf.data()) !=
        NETWORK_ERR_OK)
    {
        return -1;
    }

    return 0;
}

int Server::send_stateless_connection_close(const ngtcp2_pkt_hd *chd,
                                            Endpoint &ep,
                                            const Address &local_addr,
                                            const sockaddr *sa,
                                            socklen_t salen)
{
    Buffer buf{NGTCP2_MAX_UDP_PAYLOAD_SIZE};

    auto nwrite = ngtcp2_crypto_write_connection_close(
        buf.wpos(), buf.left(), chd->version, &chd->scid, &chd->dcid,
        NGTCP2_INVALID_TOKEN, nullptr, 0);
    if (nwrite < 0)
    {
        std::cerr << "Server ngtcp2_crypto_write_connection_close failed" << std::endl;
        return -1;
    }

    buf.push(nwrite);

    ngtcp2_addr laddr{
        .addr = const_cast<sockaddr *>(&local_addr.su.sa),
        .addrlen = local_addr.len,
    };
    ngtcp2_addr raddr{
        .addr = const_cast<sockaddr *>(sa),
        .addrlen = salen,
    };

    if (send_packet(ep, laddr, raddr, /* ecn = */ 0, buf.data()) !=
        NETWORK_ERR_OK)
    {
        return -1;
    }

    return 0;
}

int Server::send_stateless_reset(size_t pktlen, std::span<const uint8_t> dcid,
                                 Endpoint &ep, const Address &local_addr,
                                 const sockaddr *sa, socklen_t salen)
{
    if (stateless_reset_bucket_ == 0)
    {
        return 0;
    }

    --stateless_reset_bucket_;

    if (!ev_is_active(&stateless_reset_regen_timer_))
    {
        ev_timer_again(loop_, &stateless_reset_regen_timer_);
    }

    ngtcp2_cid cid;

    ngtcp2_cid_init(&cid, dcid.data(), dcid.size());

    std::array<uint8_t, NGTCP2_STATELESS_RESET_TOKENLEN> token;

    if (ngtcp2_crypto_generate_stateless_reset_token(
            token.data(), config.static_secret.data(), config.static_secret.size(),
            &cid) != 0)
    {
        return -1;
    }

    // SCID + minimum expansion - NGTCP2_STATELESS_RESET_TOKENLEN
    constexpr size_t max_rand_byteslen =
        NGTCP2_MAX_CIDLEN + 22 - NGTCP2_STATELESS_RESET_TOKENLEN;

    size_t rand_byteslen;

    if (pktlen <= 43)
    {
        // As per
        // https://datatracker.ietf.org/doc/html/rfc9000#section-10.3
        rand_byteslen = pktlen - NGTCP2_STATELESS_RESET_TOKENLEN - 1;
    }
    else
    {
        rand_byteslen = max_rand_byteslen;
    }

    std::array<uint8_t, max_rand_byteslen> rand_bytes;

    if (util::generate_secure_random({rand_bytes.data(), rand_byteslen}) != 0)
    {
        return -1;
    }

    Buffer buf{NGTCP2_MAX_UDP_PAYLOAD_SIZE};

    auto nwrite = ngtcp2_pkt_write_stateless_reset(
        buf.wpos(), buf.left(), token.data(), rand_bytes.data(), rand_byteslen);
    if (nwrite < 0)
    {
        std::cerr << "Server ngtcp2_pkt_write_stateless_reset: " << ngtcp2_strerror(nwrite)
                  << std::endl;

        return -1;
    }

    buf.push(nwrite);

    ngtcp2_addr laddr{
        .addr = const_cast<sockaddr *>(&local_addr.su.sa),
        .addrlen = local_addr.len,
    };
    ngtcp2_addr raddr{
        .addr = const_cast<sockaddr *>(sa),
        .addrlen = salen,
    };

    if (send_packet(ep, laddr, raddr, /* ecn = */ 0, buf.data()) !=
        NETWORK_ERR_OK)
    {
        return -1;
    }

    return 0;
}

int Server::verify_retry_token(ngtcp2_cid *ocid, const ngtcp2_pkt_hd *hd,
                               const sockaddr *sa, socklen_t salen)
{
    int rv;

    if (!config.quiet)
    {
        std::array<char, NI_MAXHOST> host;
        std::array<char, NI_MAXSERV> port;

        if (auto rv = getnameinfo(sa, salen, host.data(), host.size(), port.data(),
                                  port.size(), NI_NUMERICHOST | NI_NUMERICSERV);
            rv != 0)
        {
            std::cerr << "Server getnameinfo: " << gai_strerror(rv) << std::endl;
            return -1;
        }

        std::cerr << "Server Verifying Retry token from [" << host.data()
                  << "]:" << port.data() << std::endl;
        util::hexdump(stderr, hd->token, hd->tokenlen);
    }

    auto t = std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();

    rv = ngtcp2_crypto_verify_retry_token2(
        ocid, hd->token, hd->tokenlen, config.static_secret.data(),
        config.static_secret.size(), hd->version, sa, salen, &hd->dcid,
        10 * NGTCP2_SECONDS, t);
    switch (rv)
    {
    case 0:
        break;
    case NGTCP2_CRYPTO_ERR_VERIFY_TOKEN:
        std::cerr << "Server Could not verify Retry token" << std::endl;

        return -1;
    default:
        std::cerr << "Server Could not read Retry token.  Continue without the token"
                  << std::endl;

        return 1;
    }

    if (!config.quiet)
    {
        std::cerr << "Server Token was successfully validated" << std::endl;
    }

    return 0;
}

int Server::verify_token(const ngtcp2_pkt_hd *hd, const sockaddr *sa,
                         socklen_t salen)
{
    std::array<char, NI_MAXHOST> host;
    std::array<char, NI_MAXSERV> port;

    if (auto rv = getnameinfo(sa, salen, host.data(), host.size(), port.data(),
                              port.size(), NI_NUMERICHOST | NI_NUMERICSERV);
        rv != 0)
    {
        std::cerr << "Server getnameinfo: " << gai_strerror(rv) << std::endl;
        return -1;
    }

    if (!config.quiet)
    {
        std::cerr << "Server Verifying token from [" << host.data() << "]:" << port.data()
                  << std::endl;
        util::hexdump(stderr, hd->token, hd->tokenlen);
    }

    auto t = std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();

    if (ngtcp2_crypto_verify_regular_token(hd->token, hd->tokenlen,
                                           config.static_secret.data(),
                                           config.static_secret.size(), sa, salen,
                                           3600 * NGTCP2_SECONDS, t) != 0)
    {
        if (!config.quiet)
        {
            std::cerr << "Server Could not verify token" << std::endl;
        }
        return -1;
    }

    if (!config.quiet)
    {
        std::cerr << "Server Token was successfully validated" << std::endl;
    }

    return 0;
}

int Server::send_packet(Endpoint &ep, const ngtcp2_addr &local_addr,
                        const ngtcp2_addr &remote_addr, unsigned int ecn,
                        std::span<const uint8_t> data)
{
    auto no_gso = false;
    auto [_, rv] =
        send_packet(ep, no_gso, local_addr, remote_addr, ecn, data, data.size());

    return rv;
}

std::pair<std::span<const uint8_t>, int>
Server::send_packet(Endpoint &ep, bool &no_gso, const ngtcp2_addr &local_addr,
                    const ngtcp2_addr &remote_addr, unsigned int ecn,
                    std::span<const uint8_t> data, size_t gso_size)
{
    assert(gso_size);

    if (debug::packet_lost(config.tx_loss_prob))
    {
        if (!config.quiet)
        {
            std::cerr << "Server ** Simulated outgoing packet loss **" << std::endl;
        }
        return {{}, NETWORK_ERR_OK};
    }

    if (no_gso && data.size() > gso_size)
    {
        for (; !data.empty();)
        {
            auto len = std::min(gso_size, data.size());

            auto [_, rv] = send_packet(ep, no_gso, local_addr, remote_addr, ecn,
                                       {std::begin(data), len}, len);
            if (rv != 0)
            {
                return {data, rv};
            }

            data = data.subspan(len);
        }

        return {{}, 0};
    }

    iovec msg_iov{
        .iov_base = const_cast<uint8_t *>(data.data()),
        .iov_len = data.size(),
    };

    uint8_t msg_ctrl[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(uint16_t)) +
                     CMSG_SPACE(sizeof(in6_pktinfo))]{};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    msghdr msg{
        .msg_name = const_cast<sockaddr *>(remote_addr.addr),
        .msg_namelen = remote_addr.addrlen,
        .msg_iov = &msg_iov,
        .msg_iovlen = 1,
        .msg_control = msg_ctrl,
        .msg_controllen = sizeof(msg_ctrl),
    };
#pragma GCC diagnostic pop

    size_t controllen = 0;

    auto cm = CMSG_FIRSTHDR(&msg);

    switch (local_addr.addr->sa_family)
    {
    case AF_INET:
    {
        controllen += CMSG_SPACE(sizeof(in_pktinfo));
        cm->cmsg_level = IPPROTO_IP;
        cm->cmsg_type = IP_PKTINFO;
        cm->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
        auto addrin = reinterpret_cast<sockaddr_in *>(local_addr.addr);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
        in_pktinfo pktinfo{
            .ipi_spec_dst = addrin->sin_addr,
        };
#pragma GCC diagnostic pop
        memcpy(CMSG_DATA(cm), &pktinfo, sizeof(pktinfo));

        break;
    }
    case AF_INET6:
    {
        controllen += CMSG_SPACE(sizeof(in6_pktinfo));
        cm->cmsg_level = IPPROTO_IPV6;
        cm->cmsg_type = IPV6_PKTINFO;
        cm->cmsg_len = CMSG_LEN(sizeof(in6_pktinfo));
        auto addrin = reinterpret_cast<sockaddr_in6 *>(local_addr.addr);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
        in6_pktinfo pktinfo{
            .ipi6_addr = addrin->sin6_addr,
        };
#pragma GCC diagnostic pop
        memcpy(CMSG_DATA(cm), &pktinfo, sizeof(pktinfo));

        break;
    }
    default:
        assert(0);
    }

#ifdef UDP_SEGMENT
    if (data.size() > gso_size)
    {
        controllen += CMSG_SPACE(sizeof(uint16_t));
        cm = CMSG_NXTHDR(&msg, cm);
        cm->cmsg_level = SOL_UDP;
        cm->cmsg_type = UDP_SEGMENT;
        cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
        uint16_t n = gso_size;
        memcpy(CMSG_DATA(cm), &n, sizeof(n));
    }
#endif // defined(UDP_SEGMENT)

    controllen += CMSG_SPACE(sizeof(int));
    cm = CMSG_NXTHDR(&msg, cm);
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &ecn, sizeof(ecn));

    switch (local_addr.addr->sa_family)
    {
    case AF_INET:
        cm->cmsg_level = IPPROTO_IP;
        cm->cmsg_type = IP_TOS;

        break;
    case AF_INET6:
        cm->cmsg_level = IPPROTO_IPV6;
        cm->cmsg_type = IPV6_TCLASS;

        break;
    default:
        assert(0);
    }

    msg.msg_controllen = controllen;

    ssize_t nwrite = 0;

    do
    {
        nwrite = sendmsg(ep.fd, &msg, 0);
    } while (nwrite == -1 && errno == EINTR);

    if (nwrite == -1)
    {
        switch (errno)
        {
        case EAGAIN:
#if EAGAIN != EWOULDBLOCK
        case EWOULDBLOCK:
#endif // EAGAIN != EWOULDBLOCK
            return {data, NETWORK_ERR_SEND_BLOCKED};
#ifdef UDP_SEGMENT
        case EIO:
            if (data.size() > gso_size)
            {
                // GSO failure; send each packet in a separate sendmsg call.
                std::cerr << "Server sendmsg: disabling GSO due to " << strerror(errno)
                          << std::endl;

                no_gso = true;

                return send_packet(ep, no_gso, local_addr, remote_addr, ecn, data,
                                   gso_size);
            }
            break;
#endif // defined(UDP_SEGMENT)
        }

        std::cerr << "Server sendmsg: " << strerror(errno) << std::endl;
        // LEGACY ISSUE We have packet which is expected to fail to send (e.g.,
        // path validation to old path).
        return {{}, NETWORK_ERR_OK};
    }

    if (!config.quiet)
    {
        std::cerr << "Server Sent packet: local="
                  << util::straddr(local_addr.addr, local_addr.addrlen)
                  << " remote="
                  << util::straddr(remote_addr.addr, remote_addr.addrlen)
                  << " ecn=0x" << std::hex << ecn << std::dec << " " << nwrite
                  << " bytes" << std::endl;
    }

    return {{}, NETWORK_ERR_OK};
}

void Server::associate_cid(const ngtcp2_cid *cid, Handler *h)
{
    handlers_.emplace(*cid, h);
}

void Server::dissociate_cid(const ngtcp2_cid *cid) { handlers_.erase(*cid); }

void Server::remove(const Handler *h)
{
    auto conn = h->conn();

    dissociate_cid(ngtcp2_conn_get_client_initial_dcid(conn));

    std::vector<ngtcp2_cid> cids(ngtcp2_conn_get_scid(conn, nullptr));
    ngtcp2_conn_get_scid(conn, cids.data());

    for (auto &cid : cids)
    {
        dissociate_cid(&cid);
    }

    delete h;
}

void Server::on_stateless_reset_regen()
{
    assert(stateless_reset_bucket_ < NGTCP2_STATELESS_RESET_BURST);

    if (++stateless_reset_bucket_ == NGTCP2_STATELESS_RESET_BURST)
    {
        ev_timer_stop(loop_, &stateless_reset_regen_timer_);
    }
}

void Server::writecb_start(StreamIdentifier sid)
{
    auto it = handlers_.find(sid.cid);
    if (it == handlers_.end())
    {
        return;
    }

    auto h = (*it).second;

    h->writecb_start();
}

StreamIdentifier QuicListener::getNewRequestStreamIdentifier(Request const &) {
    throw std::runtime_error("Not implemented");
}

void QuicListener::registerResponseHandler(StreamIdentifier sid, stream_callback_fn cb) {
    lock_guard<std::mutex> lock(requestorQueueMutex);
    requestorQueue.push_back(std::make_pair(sid, cb));
}

bool QuicListener::hasResponseHandler(StreamIdentifier sid) {
    lock_guard<std::mutex> lock(requestorQueueMutex);
    auto it = std::find_if(requestorQueue.begin(), requestorQueue.end(),
        [&sid](const stream_callback& stream_cb) {
            return stream_cb.first == sid;
        });
    if (it != requestorQueue.end()) {
        // The handler is already registered
        return true;
    }
    return false;
}

void QuicListener::deregisterResponseHandler(StreamIdentifier sid) {
    lock_guard<std::mutex> lock(requestorQueueMutex);
    auto it = std::remove_if(requestorQueue.begin(), requestorQueue.end(),
        [&sid](const stream_callback& stream_cb) {
            return stream_cb.first == sid;
        });
    requestorQueue.erase(it, requestorQueue.end());
}

bool QuicListener::processRequestStream() {
    std::unique_lock<std::mutex> requestorLock(requestorQueueMutex);
    if (!requestorQueue.empty()) {
        stream_callback stream_cb = requestorQueue.front();
        requestorQueue.erase(requestorQueue.begin());
        requestorLock.unlock();
        requestorLock.lock();
        requestorLock.unlock();
        StreamIdentifier stream_id = stream_cb.first;
        // Now the stream callback is not on the queue, so no other worker will try to service it while this is operational.
        // Call the callback function OUTSIDE the lock
        stream_callback_fn fn = stream_cb.second;
        chunks to_process;
        {
            lock_guard<std::mutex> lock(incomingChunksMutex);
            auto incoming = incomingChunks.find(stream_id);
            if (incoming != incomingChunks.end()) {
                to_process.swap(incoming->second);
            }
        }
        chunks processed = fn(stream_cb.first, to_process);
        if(!processed.empty())
        {   
            lock_guard<std::mutex> lock(outgoingChunksMutex);
            auto outgoing = outgoingChunks.find(stream_cb.first);
            if (outgoing == outgoingChunks.end()) {
                auto inserted = outgoingChunks.insert(make_pair(stream_cb.first, chunks()));
                inserted.first->second.swap(processed);
            }
            else {
                for (auto &chunk : processed) {
                    outgoing->second.emplace_back(chunk);
                }
            }
        }
        requestorLock.lock();
        requestorQueue.push_back(stream_cb);
    } else {
        return false;
    }
    return true;
}

void QuicListener::registerRequestHandler(named_prepare_fn preparer)
{
    lock_guard<std::mutex> lock(preparerStackMutex);
    preparersStack.push_back(preparer);
}

void QuicListener::deregisterRequestHandler(string preparer_name)
{
    lock_guard<std::mutex> lock(preparerStackMutex);
    auto it = std::remove_if(preparersStack.begin(), preparersStack.end(),
        [&preparer_name](const named_prepare_fn& preparer) {
            return preparer.first == preparer_name;
        });
    preparersStack.erase(it, preparersStack.end());
}

uri_response_info QuicListener::prepareInfo(const Request& req)
{
    lock_guard<std::mutex> lock(preparerStackMutex);
    for (auto &preparer : preparersStack) {
        auto response = preparer.second(req);
        auto response_info = response.first;
        if (response_info.can_handle) {
            return response_info;
        }
    }
    return {false, false, false, 0};
}

stream_callback_fn global_noop_stream_callback = [](StreamIdentifier, chunks) { return chunks(); };

uri_response_info QuicListener::prepareHandler(StreamIdentifier sid, const Request& req)
{
    unique_lock<std::mutex> preparerlock(preparerStackMutex);
    for (auto &preparer : preparersStack) {
        auto response = preparer.second(req);
        preparerlock.unlock();
        auto stream_callback = make_pair(sid, response.second);
        auto response_info = response.first;
        if (response_info.can_handle) {
            if (!response_info.is_file)
            {
                if (!response_info.is_live_stream)
                {
                    chunks no_input;
                    auto static_chunks = response.second(sid, no_input);
                    lock_guard<std::mutex> lock(outgoingChunksMutex);
                    auto outgoing = outgoingChunks.find(sid);
                    if (outgoing == outgoingChunks.end()) {
                        auto inserted = outgoingChunks.insert(make_pair(sid, chunks()));
                        inserted.first->second.swap(static_chunks);
                    }
                    else {
                        for (auto &chunk : static_chunks) {
                            outgoing->second.emplace_back(chunk);
                        }
                    }
                    stream_callback = make_pair(sid, global_noop_stream_callback);
                }
                {
                    lock_guard<std::mutex> lock(responderQueueMutex);
                    responderQueue.push_back(stream_callback);
                }
                {
                    lock_guard<std::mutex> lock(returnPathsMutex);
                    returnPaths.insert(make_pair(sid, req));
                }
            }
            return response_info;
        }
    }
    return {false, false, false, 0};
}

bool QuicListener::processResponseStream() {
    std::unique_lock<std::mutex> responderLock(responderQueueMutex);
    if (!responderQueue.empty()) {
        stream_callback stream_cb = responderQueue.front();
        responderQueue.erase(responderQueue.begin());
        responderLock.unlock();
        // Now the stream callback is not on the queue, so no other worker will try to service it while this is operational.
        // Call the callback function OUTSIDE the lock
        stream_callback_fn fn = stream_cb.second;
        chunks to_process;
        bool signal_closed = false;
        {
            lock_guard<std::mutex> lock(incomingChunksMutex);
            auto incoming = incomingChunks.find(stream_cb.first);
            if (incoming != incomingChunks.end()) {
                to_process.swap(incoming->second);
            }
        }
        chunks processed = fn(stream_cb.first, to_process);
        size_t chunk_count = processed.size();
        if(!processed.empty())
        {   
            unique_lock<std::mutex> lock(outgoingChunksMutex);
            auto outgoing = outgoingChunks.find(stream_cb.first);
            if (processed.size() == 1 && processed[0].get_signal_type() == signal_chunk_header::GLOBAL_SIGNAL_TYPE) {
                // If the processed chunk is a signal chunk, we need to check if it is a close stream signal
                auto signal = processed[0].get_signal<signal_chunk_header>();
                if (signal.signal == signal_chunk_header::SIGNAL_CLOSE_STREAM) {
                    if (outgoing == outgoingChunks.end()) {
                        // No chunks left to send
                        signal_closed = true;
                        lock.unlock();
                        processed = fn(stream_cb.first, processed);  // Feed the signal close back to the callback so it can cleanup too. 
                    }
                }
            }
            else {
                if (outgoing == outgoingChunks.end()) {
                    auto inserted = outgoingChunks.insert(make_pair(stream_cb.first, chunks()));
                    inserted.first->second.swap(processed);
                }
                else {
                    for (auto &chunk : processed) {
                        outgoing->second.emplace_back(chunk);
                    }
                }
            }
        }
        if (!signal_closed) {
            // If the stream is not closed, we need to push it back to the requestorQueue
            // This is done by locking the requestorQueueMutex again
            responderLock.lock();
            responderQueue.push_back(stream_cb);
            if (chunk_count > 0) {
                server->writecb_start(stream_cb.first);
            }
        }
        else {
            // If the stream is closed, we need to also remove any remainders from the incomingChunks and outgoingChunks
            {
                lock_guard<std::mutex> lock(incomingChunksMutex);
                incomingChunks.erase(stream_cb.first);
            }
            {
                lock_guard<std::mutex> lock(outgoingChunksMutex);
                outgoingChunks.erase(stream_cb.first);
            }
            {
                lock_guard<std::mutex> lock(returnPathsMutex);
                returnPaths.erase(stream_cb.first);
            }
            // And by definition, it is not in the requestorQueue anymore
        }
    } else {
        return false;
    }
    return true;
}

void QuicListener::check_deadline()
{
    if (timer.expires_at() <= boost::asio::deadline_timer::traits_type::now())
    {
        timed_out = true;
        socket.cancel();
        timer.expires_at(boost::posix_time::pos_infin);
    }
    timer.async_wait([this](const boost::system::error_code &)
                     { check_deadline(); });
}

void QuicListener::listen(const string &local_name, const string &local_ip_addr, int local_port)
{
    reqrep_thread_ = thread([this, local_name, local_ip_addr, local_port]()
                            {
        is_server = true;
        // This needs to more or less duplicate the server.cc main function
        
        config.port = local_port;

        if (auto mt = util::read_mime_types(config.mime_types_file); !mt) {
            std::cerr << "Server mime-types-file: Could not read MIME media types file "
                    << std::quoted(config.mime_types_file) << std::endl;
        } else {
            config.mime_types = std::move(*mt);
        }
        
        TLSServerContext tls_ctx;

        std::cerr << "Server About to initialize TLS server context with private key file " << private_key_file << " and certificate file " << cert_file << std::endl;
        if (tls_ctx.init(private_key_file.c_str(), cert_file.c_str(), AppProtocol::H3) != 0) {
            exit(EXIT_FAILURE);
        }
        std::cerr << "Server Finished initializing TLS server context" << std::endl;

        if (config.htdocs.back() != '/') {
            config.htdocs += '/';
        }
        
        std::cerr << "Server Using document root " << config.htdocs << std::endl;

        //auto ev_loop_d = defer(ev_loop_destroy, loop);

        if (!config.keylog_filename.empty()) {
            auto server_keylog_file = config.keylog_filename + ".server";
            keylog_file.open(server_keylog_file.c_str(), std::ios_base::app);
            if (keylog_file) {
            tls_ctx.enable_keylog();
            }
        }

        if (util::generate_secret(config.static_secret) != 0) {
            std::cerr << "Server Unable to generate static secret" << std::endl;
            exit(EXIT_FAILURE);
        }

        Server s(loop, tls_ctx, *this);
        server = &s;
        auto port_str = std::to_string(config.port);
        if (s.init(local_ip_addr.c_str(), port_str.c_str()) != 0) {
            exit(EXIT_FAILURE);
        }
    
        while(!terminate_.load())
        {
            ev_run(loop, EVRUN_ONCE);
        }
    
        s.disconnect();
        s.close();
        return EXIT_SUCCESS; 
    });
}

void QuicListener::close()
{
    socket.close();
}

void QuicListener::connect(const string &, const string &, int)
{
    // throw an exception, this is not supported
    throw std::runtime_error("QuicListener::connect is not supported");
}