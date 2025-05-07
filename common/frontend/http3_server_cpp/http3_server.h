#pragma once

#include "backend.h"
#include "quic_listener.h"

using ResumeCallback = std::function<pair<chunks, fplus::maybe<ResumeCallback> >(const StreamIdentifier&, const chunks&)>;

// This HTTP3Server class does not need to perform authentication or authorization, because that job will be handled by the
// HTTP3Authenticator service, which stands between clients and these HTTP3Server instances.  The job of the HTTP3Authenticator
// is to block scid creation by unauthorized clients, and to filter static asset request, various tree reads, 
// various tree writes (separate level of authority), and to filter the returning journal notifications.
// This does imply one thing about journal notifications: the response can contain just the sequence number,
// hiding the notification itself, but keeping the client up-to-date without revealing secrets.

// The HTTP3Server class is a utility class which operates inside the Server Handler callback in the QuicListener.
// It also provides static "assets" for urls which does not require the callback listener.
// If an url is NOT a chunk stream, then the assumption is that the url is a static asset and can be immediately served.
// On the other hand, the journaling and backend api support will be chunk stream mode.
// In fact, by default, the distinction for is_chunk_stream just checks if the url contains the path /wwatp 
// WWATP tree protocol then functions beyond the /wwatp path point, and multiple different sets of journals can 
// function independently of each other.
class Http3Server {
public:
    Http3Server(Backend& backend);
    ~Http3Server();

    // A some things of note:
    // * For the purpose of keeping Journaling sane, all clients are assumed to recieve the same notifications. 
    //   If some clients really want to have very different notification sets, then they really should connect to different servers.
    //   Since multiple servers can be connected to the same backend, this is not a problem.
    // * Whenever a client registers as a listener, the server will add the listener for all clients, and also track the total
    //   set of listening labels.  This obviates the need to track callbacks separately for each client.
    // * We will try the following algorithm for server reset:
    //   * If the client requests a notification sequence number > than the server last notification, then the server will send WRONG_JOURNAL_SEQUENCE
    //   * If the client requests a notification sequence number < than the server first notification, then the server will send WRONG_JOURNAL_SEQUENCE
    //   * Thus, the the worst case scenario seems to be if the server restarts, and the sequence number grows past the client's last notification 
    //     before the client can request the next notification.  To fail, the client would have to wait to request a notification more seconds than
    //     the sequence number / notifications per second.  If the server has been running a long time, then this is unlikely to happen. It is more
    //     of a problem when the service is bouncing a lot.
    // * The server can also add in unsolicited notifications without any sequence number.  This is to support changes the client 
    //   tried to make, but the server rejected.  The unsolicited notifications will be undoing the attempted changes.
    chunks processResponseStream(const StreamIdentifier& stream_id, chunks& request);

    ResumeCallback intializeResumeCallback(const StreamIdentifier& stream_id, chunks& request) const;

private:
    Backend& backend_;
    std::map<const StreamIdentifier&, ResumeCallback> ongoingResponses_;
    // Journal is an ordered list of notifications. 
    // The concept here is that if the server cannot provde the all the notifications back to the client,
    // then the client will have to "start over" and request the mutable part of the tree again.
    // Of course, the client is allowed to request static parts of the tree as well.
    uint64_t lastNotificationIndex_;
    using Notification = std::pair<std::string, fplus::maybe<TreeNode>>;
    using SequentialNotification = std::pair<uint64_t, Notification>;
    std::vector<SequentialNotification> journal_;
    std::set<std::string> listeningLabels_;
};

class HTTP3ServerRouter {
    public:
        HTTP3ServerRouter(std::map<std::string, chunks>&& staticAssets) : staticAssets_(std::move(staticAssets)) {};
        ~HTTP3ServerRouter() = default;

        void addBackendRoute(std::string const& url, Backend& backend) {
            servers_.emplace(url, backend);
        }
        void addStaticAsset(std::string const& url, chunks& asset) {
            staticAssets_.emplace(url, asset);
        }

        bool isChunkStream(std::string const& url) const;
        // The static asset method servers static assets from the server, and also has special case logic for /index.html (and /),
        // and will ususally compose it from the backend index node page. It will also usually inject the css and js from the tree into the page.
        chunks staticAsset(std::string const& url) const;
        // The chunk stream handler in the router contains logic to interpret request chunks as calls to the WWATB backend, and route them correctly.
        chunks processResponseStream(const StreamIdentifier& stream_id, chunks& request);    

    private:
        std::map<std::string, Http3Server> servers_;
        std::map<std::string, chunks> staticAssets_;
        std::map<StreamIdentifier, std::string> requestRoutes_;
};