// SPDX-License-Identifier: Apache-2.0
//
// Discovery response validation. Upstream clients accept only the current
// STYLY-NETSYNC3 format; older replies exist on some networks and must be
// rejected so the client keeps scanning rather than connecting to a server it
// cannot talk to.

#include <string>

#include "../test_support.hpp"
#include "transport/server_discovery.hpp"

using namespace styly::netsync;
using namespace styly::netsync::test;

namespace {

void test_wire_constants() {
    CHECK_EQ(std::string(kDiscoveryRequest), std::string("STYLY-NETSYNC-DISCOVER"));
    CHECK_EQ(std::string(kDiscoveryResponseVersion), std::string("STYLY-NETSYNC3"));
}

void test_accepts_current_format() {
    DiscoveredServer server;
    CHECK(parse_discovery_response("STYLY-NETSYNC3|5555|5557|5556|8000|my-server", "192.168.1.5",
                                   server));
    CHECK_EQ(server.control_port, 5555);
    CHECK_EQ(server.transform_port, 5557);
    CHECK_EQ(server.sub_port, 5556);
    CHECK_EQ(server.rest_api_port, 8000);
    CHECK_EQ(server.server_name, std::string("my-server"));
    CHECK_EQ(server.ip, std::string("192.168.1.5"));
    CHECK_EQ(server.address, std::string("tcp://192.168.1.5"));
}

void test_accepts_tcp_trailing_newline() {
    // The TCP responder appends "\n"; the UDP one does not.
    DiscoveredServer server;
    CHECK(parse_discovery_response("STYLY-NETSYNC3|5555|5557|5556|8000|srv\n", "10.0.0.1", server));
    CHECK_EQ(server.server_name, std::string("srv"));

    CHECK(parse_discovery_response("STYLY-NETSYNC3|5555|5557|5556|8000|srv\r\n", "10.0.0.1",
                                   server));
    CHECK_EQ(server.server_name, std::string("srv"));
}

void test_rejects_legacy_versions() {
    DiscoveredServer server;
    // STYLY-NETSYNC2 carried one fewer port; the server still recognises it for
    // conflict warnings, but a client must not connect to it.
    CHECK(!parse_discovery_response("STYLY-NETSYNC2|5555|5556|8000|old-server", "10.0.0.2",
                                    server));
    CHECK(!parse_discovery_response("STYLY-NETSYNC|5555|5556|old-server", "10.0.0.2", server));
    // Even a v2 reply padded to six fields must fail on the version token.
    CHECK(!parse_discovery_response("STYLY-NETSYNC2|5555|5557|5556|8000|old", "10.0.0.2", server));
}

void test_rejects_malformed_responses() {
    DiscoveredServer server;
    CHECK(!parse_discovery_response("", "10.0.0.3", server));
    CHECK(!parse_discovery_response("garbage", "10.0.0.3", server));
    // Too few fields.
    CHECK(!parse_discovery_response("STYLY-NETSYNC3|5555|5557|5556|8000", "10.0.0.3", server));
    // Non-numeric ports.
    CHECK(!parse_discovery_response("STYLY-NETSYNC3|abc|5557|5556|8000|s", "10.0.0.3", server));
    CHECK(!parse_discovery_response("STYLY-NETSYNC3|5555||5556|8000|s", "10.0.0.3", server));
    CHECK(!parse_discovery_response("STYLY-NETSYNC3|5555|5557|5556|-1|s", "10.0.0.3", server));
    // Port out of range.
    CHECK(!parse_discovery_response("STYLY-NETSYNC3|70000|5557|5556|8000|s", "10.0.0.3", server));
    // Version token that merely starts with the right text.
    CHECK(!parse_discovery_response("STYLY-NETSYNC30|5555|5557|5556|8000|s", "10.0.0.3", server));
    // The discovery *request* is not a response.
    CHECK(!parse_discovery_response(kDiscoveryRequest, "10.0.0.3", server));
}

void test_server_name_may_contain_separators() {
    // The name is the remainder of the line, so a '|' inside it survives.
    DiscoveredServer server;
    CHECK(parse_discovery_response("STYLY-NETSYNC3|5555|5557|5556|8000|desk|top|pc", "10.0.0.4",
                                   server));
    CHECK_EQ(server.server_name, std::string("desk|top|pc"));
}

void test_empty_server_name_is_accepted() {
    DiscoveredServer server;
    CHECK(parse_discovery_response("STYLY-NETSYNC3|5555|5557|5556|8000|", "10.0.0.5", server));
    CHECK_EQ(server.server_name, std::string(""));
    CHECK_EQ(server.control_port, 5555);
}

void test_interface_enumeration_is_well_formed() {
    // Cannot assert which interfaces exist on a CI runner, only that whatever
    // is reported is self-consistent.
    for (const LocalInterface &item : enumerate_local_interfaces()) {
        CHECK(!item.address.empty());
        CHECK(!item.broadcast.empty());
        CHECK(item.address != std::string("127.0.0.1"));
    }
}

}  // namespace

int main() {
    test_wire_constants();
    test_accepts_current_format();
    test_accepts_tcp_trailing_newline();
    test_rejects_legacy_versions();
    test_rejects_malformed_responses();
    test_server_name_may_contain_separators();
    test_empty_server_name_is_accepted();
    test_interface_enumeration_is_well_formed();
    return summary("server discovery parsing");
}
