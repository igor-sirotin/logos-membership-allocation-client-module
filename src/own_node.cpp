#include "own_node.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <libp2p.h>

using json = nlohmann::json;

namespace {

constexpr int kCreateWaitMs = 10000;
constexpr int kStartWaitMs = 15000;
constexpr int kCallWaitMs = 10000;
constexpr int kWaitSlackMs = 5000;
constexpr int64_t kDefaultRequestMs = 10000;
constexpr int64_t kReadMaxBytes = 1 << 20;
constexpr int64_t kMaxConnections = 32;
constexpr char kLoopbackListen[] = "/ip4/127.0.0.1/tcp/0";
constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

struct Reply {
    bool ok = false;
    std::string error;
    json value;
    LibP2PCtx* created = nullptr;
};

using Promise = std::promise<Reply>;

Reply failure(std::string error) {
    Reply reply;
    reply.error = std::move(error);
    return reply;
}

Reply success(json value = nullptr) {
    Reply reply;
    reply.ok = true;
    reply.value = std::move(value);
    return reply;
}

void settle(void* user_data, Reply reply) {
    auto* promise = static_cast<Promise*>(user_data);
    promise->set_value(std::move(reply));
    delete promise;
}

Reply outcome(int code, const char* message) {
    Reply reply;
    reply.ok = code == NIMFFI_RET_OK;
    if (!reply.ok) reply.error = message ? message : "";
    return reply;
}

std::string text(const NimFfiStr& s) {
    return s.data ? std::string(s.data, s.len) : std::string();
}

json strings(const LibP2PSeq_Str& seq) {
    json out = json::array();
    for (size_t i = 0; seq.data && i < seq.len; ++i) out.push_back(text(seq.data[i]));
    return out;
}

std::string base64(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = i + 1 < len ? data[i + 1] : 0;
        const uint32_t c = i + 2 < len ? data[i + 2] : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;
        out.push_back(kAlphabet[(triple >> 18) & 63]);
        out.push_back(kAlphabet[(triple >> 12) & 63]);
        out.push_back(i + 1 < len ? kAlphabet[(triple >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? kAlphabet[triple & 63] : '=');
    }
    return out;
}

bool unbase64(const std::string& in, std::vector<uint8_t>& out) {
    uint32_t buffer = 0;
    int bits = 0;
    for (const char ch : in) {
        if (ch == '=') break;
        const char* at = std::strchr(kAlphabet, ch);
        if (!at || ch == '\0') return false;
        buffer = (buffer << 6) | static_cast<uint32_t>(at - kAlphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xff));
        }
    }
    return true;
}

std::string hex(const uint8_t* data, size_t len) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 15]);
    }
    return out;
}

int nibble(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool unhex(const std::string& in, std::vector<uint8_t>& out) {
    if (in.size() % 2 != 0) return false;
    for (size_t i = 0; i < in.size(); i += 2) {
        const int high = nibble(in[i]);
        const int low = nibble(in[i + 1]);
        if (high < 0 || low < 0) return false;
        out.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return true;
}

std::vector<NimFfiStr> borrowed(const std::vector<std::string>& in) {
    std::vector<NimFfiStr> out;
    out.reserve(in.size());
    for (const auto& s : in) out.push_back(nimffi_str(s.c_str()));
    return out;
}

NimFfiBytes borrowed(const std::vector<uint8_t>& in) {
    NimFfiBytes out;
    out.data = in.empty() ? nullptr : const_cast<uint8_t*>(in.data());
    out.len = in.size();
    return out;
}

void onDone(int code, const bool*, const char* message, void* user_data) {
    settle(user_data, outcome(code, message));
}

void onKey(int code, const NimFfiBytes* key, const char* message, void* user_data) {
    Reply reply = outcome(code, message);
    if (reply.ok) reply.value = key ? hex(key->data, key->len) : std::string();
    settle(user_data, std::move(reply));
}

void onCreated(int code, LibP2PCtx* ctx, const char* message, void* user_data) {
    Reply reply = outcome(code, message);
    if (reply.ok) reply.created = ctx;
    settle(user_data, std::move(reply));
}

void onPeers(int code, const PeersResponse* peers, const char* message, void* user_data) {
    Reply reply = outcome(code, message);
    if (reply.ok) reply.value = peers ? strings(peers->peerIds) : json::array();
    settle(user_data, std::move(reply));
}

void onPeerInfo(int code, const PeerInfoResponse* info, const char* message, void* user_data) {
    Reply reply = outcome(code, message);
    if (reply.ok && info) {
        reply.value = json{{"peerId", text(info->peerId)}, {"addrs", strings(info->addrs)}};
    }
    settle(user_data, std::move(reply));
}

void onDialed(int code, const DialResponse* dialed, const char* message, void* user_data) {
    Reply reply = outcome(code, message);
    if (reply.ok && dialed) reply.value = dialed->streamId;
    settle(user_data, std::move(reply));
}

void onRead(int code, const ReadResponse* read, const char* message, void* user_data) {
    Reply reply = outcome(code, message);
    if (reply.ok) reply.value = read ? base64(read->data.data, read->data.len) : std::string();
    settle(user_data, std::move(reply));
}

void onRecords(int code, const ExtendedRecordsResponse* found, const char* message,
               void* user_data) {
    Reply reply = outcome(code, message);
    if (reply.ok) {
        json records = json::array();
        for (size_t i = 0; found && found->records.data && i < found->records.len; ++i) {
            const ExtendedPeerRecordEntry& record = found->records.data[i];
            json services = json::array();
            for (size_t j = 0; record.services.data && j < record.services.len; ++j) {
                const ServiceInfoEntry& service = record.services.data[j];
                services.push_back(json{{"id", text(service.id)},
                                        {"data", base64(service.data.data, service.data.len)}});
            }
            records.push_back(json{{"peerId", text(record.peerId)},
                                   {"seqNo", record.seqNo},
                                   {"addrs", strings(record.addrs)},
                                   {"services", std::move(services)}});
        }
        reply.value = std::move(records);
    }
    settle(user_data, std::move(reply));
}

template <class Submit>
Reply await(const char* what, int64_t wait_ms, Submit&& submit) {
    auto* promise = new Promise();
    auto future = promise->get_future();
    const int submitted = submit(promise);
    if (submitted != 0) {
        if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) delete promise;
        return failure(std::string(what) + " (ret=" + std::to_string(submitted) + ")");
    }
    if (future.wait_for(std::chrono::milliseconds(wait_ms)) != std::future_status::ready) {
        return failure(std::string(what) + ": timeout");
    }
    Reply reply = future.get();
    if (!reply.ok) reply.error = std::string(what) + ": " + reply.error;
    return reply;
}

class OwnNode {
public:
    Reply create(const json& config) {
        std::lock_guard<std::mutex> creating(m_creating);
        if (context()) return failure("node already created");

        std::vector<std::string> peer_ids;
        std::vector<std::vector<std::string>> peer_addrs;
        for (const auto& peer : config.value("bootstrapNodes", json::array())) {
            if (!peer.is_object() || !peer.value("peerId", json()).is_string()) {
                return failure("bootstrapNodes entries need a peerId string");
            }
            std::vector<std::string> addrs;
            for (const auto& addr : peer.value("addrs", json::array())) {
                if (!addr.is_string()) return failure("bootstrapNodes addrs must be strings");
                addrs.push_back(addr.get<std::string>());
            }
            peer_ids.push_back(peer["peerId"].get<std::string>());
            peer_addrs.push_back(std::move(addrs));
        }
        std::vector<uint8_t> key;
        const json key_hex = config.value("privKey", json());
        if (key_hex.is_string() && !unhex(key_hex.get<std::string>(), key)) {
            return failure("privKey is not hex");
        }

        std::vector<std::vector<NimFfiStr>> addr_views;
        std::vector<BootstrapNode> nodes;
        addr_views.reserve(peer_ids.size());
        nodes.reserve(peer_ids.size());
        for (size_t i = 0; i < peer_ids.size(); ++i) {
            addr_views.push_back(borrowed(peer_addrs[i]));
            BootstrapNode node{};
            node.peerId = nimffi_str(peer_ids[i].c_str());
            node.multiaddrs = LibP2PSeq_Str{addr_views[i].data(), addr_views[i].size()};
            nodes.push_back(node);
        }

        NimFfiStr listen[] = {nimffi_str(kLoopbackListen)};

        Libp2pConfig cfg{};
        cfg.logLevel = LOG_LEVEL_WARN;
        cfg.addrs = LibP2PSeq_Str{listen, 1};
        cfg.mountKad = true;
        cfg.mountServiceDiscovery = true;
        cfg.serviceDiscoveryMode = SERVICE_DISCOVERY_MODE_CLIENT;
        cfg.serviceDiscoveryDisableXprPublishing = true;
        cfg.muxer = MUXER_TYPE_MPLEX;
        cfg.transport = TRANSPORT_TYPE_TCP;
        cfg.bootstrapNodes = LibP2PSeq_BootstrapNode{nodes.data(), nodes.size()};
        cfg.privKey = borrowed(key);
        cfg.maxConnections = kMaxConnections;
        cfg.maxConnsPerPeer = 1;
        cfg.natExplicitIp = nimffi_str("");

        Reply reply = await("libp2p_ctx_create failed", kCreateWaitMs, [&](Promise* p) {
            return libp2p_ctx_create(&cfg, &onCreated, p);
        });
        if (!reply.ok) return reply;
        if (!reply.created) return failure("libp2p_ctx_create failed: no context returned");

        std::unique_lock<std::shared_mutex> writing(m_ctx_lock);
        m_ctx = reply.created;
        return success();
    }

    Reply start() {
        return with_context("Failed to start libp2p", kStartWaitMs, [](LibP2PCtx* ctx, Promise* p) {
            return libp2p_ctx_start(ctx, &onDone, p);
        });
    }

    Reply destroy() {
        std::lock_guard<std::mutex> creating(m_creating);
        LibP2PCtx* doomed = nullptr;
        {
            std::unique_lock<std::shared_mutex> writing(m_ctx_lock);
            doomed = m_ctx;
            m_ctx = nullptr;
        }
        if (!doomed) return success();
        const int rc = libp2p_ctx_destroy(doomed);
        if (rc != NIMFFI_RET_OK) {
            return failure("libp2p_ctx_destroy failed (rc=" + std::to_string(rc) + ")");
        }
        return success();
    }

    Reply peer_info() {
        return with_context("Failed to get peer info", kCallWaitMs,
                            [](LibP2PCtx* ctx, Promise* p) {
                                return libp2p_ctx_peer_info(ctx, &onPeerInfo, p);
                            });
    }

    Reply connected_peers(int64_t direction) {
        if (direction != PEER_DIRECTION_INBOUND && direction != PEER_DIRECTION_OUTBOUND) {
            return failure("Failed to get connected peers: invalid direction: " +
                           std::to_string(direction));
        }
        const auto dir = static_cast<PeerDirection>(direction);
        return with_context("Failed to get connected peers", kCallWaitMs,
                            [dir](LibP2PCtx* ctx, Promise* p) {
                                return libp2p_ctx_connected_peers(ctx, dir, &onPeers, p);
                            });
    }

    Reply register_interest(const std::string& service_id) {
        return with_context("Failed to register interest", kCallWaitMs,
                            [&](LibP2PCtx* ctx, Promise* p) {
                                return libp2p_ctx_service_disco_register_interest(
                                    ctx, nimffi_str(service_id.c_str()), &onDone, p);
                            });
    }

    Reply lookup(const std::string& service_id, const std::string& service_data) {
        const std::vector<uint8_t> data(service_data.begin(), service_data.end());
        LookupRequest query{};
        query.serviceId = nimffi_str(service_id.c_str());
        query.serviceData = borrowed(data);
        Reply reply = with_context("Failed to lookup", kCallWaitMs,
                                   [&](LibP2PCtx* ctx, Promise* p) {
                                       return libp2p_ctx_service_disco_lookup(ctx, &query,
                                                                              &onRecords, p);
                                   });
        if (reply.ok && reply.value.is_null()) reply.value = json::array();
        return reply;
    }

    Reply request(const json& args) {
        if (!args.is_object()) return failure("protocolRequest: invalid JSON object arg");
        std::string peer_id;
        std::string proto;
        std::vector<std::string> multiaddrs;
        std::vector<uint8_t> body;
        int64_t timeout_ms = 0;
        try {
            peer_id = args.at("peerId").get<std::string>();
            proto = args.at("proto").get<std::string>();
            for (const auto& addr : args.value("multiaddrs", json::array())) {
                multiaddrs.push_back(addr.get<std::string>());
            }
            if (!unbase64(args.at("requestB64").get<std::string>(), body)) {
                return failure("protocolRequest: bad requestB64");
            }
            timeout_ms = args.value("timeoutMs", static_cast<int64_t>(0));
        } catch (const json::exception&) {
            return failure(
                "protocolRequest: bad args (need {peerId,proto,multiaddrs?,requestB64,timeoutMs?})");
        }
        if (timeout_ms <= 0) timeout_ms = kDefaultRequestMs;

        const std::vector<NimFfiStr> addr_views = borrowed(multiaddrs);
        DialRequest to_dial{};
        to_dial.peerId = nimffi_str(peer_id.c_str());
        to_dial.proto = nimffi_str(proto.c_str());
        to_dial.multiaddrs = LibP2PSeq_Str{const_cast<NimFfiStr*>(addr_views.data()),
                                           addr_views.size()};
        to_dial.forceDial = false;
        to_dial.timeoutMs = timeout_ms;
        const Reply dialed = with_context("Failed to dial", timeout_ms + kWaitSlackMs,
                                          [&](LibP2PCtx* ctx, Promise* p) {
                                              return libp2p_ctx_dial(ctx, &to_dial, &onDialed, p);
                                          });
        if (!dialed.ok) return failure("protocolRequest: dial failed: " + dialed.error);
        const uint64_t stream =
            dialed.value.is_number_unsigned() ? dialed.value.get<uint64_t>() : 0;
        if (stream == 0) return failure("protocolRequest: dial returned no stream");

        StreamWriteRequest to_write{};
        to_write.streamId = stream;
        to_write.data = borrowed(body);
        const Reply written = with_context("Failed to write LP to stream", kCallWaitMs,
                                           [&](LibP2PCtx* ctx, Promise* p) {
                                               return libp2p_ctx_stream_write_lp(ctx, &to_write,
                                                                                 &onDone, p);
                                           });
        if (!written.ok) {
            release(stream);
            return failure("protocolRequest: write failed: " + written.error);
        }

        StreamReadLpRequest to_read{};
        to_read.streamId = stream;
        to_read.maxSize = kReadMaxBytes;
        const Reply answered = with_context("Failed to read LP from stream",
                                            timeout_ms + kWaitSlackMs,
                                            [&](LibP2PCtx* ctx, Promise* p) {
                                                return libp2p_ctx_stream_read_lp(ctx, &to_read,
                                                                                 &onRead, p);
                                            });
        release(stream);
        if (!answered.ok) return failure("protocolRequest: read failed: " + answered.error);
        return success(json{{"responseB64", answered.value}});
    }

private:
    LibP2PCtx* context() {
        std::shared_lock<std::shared_mutex> reading(m_ctx_lock);
        return m_ctx;
    }

    template <class Submit>
    Reply with_context(const char* what, int64_t wait_ms, Submit&& submit) {
        return await(what, wait_ms, [&](Promise* p) {
            std::shared_lock<std::shared_mutex> reading(m_ctx_lock);
            if (!m_ctx) {
                settle(p, failure("No libp2p context"));
                return 0;
            }
            return submit(m_ctx, p);
        });
    }

    void release(uint64_t stream) {
        with_context("Failed to close stream", kCallWaitMs, [&](LibP2PCtx* ctx, Promise* p) {
            return libp2p_ctx_stream_close(ctx, stream, &onDone, p);
        });
        with_context("Failed to release stream", kCallWaitMs, [&](LibP2PCtx* ctx, Promise* p) {
            return libp2p_ctx_stream_release(ctx, stream, &onDone, p);
        });
    }

    std::mutex m_creating;
    std::shared_mutex m_ctx_lock;
    LibP2PCtx* m_ctx = nullptr;
};

Reply new_private_key() {
    NewPrivateKeyRequest wanted{};
    wanted.scheme = KEY_SCHEME_SECP256K1;
    Reply reply = await("Failed to generate private key", kCallWaitMs, [&](Promise* p) {
        return libp2p_static_new_private_key(&wanted, &onKey, p);
    });
    if (reply.ok && (!reply.value.is_string() || reply.value.get<std::string>().empty())) {
        return failure("Failed to generate private key: empty key");
    }
    return reply;
}

OwnNode& own_node() {
    static OwnNode node;
    return node;
}

std::string string_arg(const json& args, size_t index) {
    if (!args.is_array() || args.size() <= index || !args[index].is_string()) return {};
    return args[index].get<std::string>();
}

Reply dispatch(const std::string& method, const json& args) {
    if (method == "createNode") {
        json config = args.is_array() && !args.empty() ? args[0] : json::object();
        if (config.is_string()) config = json::parse(config.get<std::string>(), nullptr, false);
        if (!config.is_object()) return failure("invalid config: not a JSON object");
        return own_node().create(config);
    }
    if (method == "start") return own_node().start();
    if (method == "destroyNode") return own_node().destroy();
    if (method == "peerInfo") return own_node().peer_info();
    if (method == "connectedPeers") {
        const bool given = args.is_array() && !args.empty() && args[0].is_number_integer();
        return own_node().connected_peers(given ? args[0].get<int64_t>() : -1);
    }
    if (method == "discoRegisterInterest") {
        return own_node().register_interest(string_arg(args, 0));
    }
    if (method == "discoLookup") {
        return own_node().lookup(string_arg(args, 0), string_arg(args, 1));
    }
    if (method == "protocolRequest") {
        json blob = args.is_array() && !args.empty() ? args[0] : args;
        if (blob.is_string()) blob = json::parse(blob.get<std::string>(), nullptr, false);
        return own_node().request(blob);
    }
    if (method == "newPrivateKey") return new_private_key();
    return failure("unknown method");
}

}

extern "C" char* allocation_node_call(const char* method, const char* args_json) {
    json envelope;
    try {
        const std::string name = method ? method : "";
        const json args = json::parse(args_json ? args_json : "null", nullptr, false);
        Reply reply = args.is_discarded() ? failure("args are not JSON") : dispatch(name, args);
        envelope = json{{"success", reply.ok}, {"value", reply.value}, {"error", reply.error}};
    } catch (const std::exception& e) {
        envelope = json{{"success", false}, {"value", nullptr}, {"error", e.what()}};
    }
    const std::string out = envelope.dump();
    char* copy = static_cast<char*>(std::malloc(out.size() + 1));
    if (!copy) return nullptr;
    std::memcpy(copy, out.c_str(), out.size() + 1);
    return copy;
}

extern "C" void allocation_node_free(char* reply) {
    std::free(reply);
}
