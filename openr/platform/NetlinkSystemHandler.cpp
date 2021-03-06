/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NetlinkSystemHandler.h"

#include <algorithm>
#include <functional>
#include <thread>
#include <utility>

#include <folly/Format.h>
#include <folly/MapUtil.h>
#include <folly/futures/Promise.h>
#include <folly/gen/Base.h>
#include <folly/system/ThreadName.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/NetworkUtil.h>

using apache::thrift::FRAGILE;

const std::chrono::seconds kNetlinkDbResyncInterval{20};

namespace openr {

NetlinkSystemHandler::NetlinkSystemHandler(fbnl::NetlinkProtocolSocket* nlSock)
    : nlSock_(nlSock) {
  CHECK(nlSock);
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::Link>>>
NetlinkSystemHandler::semifuture_getAllLinks() {
  VLOG(3) << "Query links from Netlink according to link name";

  return collectAll(nlSock_->getAllLinks(), nlSock_->getAllIfAddresses())
      .deferValue([](std::tuple<
                      folly::Try<std::vector<fbnl::Link>>,
                      folly::Try<std::vector<fbnl::IfAddress>>>&& res) {
        std::unordered_map<int, thrift::Link> links;
        // Create links
        for (auto& nlLink : std::get<0>(res).value()) {
          thrift::Link link;
          link.ifName = nlLink.getLinkName();
          link.ifIndex = nlLink.getIfIndex();
          link.isUp = nlLink.isUp();
          links.emplace(nlLink.getIfIndex(), std::move(link));
        }

        // Add addresses
        for (auto& nlAddr : std::get<1>(res).value()) {
          auto& link = links.at(nlAddr.getIfIndex());
          link.networks.emplace_back(toIpPrefix(nlAddr.getPrefix().value()));
        }

        // Convert to list and return
        auto result = std::make_unique<std::vector<thrift::Link>>();
        for (auto& kv : links) {
          result->emplace_back(std::move(kv.second));
        }
        return result;
      });
}

folly::SemiFuture<folly::Unit>
NetlinkSystemHandler::semifuture_addIfaceAddresses(
    std::unique_ptr<std::string> ifName,
    std::unique_ptr<std::vector<::openr::thrift::IpPrefix>> addrs) {
  VLOG(3) << "Add iface addresses";
  return addRemoveIfAddresses(true, *ifName, *addrs);
}

folly::SemiFuture<folly::Unit>
NetlinkSystemHandler::semifuture_removeIfaceAddresses(
    std::unique_ptr<std::string> ifName,
    std::unique_ptr<std::vector<::openr::thrift::IpPrefix>> addrs) {
  VLOG(3) << "Remove iface addresses";
  return addRemoveIfAddresses(false, *ifName, *addrs);
}

folly::SemiFuture<folly::Unit>
NetlinkSystemHandler::addRemoveIfAddresses(
    const bool isAdd,
    const std::string& ifName,
    const std::vector<thrift::IpPrefix>& addrs) {
  // Get iface index
  const int ifIndex = getIfIndex(ifName).value();

  // Add netlink requests
  std::vector<folly::SemiFuture<int>> futures;
  for (const auto& addr : addrs) {
    fbnl::IfAddressBuilder builder;
    auto const network = toIPNetwork(addr, false /* applyMask */);
    builder.setPrefix(network);
    builder.setIfIndex(ifIndex);
    if (network.first.isLoopback()) {
      builder.setScope(RT_SCOPE_HOST);
    } else if (network.first.isLinkLocal()) {
      builder.setScope(RT_SCOPE_LINK);
    } else {
      builder.setScope(RT_SCOPE_UNIVERSE);
    }
    if (isAdd) {
      futures.emplace_back(nlSock_->addIfAddress(builder.build()));
    } else {
      futures.emplace_back(nlSock_->deleteIfAddress(builder.build()));
    }
  }

  // Accumulate futures into a single one
  return collectAll(std::move(futures))
      .deferValue([](std::vector<folly::Try<int>>&& retvals) {
        for (auto& retval : retvals) {
          const int ret = std::abs(retval.value());
          if (ret != 0 && ret != EEXIST && ret != EADDRNOTAVAIL) {
            throw fbnl::NlException("Address add/remove failed.", ret);
          }
        }
        return folly::Unit();
      });
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::IpPrefix>>>
NetlinkSystemHandler::semifuture_getIfaceAddresses(
    std::unique_ptr<std::string> ifName, int16_t family, int16_t scope) {
  VLOG(3) << "Get iface addresses";

  // Get iface index
  const int ifIndex = getIfIndex(*ifName).value();

  return nlSock_->getAllIfAddresses().deferValue(
      [ifIndex, family, scope](std::vector<fbnl::IfAddress>&& nlAddrs) {
        auto addrs = std::make_unique<std::vector<thrift::IpPrefix>>();
        for (auto& nlAddr : nlAddrs) {
          if (nlAddr.getIfIndex() != ifIndex) {
            continue;
          }
          // Apply filter on family if specified
          if (family && nlAddr.getFamily() != family) {
            continue;
          }
          // Apply filter on scope. Must always be specified
          if (nlAddr.getScope() != scope) {
            continue;
          }
          addrs->emplace_back(toIpPrefix(nlAddr.getPrefix().value()));
        }
        return addrs;
      });
}

folly::SemiFuture<folly::Unit>
NetlinkSystemHandler::semifuture_syncIfaceAddresses(
    std::unique_ptr<std::string> iface,
    int16_t family,
    int16_t scope,
    std::unique_ptr<std::vector<::openr::thrift::IpPrefix>> newAddrs) {
  VLOG(3) << "Sync iface addresses";

  const auto ifName = *iface; // Copy intended
  const auto ifIndex = getIfIndex(ifName).value();

  auto oldAddrs =
      semifuture_getIfaceAddresses(std::move(iface), family, scope).get();
  std::vector<folly::SemiFuture<int>> futures;

  // Add new addresses
  for (auto& newAddr : *newAddrs) {
    // Skip adding existing addresse
    if (std::find(oldAddrs->begin(), oldAddrs->end(), newAddr) !=
        oldAddrs->end()) {
      continue;
    }
    // Add non-existing new address
    fbnl::IfAddressBuilder builder;
    builder.setPrefix(toIPNetwork(newAddr, false /* applyMask */));
    builder.setIfIndex(ifIndex);
    builder.setScope(scope);
    futures.emplace_back(nlSock_->addIfAddress(builder.build()));
  }

  // Delete old addresses
  for (auto& oldAddr : *oldAddrs) {
    // Skip removing new addresse
    if (std::find(newAddrs->begin(), newAddrs->end(), oldAddr) !=
        newAddrs->end()) {
      continue;
    }
    // Remove non-existing old address
    fbnl::IfAddressBuilder builder;
    builder.setPrefix(toIPNetwork(oldAddr, false /* applyMask */));
    builder.setIfIndex(ifIndex);
    builder.setScope(scope);
    futures.emplace_back(nlSock_->deleteIfAddress(builder.build()));
  }

  // Collect all futures
  return collectAll(std::move(futures))
      .deferValue([](std::vector<folly::Try<int>>&& retvals) {
        for (auto& retval : retvals) {
          const int ret = std::abs(retval.value());
          if (ret != 0 && ret != EEXIST && ret != EADDRNOTAVAIL) {
            throw fbnl::NlException("Address add/remove failed.", ret);
          }
        }
        return folly::Unit();
      });
}

std::optional<int>
NetlinkSystemHandler::getIfIndex(const std::string& ifName) {
  auto links = nlSock_->getAllLinks().get();
  for (auto& link : links) {
    if (link.getLinkName() == ifName) {
      return link.getIfIndex();
    }
  }
  return std::nullopt;
}

} // namespace openr
