/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/nl/tests/FakeNetlinkProtocolSocket.h"

namespace openr::fbnl {

namespace utils {

fbnl::Link
createLink(
    const int ifIndex, const std::string& ifName, bool isUp, bool isLoopback) {
  fbnl::LinkBuilder builder;
  builder.setIfIndex(ifIndex);
  builder.setLinkName(ifName);
  if (isUp) {
    builder.setFlags(IFF_RUNNING);
  }
  if (isLoopback) {
    builder.setFlags(IFF_LOOPBACK);
  }
  return builder.build();
}

fbnl::IfAddress
createIfAddress(const int ifIndex, const std::string& addrMask) {
  const auto network = folly::IPAddress::createNetwork(addrMask, -1, false);
  fbnl::IfAddressBuilder builder;
  builder.setIfIndex(ifIndex);
  builder.setPrefix(network);
  if (network.first.isLoopback()) {
    builder.setScope(RT_SCOPE_HOST);
  } else if (network.first.isLinkLocal()) {
    builder.setScope(RT_SCOPE_LINK);
  } else {
    builder.setScope(RT_SCOPE_UNIVERSE);
  }
  return builder.build();
}

} // namespace utils

folly::SemiFuture<int>
FakeNetlinkProtocolSocket::addRoute(const fbnl::Route& route) {
  // Blindly replace existing route
  const auto proto = route.getProtocolId();
  if (route.getFamily() == AF_MPLS) {
    mplsRoutes_[proto][route.getMplsLabel().value()] = route;
  } else {
    unicastRoutes_[proto][route.getDestination()] = route;
  }
  return folly::SemiFuture<int>(0);
}

folly::SemiFuture<int>
FakeNetlinkProtocolSocket::deleteRoute(const fbnl::Route& route) {
  // Count number of elements erased
  int cnt{0};
  const auto proto = route.getProtocolId();
  if (route.getFamily() == AF_MPLS) {
    cnt = mplsRoutes_[proto].erase(route.getMplsLabel().value());
  } else {
    cnt = unicastRoutes_[proto].erase(route.getDestination());
  }
  // Return 0 on success else ESRCH (no such process) error code
  return folly::SemiFuture<int>(cnt ? 0 : ESRCH);
}

folly::SemiFuture<std::vector<fbnl::Route>>
FakeNetlinkProtocolSocket::getRoutes(const fbnl::Route& filter) {
  const auto filterFamily = filter.getFamily();
  const auto filterProto = filter.getProtocolId();
  const auto filterType = filter.getType();

  std::vector<fbnl::Route> result;
  auto applyFilter = [&](const fbnl::Route& route) {
    // Filter on protocol
    if (filterProto && filterProto != route.getProtocolId()) {
      return;
    }

    // Filter on AFI
    if (filterFamily && filterFamily != route.getFamily()) {
      return;
    }

    // Filter on type
    if (filterType && filterType != route.getType()) {
      return;
    }

    result.emplace_back(route);
  };

  // Loop through mpls routes
  for (auto& [protoId, routes] : mplsRoutes_) {
    for (auto& [_, route] : routes) {
      applyFilter(route);
    }
  }

  // Loop through unicast routes
  for (auto& [protoId, routes] : unicastRoutes_) {
    for (auto& [_, route] : routes) {
      applyFilter(route);
    }
  }

  return result;
}

folly::SemiFuture<int>
FakeNetlinkProtocolSocket::addIfAddress(const fbnl::IfAddress& addr) {
  // Search for addr list of interface index (it must exists)
  auto it = ifAddrs_.find(addr.getIfIndex());
  if (it == ifAddrs_.end() or !addr.getPrefix().has_value()) {
    return folly::SemiFuture<int>(-ENXIO); // No such device or address
  }

  // Find if existing. Return EEXIST
  for (auto addrIt = it->second.begin(); addrIt != it->second.end(); ++addrIt) {
    if (addrIt->getPrefix() == addr.getPrefix()) {
      return folly::SemiFuture<int>(-EEXIST);
    }
  }

  // Non existing address. Add
  it->second.emplace_back(addr); // Add
  return folly::SemiFuture<int>(0);
}

folly::SemiFuture<int>
FakeNetlinkProtocolSocket::deleteIfAddress(const fbnl::IfAddress& addr) {
  // Search for addr list of interface index (it must exists)
  auto it = ifAddrs_.find(addr.getIfIndex());
  if (it == ifAddrs_.end() or !addr.getPrefix().has_value()) {
    return folly::SemiFuture<int>(-ENXIO); // No such device or address
  }

  // Find & delete
  for (auto addrIt = it->second.begin(); addrIt != it->second.end(); ++addrIt) {
    if (addrIt->getPrefix() == addr.getPrefix()) {
      it->second.erase(addrIt);
      return folly::SemiFuture<int>(0);
    }
  }

  // Address not available for deletion
  return folly::SemiFuture<int>(-EADDRNOTAVAIL);
}

folly::SemiFuture<std::vector<fbnl::IfAddress>>
FakeNetlinkProtocolSocket::getAllIfAddresses() {
  std::vector<fbnl::IfAddress> addrs;
  for (auto& [_, addrs_] : ifAddrs_) {
    addrs.insert(addrs.end(), addrs_.begin(), addrs_.end());
  }
  return folly::SemiFuture<std::vector<fbnl::IfAddress>>(std::move(addrs));
}

folly::SemiFuture<int>
FakeNetlinkProtocolSocket::addLink(const fbnl::Link& link) {
  // Check if link exists already
  if (links_.count(link.getIfIndex())) {
    return folly::SemiFuture<int>(-EEXIST);
  }

  links_.emplace(link.getIfIndex(), link);
  ifAddrs_.emplace(link.getIfIndex(), std::list<fbnl::IfAddress>());
  return folly::SemiFuture<int>(0);
}

folly::SemiFuture<std::vector<fbnl::Link>>
FakeNetlinkProtocolSocket::getAllLinks() {
  std::vector<fbnl::Link> links;
  for (auto& [_, link] : links_) {
    links.emplace_back(link);
  }
  return folly::SemiFuture<std::vector<fbnl::Link>>(std::move(links));
}

folly::SemiFuture<std::vector<fbnl::Neighbor>>
FakeNetlinkProtocolSocket::getAllNeighbors() {
  CHECK(false) << "Not implemented";
}

} // namespace openr::fbnl
