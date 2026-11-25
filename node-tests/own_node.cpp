#include <logos_test.h>
#include <plugin.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "own_node.h"

using json = nlohmann::json;

namespace {

constexpr char kService[] = "allocation-own-node-test";
constexpr char kProto[] = "/allocation-own-node-test/1.0.0";

json call(const char* method, const json& args) {
    char* raw = allocation_node_call(method, args.dump().c_str());
    json reply = raw ? json::parse(raw, nullptr, false) : json();
    allocation_node_free(raw);
    if (!reply.is_object() || !reply.value("success", false)) {
        std::fprintf(stderr, "%s -> %s\n", method, reply.dump().c_str());
    }
    return reply;
}

struct Peer {
    std::string id;
    std::vector<std::string> addrs;
};

Peer identity(Libp2pModuleImpl& node) {
    const json info = node.peerInfo().value;
    Peer peer;
    peer.id = info["peerId"].get<std::string>();
    for (const auto& addr : info["addrs"]) peer.addrs.push_back(addr.get<std::string>());
    return peer;
}

json node_config(const Peer& bootstrap, const std::string& key_hex) {
    json config{{"bootstrapNodes",
                 json::array({json{{"peerId", bootstrap.id}, {"addrs", bootstrap.addrs}}})}};
    if (!key_hex.empty()) config["privKey"] = key_hex;
    return json::array({config.dump()});
}

bool joined() {
    for (int i = 0; i < 20; ++i) {
        const json peers = call("connectedPeers", json::array({1}));
        if (peers.value("success", false) && !peers["value"].empty()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

bool connected(Libp2pModuleImpl& node, const std::string& peer) {
    for (const int direction : {0, 1}) {
        for (const auto& seen : node.connectedPeers(direction).value) {
            if (seen.get<std::string>() == peer) return true;
        }
    }
    return false;
}

bool gone_from(Libp2pModuleImpl& node, const std::string& peer) {
    for (int i = 0; i < 20; ++i) {
        if (!connected(node, peer)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

json found(const std::string& advertiser) {
    for (int i = 0; i < 10; ++i) {
        const json reply = call("discoLookup", json::array({kService, ""}));
        if (reply.value("success", false)) {
            for (const auto& record : reply["value"]) {
                if (record.value("peerId", std::string()) == advertiser) return record;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    return json();
}

}

LOGOS_TEST(a_loopback_only_client_node_finds_an_advertiser_and_gets_its_reply) {
    Libp2pModuleImpl registrar(Libp2pModuleOptions{.mountServiceDiscovery = true});
    LOGOS_ASSERT_TRUE(registrar.start().success);
    LOGOS_ASSERT_TRUE(registrar.discoStart().success);
    const Peer at_registrar = identity(registrar);

    Libp2pModuleOptions options{.mountServiceDiscovery = true};
    options.bootstrapNodes = {{at_registrar.id, at_registrar.addrs}};
    Libp2pModuleImpl advertiser(options);
    LOGOS_ASSERT_TRUE(advertiser.start().success);
    LOGOS_ASSERT_TRUE(advertiser.discoStart().success);
    LOGOS_ASSERT_TRUE(advertiser.connectPeer(at_registrar.id, at_registrar.addrs, 5000).success);
    LOGOS_ASSERT_TRUE(advertiser.mountProtocol(kProto).success);
    LOGOS_ASSERT_TRUE(advertiser.discoStartAdvertising(kService, "", "").success);
    const Peer at_advertiser = identity(advertiser);

    const std::string key_hex = registrar.newPrivateKey("secp256k1").value.get<std::string>();
    LOGOS_ASSERT_FALSE(key_hex.empty());

    LOGOS_ASSERT_TRUE(call("createNode", node_config(at_registrar, key_hex)).value("success", false));
    const json again = call("createNode", node_config(at_registrar, key_hex));
    LOGOS_ASSERT_FALSE(again.value("success", true));
    LOGOS_ASSERT_TRUE(again.value("error", std::string()).find("already created") != std::string::npos);
    LOGOS_ASSERT_TRUE(call("start", json::array()).value("success", false));

    const json own = call("peerInfo", json::array());
    LOGOS_ASSERT_TRUE(own.value("success", false));
    const std::string own_id = own["value"]["peerId"].get<std::string>();
    LOGOS_ASSERT_FALSE(own["value"]["addrs"].empty());
    for (const auto& addr : own["value"]["addrs"]) {
        LOGOS_ASSERT_TRUE(addr.get<std::string>().rfind("/ip4/127.0.0.1/tcp/", 0) == 0);
    }

    LOGOS_ASSERT_TRUE(joined());
    LOGOS_ASSERT_TRUE(call("discoRegisterInterest", json::array({kService})).value("success", false));
    const json record = found(at_advertiser.id);
    LOGOS_ASSERT_TRUE(record.is_object());

    std::string asked_by;
    std::string asked;
    std::thread serving([&] {
        const auto accepted =
            advertiser.protocolAcceptStream(json{{"proto", kProto}, {"timeoutMs", 20000}}.dump());
        if (!accepted.success) return;
        const json stream = accepted.value["streamId"];
        asked_by = accepted.value["peerId"].get<std::string>();
        const auto read =
            advertiser.streamReadLpJson(json{{"streamId", stream}, {"timeoutMs", 10000}}.dump());
        if (read.success) asked = base64Decode(read.value["dataB64"].get<std::string>());
        const std::string answer = "granted:" + asked;
        advertiser.streamWriteLpJson(
            json{{"streamId", stream},
                 {"dataB64", base64Encode(std::vector<uint8_t>(answer.begin(), answer.end()))}}
                .dump());
        advertiser.streamCloseJson(json{{"streamId", stream}}.dump());
        advertiser.streamReleaseJson(json{{"streamId", stream}}.dump());
    });

    const std::string question = "membership please";
    const json answered = call(
        "protocolRequest",
        json{{"peerId", at_advertiser.id},
             {"multiaddrs", record["addrs"]},
             {"proto", kProto},
             {"requestB64",
              base64Encode(std::vector<uint8_t>(question.begin(), question.end()))},
             {"timeoutMs", 10000}});
    serving.join();
    LOGOS_ASSERT_TRUE(answered.value("success", false));
    LOGOS_ASSERT_TRUE(base64Decode(answered["value"]["responseB64"].get<std::string>())
                      == "granted:" + question);
    LOGOS_ASSERT_TRUE(asked == question);
    LOGOS_ASSERT_TRUE(asked_by == own_id);

    LOGOS_ASSERT_TRUE(connected(registrar, own_id));
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const auto at_peerstore = registrar.peerstoreGetPeerInfo(own_id);
    std::fprintf(stderr, "own node as the registrar stores it: success=%d %s %s\n",
                 at_peerstore.success, at_peerstore.value.dump().c_str(),
                 at_peerstore.error.c_str());
    const auto random_records = registrar.discoRandomLookup();
    for (const auto& seen : random_records.value) {
        LOGOS_ASSERT_FALSE(seen.value("peerId", std::string()) == own_id);
    }
    const auto nearest = advertiser.kadFindNode(own_id);
    for (const auto& seen : nearest.value) {
        LOGOS_ASSERT_FALSE(seen.get<std::string>() == own_id);
    }

    const auto before = std::chrono::steady_clock::now();
    LOGOS_ASSERT_TRUE(call("destroyNode", json::array()).value("success", false));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - before);
    std::fprintf(stderr, "own node torn down in %lld ms\n", static_cast<long long>(took.count()));
    LOGOS_ASSERT_FALSE(call("connectedPeers", json::array({1})).value("success", true));
    LOGOS_ASSERT_TRUE(gone_from(registrar, own_id));
    LOGOS_ASSERT_TRUE(gone_from(advertiser, own_id));

    LOGOS_ASSERT_TRUE(call("createNode", node_config(at_registrar, key_hex)).value("success", false));
    LOGOS_ASSERT_TRUE(call("start", json::array()).value("success", false));
    const json reborn = call("peerInfo", json::array());
    LOGOS_ASSERT_TRUE(reborn["value"]["peerId"].get<std::string>() == own_id);
    LOGOS_ASSERT_TRUE(call("destroyNode", json::array()).value("success", false));

    LOGOS_ASSERT_TRUE(advertiser.stop().success);
    LOGOS_ASSERT_TRUE(registrar.stop().success);
}
