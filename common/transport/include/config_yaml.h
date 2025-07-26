#pragma once
#include <yaml-cpp/yaml.h>
#include "network.h"
#include "util.h"

namespace YAML {
// struct Address {
//   socklen_t len;
//   union sockaddr_union su;
//   uint32_t ifindex;
// };
template<>
struct as_if<ngtcp2::Address, void> {
    const Node& node;
    as_if(const Node& n) : node(n) {}
    ngtcp2::Address operator()() const {
        string addr_str = node.as<string>();
        if (addr_str.empty()) {
            return ngtcp2::Address(); // Return default Address if empty
        }
        ngtcp2::Address addr;
        // Use the standard library function to parse the address string into the ngtcp2::Address structure
        if (inet_pton(AF_INET, addr_str.c_str(), &addr.su.in) == 1) {
            addr.len = sizeof(sockaddr_in);
        } else if (inet_pton(AF_INET6, addr_str.c_str(), &addr.su.in6) == 1) {
            addr.len = sizeof(sockaddr_in6);
        } else {
            throw std::runtime_error("Invalid address format: " + addr_str);
        }
        return addr;
    }
};

// ngtcp2_cid
template<>
struct as_if<ngtcp2_cid, void> {
    const Node& node;
    as_if(const Node& n) : node(n) {}
    ngtcp2_cid operator()() const {
        string cid_str = node.as<string>();
        if (cid_str.empty()) {
            return ngtcp2_cid(); // Return default CID if empty
        }
        // Convert the CID string as a hexadecimal string
        string uint8_t_cid;
        for (size_t i = 0; i < cid_str.size(); i += 2) {
            string byte_str = cid_str.substr(i, 2);
            uint8_t byte = static_cast<uint8_t>(strtol(byte_str.c_str(), nullptr, 16));
            uint8_t_cid.push_back(byte);
        }
        // Use the standard ngtcp2 library function to parse the CID string into the ngtcp2_cid structure
        return ngtcp2::util::make_cid_key(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(uint8_t_cid.data()), uint8_t_cid.size()));
    }
};

}