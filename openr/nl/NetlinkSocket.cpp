/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/nl/NetlinkSocket.h>

namespace {
const folly::StringPiece kRouteObjectStr("route/route");
const folly::StringPiece kLinkObjectStr("route/link");
const folly::StringPiece kAddrObjectStr("route/addr");
const folly::StringPiece kNeighborObjectStr("route/neigh");

// Socket buffer size for netlink sockets we create
// We use 2MB, default is 32KB
const size_t kNlSockRecvBuf{2 * 1024 * 1024};

} // anonymous namespace

namespace openr {
namespace fbnl {

NetlinkSocket::NetlinkSocket(
    fbzmq::ZmqEventLoop* evl, EventsHandler* handler, bool useNetlinkMessage)
    : evl_(evl), handler_(handler), useNetlinkMessage_(useNetlinkMessage) {
  CHECK(evl_ != nullptr) << "Missing event loop.";

  // Create netlink socket for only notification subscription
  subSock_ = nl_socket_alloc();
  CHECK(subSock_ != nullptr) << "Failed to create netlink socket.";

  // Create netlink socket for periodic refresh of our caches
  reqSock_ = nl_socket_alloc();
  CHECK(reqSock_ != nullptr) << "Failed to create netlink socket.";

  int err = nl_connect(reqSock_, NETLINK_ROUTE);
  CHECK_EQ(err, 0) << "Failed to connect nl socket. Error " << nl_geterror(err);

  // Create cache manager using notification socket
  err = nl_cache_mngr_alloc(
      subSock_, NETLINK_ROUTE, NL_AUTO_PROVIDE, &cacheManager_);
  CHECK_EQ(err, 0) << "Failed to create cache manager. Error: "
                   << nl_geterror(err);

  // Set high buffers on netlink socket (especially on sub socket) so that
  // bulk events can also be received
  err = nl_socket_set_buffer_size(reqSock_, kNlSockRecvBuf, 0);
  CHECK_EQ(err, 0) << "Failed to set socket buffer on reqSock_";
  err = nl_socket_set_buffer_size(subSock_, kNlSockRecvBuf, 0);
  CHECK_EQ(err, 0) << "Failed to set socket buffer on subSock_";

  // create netlink protocol object
  if (useNetlinkMessage_) {
    auto tid = static_cast<int>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    nlSock_ =
        std::make_unique<openr::Netlink::NetlinkProtocolSocket>(evl_, tid);
    CHECK(nlSock_ != nullptr) << "Missing event loop.";
    nlSock_->init();
  }

  // Request a route cache to be created and registered with cache manager
  // route event handler is provided which has this object as opaque data so
  // we can get object state back in this static callback
  err = nl_cache_mngr_add(
      cacheManager_, kRouteObjectStr.data(), routeCacheCB, this, &routeCache_);
  if (err != 0 || !routeCache_) {
    CHECK(false) << "Failed to add neighbor cache to manager. Error: "
                 << nl_geterror(err);
  }

  // Add link cache
  err = nl_cache_mngr_add(
      cacheManager_, kLinkObjectStr.data(), linkCacheCB, this, &linkCache_);
  if (err != 0 || !linkCache_) {
    CHECK(false) << "Failed to add link cache to manager. Error: "
                 << nl_geterror(err);
  }

  // Add address cache
  err = nl_cache_mngr_add(
      cacheManager_, kAddrObjectStr.data(), addrCacheCB, this, &addrCache_);
  if (err != 0 || !addrCache_) {
    CHECK(false) << "Failed to add addr cache to manager. Error: "
                 << nl_geterror(err);
  }

  err = nl_cache_mngr_add(
      cacheManager_,
      kNeighborObjectStr.data(),
      neighCacheCB,
      this,
      &neighborCache_);
  if (err != 0 || !neighborCache_) {
    CHECK(false) << "Failed to add neighbor cache to manager. Error: "
                 << nl_geterror(err);
  }

  // Get socket FD to monitor for updates
  int socketFd = nl_cache_mngr_get_fd(cacheManager_);
  CHECK_NE(socketFd, -1) << "Failed to get socket fd";

  // Anytime this socket has data, have libnl process it
  // Our registered handlers will be invoked..
  evl_->addSocketFd(socketFd, POLLIN, [this](int) noexcept {
    int lambdaErr = nl_cache_mngr_data_ready(cacheManager_);
    if (lambdaErr < 0) {
      LOG(ERROR) << "Error processing data on netlink socket. Error: "
                 << nl_geterror(lambdaErr);
    } else {
      VLOG(2) << "Processed " << lambdaErr << " netlink messages.";
    }
  });

  // need to reload routes from kernel to avoid re-adding existing route
  // type of exception in NetlinkSocket
  updateRouteCache();
}

NetlinkSocket::~NetlinkSocket() {
  VLOG(2) << "NetlinkSocket destroy cache";

  evl_->removeSocketFd(nl_cache_mngr_get_fd(cacheManager_));

  // Manager will release our caches internally
  nl_cache_mngr_free(cacheManager_);
  nl_socket_free(subSock_);
  nl_close(reqSock_);
  nl_socket_free(reqSock_);

  routeCache_ = nullptr;
  linkCache_ = nullptr;
  cacheManager_ = nullptr;
  neighborCache_ = nullptr;
  subSock_ = nullptr;
  reqSock_ = nullptr;
}

void
NetlinkSocket::routeCacheCB(
    struct nl_cache*, struct nl_object* obj, int action, void* data) noexcept {
  CHECK(data) << "Opaque context does not exist in route callback";
  reinterpret_cast<NetlinkSocket*>(data)->handleRouteEvent(
      obj, action, true, false);
}

void
NetlinkSocket::handleRouteEvent(
    struct nl_object* obj,
    int action,
    bool runHandler,
    bool updateUnicastRoute) noexcept {
  CHECK_NOTNULL(obj);
  if (!checkObjectType(obj, kRouteObjectStr)) {
    return;
  }

  struct rtnl_route* routeObj = reinterpret_cast<struct rtnl_route*>(obj);
  try {
    doUpdateRouteCache(routeObj, action, updateUnicastRoute);
  } catch (const std::exception& ex) {
    LOG(ERROR) << "UpdateCacheFailed";
  }

  if (handler_ && runHandler && eventFlags_[ROUTE_EVENT]) {
    const bool isValid = (action != NL_ACT_DEL);
    RouteBuilder builder;
    auto route = builder.loadFromObject(routeObj).setValid(isValid).build();
    std::string ifName =
        route.getRouteIfName().hasValue() ? route.getRouteIfName().value() : "";
    EventVariant event = std::move(route);
    handler_->handleEvent(ifName, action, event);
  }
}

void
NetlinkSocket::linkCacheCB(
    struct nl_cache*, struct nl_object* obj, int action, void* data) noexcept {
  CHECK(data) << "Opaque context does not exist in link callback";
  reinterpret_cast<NetlinkSocket*>(data)->handleLinkEvent(obj, action, true);
}

void
NetlinkSocket::handleLinkEvent(
    struct nl_object* obj, int action, bool runHandler) noexcept {
  CHECK_NOTNULL(obj);
  if (!checkObjectType(obj, kLinkObjectStr)) {
    return;
  }

  struct rtnl_link* linkObj = reinterpret_cast<struct rtnl_link*>(obj);
  LinkBuilder builder;
  auto link = builder.buildFromObject(linkObj);
  const auto linkName = link.getLinkName();
  auto& linkAttr = links_[linkName];
  linkAttr.isUp = link.isUp();
  linkAttr.ifIndex = link.getIfIndex();
  if (link.isLoopback()) {
    loopbackIfIndex_ = linkAttr.ifIndex;
  }
  if (!linkAttr.isUp) {
    removeNeighborCacheEntries(linkName);
  }
  if (handler_ && runHandler && eventFlags_[LINK_EVENT]) {
    EventVariant event = std::move(link);
    handler_->handleEvent(linkName, action, event);
  }
}

void
NetlinkSocket::removeNeighborCacheEntries(const std::string& ifName) {
  for (auto it = neighbors_.begin(); it != neighbors_.end();) {
    if (std::get<0>(it->first) == ifName) {
      it = neighbors_.erase(it);
    } else {
      ++it;
    }
  }
}

void
NetlinkSocket::addrCacheCB(
    struct nl_cache*, struct nl_object* obj, int action, void* data) noexcept {
  CHECK(data) << "Opaque context does not exist in address callback";
  reinterpret_cast<NetlinkSocket*>(data)->handleAddrEvent(obj, action, true);
}

void
NetlinkSocket::handleAddrEvent(
    struct nl_object* obj, int action, bool runHandler) noexcept {
  CHECK_NOTNULL(obj);
  if (!checkObjectType(obj, kAddrObjectStr)) {
    return;
  }

  struct rtnl_addr* addrObj = reinterpret_cast<struct rtnl_addr*>(obj);
  IfAddressBuilder builder;
  const bool isValid = (action != NL_ACT_DEL);
  auto ifAddr = builder.loadFromObject(addrObj).setValid(isValid).build();
  std::string ifName = getIfName(ifAddr.getIfIndex()).get();
  if (isValid) {
    links_[ifName].networks.insert(ifAddr.getPrefix().value());
  } else if (action == NL_ACT_DEL) {
    auto it = links_.find(ifName);
    if (it != links_.end()) {
      it->second.networks.erase(ifAddr.getPrefix().value());
    }
  }

  if (handler_ && runHandler && eventFlags_[ADDR_EVENT]) {
    EventVariant event = std::move(ifAddr);
    handler_->handleEvent(ifName, action, event);
  }
}

void
NetlinkSocket::neighCacheCB(
    struct nl_cache*, struct nl_object* obj, int action, void* data) noexcept {
  CHECK(data) << "Opaque context does not exist in neighbor callback";
  reinterpret_cast<NetlinkSocket*>(data)->handleNeighborEvent(
      obj, action, true);
}

void
NetlinkSocket::handleNeighborEvent(
    nl_object* obj, int action, bool runHandler) noexcept {
  CHECK_NOTNULL(obj);
  if (!checkObjectType(obj, kNeighborObjectStr)) {
    return;
  }
  struct rtnl_neigh* neighObj = reinterpret_cast<struct rtnl_neigh*>(obj);
  struct nl_addr* dst = rtnl_neigh_get_dst(neighObj);
  if (!dst) {
    LOG(WARNING) << "Empty neighbor in netlink neighbor event";
    return;
  }
  NeighborBuilder builder;
  auto neigh = builder.buildFromObject(neighObj, NL_ACT_DEL == action);
  std::string ifName = getIfName(neigh.getIfIndex()).get();
  auto key = std::make_pair(ifName, neigh.getDestination());
  neighbors_.erase(key);
  if (neigh.isReachable()) {
    neighbors_.emplace(std::make_pair(key, std::move(neigh)));
  }

  if (handler_ && runHandler && eventFlags_[NEIGH_EVENT]) {
    NeighborBuilder nhBuilder;
    EventVariant event = nhBuilder.buildFromObject(neighObj);
    handler_->handleEvent(ifName, action, event);
  }
}

void
NetlinkSocket::doUpdateRouteCache(
    struct rtnl_route* obj, int action, bool updateUnicastRoute) {
  RouteBuilder builder;
  bool isValid = (action != NL_ACT_DEL);
  auto route = builder.loadFromObject(obj).setValid(isValid).build();
  // Skip cached route entries and any routes not in the main table
  int flags = route.getFlags().hasValue() ? route.getFlags().value() : 0;
  if (route.getRouteTable() != RT_TABLE_MAIN || flags & RTM_F_CLONED) {
    return;
  }

  uint8_t protocol = route.getProtocolId();
  // Multicast routes do not belong to our proto
  // Save it in our local copy and move on
  const folly::CIDRNetwork& prefix = route.getDestination();
  if (prefix.first.isMulticast()) {
    if (route.getNextHops().size() != 1) {
      LOG(ERROR) << "Unexpected nextHops for multicast address: "
                 << folly::IPAddress::networkToString(prefix);
      return;
    }
    auto maybeIfIndex = route.getNextHops().begin()->getIfIndex();
    if (!maybeIfIndex.hasValue()) {
      LOG(ERROR) << "Invalid NextHop"
                 << folly::IPAddress::networkToString(prefix);
      return;
    }
    const std::string& ifName = getIfName(maybeIfIndex.value()).get();
    auto key = std::make_pair(prefix, ifName);
    auto& mcastRoutes = mcastRoutesCache_[protocol];

    mcastRoutes.erase(key);
    if (route.isValid()) {
      mcastRoutes.emplace(std::make_pair(key, std::move(route)));
    }
    return;
  }

  // Handle link scope routes
  if (route.getScope() == RT_SCOPE_LINK) {
    if (route.getNextHops().size() != 1) {
      LOG(ERROR) << "Unexpected nextHops for link scope route: "
                 << folly::IPAddress::networkToString(prefix);
      return;
    }
    auto maybeIfIndex = route.getNextHops().begin()->getIfIndex();
    if (!maybeIfIndex.hasValue()) {
      LOG(ERROR) << "Invalid NextHop"
                 << folly::IPAddress::networkToString(prefix);
      return;
    }
    const std::string& ifName = getIfName(maybeIfIndex.value()).get();
    auto key = std::make_pair(prefix, ifName);
    auto& linkRoutes = linkRoutesCache_[protocol];

    linkRoutes.erase(key);
    if (route.isValid()) {
      linkRoutes.emplace(std::make_pair(key, std::move(route)));
    }
    return;
  }

  if (updateUnicastRoute) {
    auto& unicastRoutes = unicastRoutesCache_[protocol];
    if (route.isValid()) {
      unicastRoutes.erase(prefix);
      unicastRoutes.emplace(prefix, std::move(route));
    }
    // NOTE: We are just updating cache. This called during initialization
  }
}

folly::Future<folly::Unit>
NetlinkSocket::addRoute(Route route) {
  auto prefix = route.getDestination();
  VLOG(3) << "NetlinkSocket add route "
          << folly::IPAddress::networkToString(prefix);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this,
                                     p = std::move(promise),
                                     dest = std::move(prefix),
                                     r = std::move(route)]() mutable {
    try {
      uint8_t type = r.getType();
      switch (type) {
      case RTN_UNICAST:
      case RTN_BLACKHOLE:
        doAddUpdateUnicastRoute(std::move(r));
        break;
      case RTN_MULTICAST:
        doAddMulticastRoute(std::move(r));
        break;
      default:
        throw fbnl::NlException(
            folly::sformat("Unsupported route type {}", (int)type));
      }
      p.setValue();
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error adding routes to "
                 << folly::IPAddress::networkToString(dest)
                 << ". Exception: " << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<folly::Unit>
NetlinkSocket::addMplsRoute(Route mplsRoute) {
  auto prefix = mplsRoute.getDestination();
  VLOG(3) << "NetlinkSocket add MPLS route "
          << folly::IPAddress::networkToString(prefix);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this,
                                     p = std::move(promise),
                                     dest = std::move(prefix),
                                     r = std::move(mplsRoute)]() mutable {
    try {
      uint8_t type = r.getType();
      switch (type) {
      case RTN_UNICAST:
        doAddUpdateMplsRoute(std::move(r));
        break;
      default:
        throw fbnl::NlException(
            folly::sformat("Unsupported MPLS route type {}", (int)type));
      }
      p.setValue();
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error adding MPLS routes to "
                 << folly::IPAddress::networkToString(dest)
                 << ". Exception: " << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<folly::Unit>
NetlinkSocket::delMplsRoute(Route mplsRoute) {
  VLOG(3) << "NetlinkSocket deleting MPLS route";
  auto prefix = mplsRoute.getDestination();
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this,
                                     p = std::move(promise),
                                     r = std::move(mplsRoute),
                                     dest = std::move(prefix)]() mutable {
    try {
      uint8_t type = r.getType();
      switch (type) {
      case RTN_UNICAST:
        doDeleteMplsRoute(std::move(r));
        break;
      default:
        throw fbnl::NlException(
            folly::sformat("Unsupported MPLS route type {}", (int)type));
      }
      p.setValue();
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error deleting MPLS routes to "
                 << folly::IPAddress::networkToString(dest)
                 << " Error: " << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<folly::Unit>
NetlinkSocket::syncMplsRoutes(uint8_t protocolId, NlMplsRoutes newMplsRouteDb) {
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this,
                                     p = std::move(promise),
                                     syncDb = std::move(newMplsRouteDb),
                                     protocolId]() mutable {
    try {
      LOG(INFO) << "Syncing " << syncDb.size() << " mpls routes";
      auto& mplsRoutes = mplsRoutesCache_[protocolId];
      std::unordered_set<int32_t> toDelete;
      // collect label routes to delete
      for (auto const& kv : mplsRoutes) {
        if (syncDb.find(kv.first) == syncDb.end()) {
          toDelete.insert(kv.first);
        }
      }
      // delete
      LOG(INFO) << "Sync: Deleting " << toDelete.size() << " mpls routes";
      for (auto label : toDelete) {
        auto mplsRouteEntry = mplsRoutes.at(label);
        doDeleteMplsRoute(mplsRouteEntry);
      }
      // Go over MPLS routes in new routeDb, update/add
      for (auto& kv : syncDb) {
        doAddUpdateMplsRoute(kv.second);
      }
      p.setValue();
      LOG(INFO) << "Sync done.";
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error syncing MPLS routeDb with Fib: "
                 << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<NlMplsRoutes>
NetlinkSocket::getCachedMplsRoutes(uint8_t protocolId) const {
  VLOG(3) << "NetlinkSocket get cached MPLS routes by protocol "
          << (int)protocolId;
  folly::Promise<NlMplsRoutes> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), protocolId]() mutable {
        auto iter = mplsRoutesCache_.find(protocolId);
        if (iter != mplsRoutesCache_.end()) {
          p.setValue(iter->second);
        } else {
          p.setValue(NlMplsRoutes{});
        }
      });
  return future;
}

folly::Future<int64_t>
NetlinkSocket::getMplsRouteCount() const {
  VLOG(3) << "NetlinkSocket get MPLS routes count";

  folly::Promise<int64_t> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this, p = std::move(promise)]() mutable {
    int64_t count = 0;
    for (const auto& routes : mplsRoutesCache_) {
      count += routes.second.size();
    }
    p.setValue(count);
  });
  return future;
}

void
NetlinkSocket::doAddUpdateUnicastRoute(Route route) {
  checkUnicastRoute(route);

  const auto& dest = route.getDestination();

  // Create new set of nexthops to be programmed. Existing + New ones
  auto& unicastRoutes = unicastRoutesCache_[route.getProtocolId()];
  auto iter = unicastRoutes.find(dest);
  // Same route
  if (iter != unicastRoutes.end() && iter->second == route) {
    return;
  }

  if (dest.first.isV6()) {
    // We need to explicitly add new V6 routes & remove old routes
    // With IPv6, if new route being requested has different properties
    // (like gateway or metric or..) the existing one will not be replaced,
    // instead a new route will be created, which may cause underlying kernel
    // crash when releasing netdevices
    if (iter != unicastRoutes.end()) {
      int err{0};
      if (useNetlinkMessage_) {
        err = static_cast<int>(nlSock_->deleteRoute(iter->second));
      } else {
        err = rtnlRouteDelete(reqSock_, iter->second.getRtnlRouteKeyRef(), 0);
        LOG_IF(ERROR, err != 0 && -NLE_OBJ_NOTFOUND != err)
            << "Failed route delete: " << nl_geterror(err);
      }
      if (0 != err && -NLE_OBJ_NOTFOUND != err) {
        throw fbnl::NlException(folly::sformat(
            "Failed to delete route\n{}\nError: {}", iter->second.str(), err));
      }
    }
  }

  // Remove route from cache
  unicastRoutes.erase(dest);

  // Add new route
  int err{0};
  if (useNetlinkMessage_) {
    err = static_cast<int>(nlSock_->addRoute(route));
  } else {
    err = rtnlRouteAdd(reqSock_, route.getRtnlRouteRef(), NLM_F_REPLACE);
    LOG_IF(ERROR, err != 0) << "Failed route add: " << nl_geterror(err);
  }
  if (0 != err) {
    throw fbnl::NlException(
        folly::sformat("Could not add route\n{}\nError: {}", route.str(), err));
  }

  // Add route entry in cache on successful addition
  unicastRoutes.emplace(std::make_pair(dest, std::move(route)));
}

folly::Future<folly::Unit>
NetlinkSocket::delRoute(Route route) {
  VLOG(3) << "NetlinkSocket deleting unicast route";
  auto prefix = route.getDestination();
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this,
                                     p = std::move(promise),
                                     r = std::move(route),
                                     dest = std::move(prefix)]() mutable {
    try {
      uint8_t type = r.getType();
      switch (type) {
      case RTN_UNICAST:
      case RTN_BLACKHOLE:
        doDeleteUnicastRoute(std::move(r));
        break;
      case RTN_MULTICAST:
        doDeleteMulticastRoute(std::move(r));
        break;
      default:
        throw fbnl::NlException(
            folly::sformat("Unsupported route type {}", (int)type));
      }
      p.setValue();
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error deleting routes to "
                 << folly::IPAddress::networkToString(dest)
                 << " Error: " << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

void
NetlinkSocket::checkUnicastRoute(const Route& route) {
  const auto& prefix = route.getDestination();
  if (prefix.first.isMulticast() || prefix.first.isLinkLocal()) {
    throw fbnl::NlException(folly::sformat(
        "Invalid unicast route type for: {}",
        folly::IPAddress::networkToString(prefix)));
  }
}

void
NetlinkSocket::doDeleteMplsRoute(Route mplsRoute) {
  if (!useNetlinkMessage_) {
    LOG(WARNING)
        << "Label programming not supported, enable use_netlink_message flag";
    return;
  }
  auto label = mplsRoute.getMplsLabel();
  if (!label.hasValue()) {
    return;
  }
  auto& mplsRoutes = mplsRoutesCache_[mplsRoute.getProtocolId()];
  if (mplsRoutes.count(label.value()) == 0) {
    LOG(ERROR) << "Trying to delete non-existing label: " << label.value();
    return;
  }

  int err{0};
  err = static_cast<int>(nlSock_->deleteLabelRoute(mplsRoute));
  // Mask off NLE_OBJ_NOTFOUND error because Netlink automatically withdraw
  // some routes when interface goes down
  if (err != 0 && -NLE_OBJ_NOTFOUND != err) {
    throw fbnl::NlException(folly::sformat(
        "Failed to delete MPLS {} Error: {}", label.value(), err));
  }
  // Update local cache with removed prefix
  mplsRoutes.erase(label.value());
}

void
NetlinkSocket::doAddUpdateMplsRoute(Route mplsRoute) {
  if (!useNetlinkMessage_) {
    LOG(WARNING)
        << "Label programming not supported, enable use_netlink_message flag";
    return;
  };
  auto label = mplsRoute.getMplsLabel();
  if (!label.hasValue()) {
    LOG(ERROR) << "MPLS route add - no label provided";
    return;
  }
  // check cache has the same entry
  auto& mplsRoutes = mplsRoutesCache_[mplsRoute.getProtocolId()];
  auto mplsRouteEntry = mplsRoutes.find(label.value());
  // Same route
  if (mplsRouteEntry != mplsRoutes.end() &&
      mplsRouteEntry->second == mplsRoute) {
    return;
  }

  mplsRoutes.erase(label.value());
  int err{0};
  err = static_cast<int>(nlSock_->addLabelRoute(mplsRoute));
  if (0 != err) {
    throw fbnl::NlException(folly::sformat(
        "Could not add mpls route\n{}\nError: {}", mplsRoute.str(), err));
  }
  // Add MPLS route entry in cache on successful addition
  mplsRoutes.emplace(std::make_pair(
      static_cast<int32_t>(label.value()), std::move(mplsRoute)));
}

void
NetlinkSocket::doDeleteUnicastRoute(Route route) {
  checkUnicastRoute(route);

  const auto& prefix = route.getDestination();
  auto& unicastRoutes = unicastRoutesCache_[route.getProtocolId()];
  if (unicastRoutes.count(prefix) == 0) {
    LOG(ERROR) << "Trying to delete non-existing prefix "
               << folly::IPAddress::networkToString(prefix);
    return;
  }

  int err{0};
  if (useNetlinkMessage_) {
    err = static_cast<int>(nlSock_->deleteRoute(route));
  } else {
    err = rtnlRouteDelete(reqSock_, route.getRtnlRouteKeyRef(), 0);
    LOG_IF(ERROR, err != 0 && -NLE_OBJ_NOTFOUND != err)
        << "Failed route delete: " << nl_geterror(err);
  }
  // Mask off NLE_OBJ_NOTFOUND error because Netlink automatically withdraw
  // some routes when interface goes down
  if (err != 0 && -NLE_OBJ_NOTFOUND != err) {
    throw fbnl::NlException(folly::sformat(
        "Failed to delete route {} Error: {}",
        folly::IPAddress::networkToString(route.getDestination()),
        err));
  }

  // Update local cache with removed prefix
  unicastRoutes.erase(route.getDestination());
}

void
NetlinkSocket::doAddMulticastRoute(Route route) {
  checkMulticastRoute(route);

  auto& mcastRoutes = mcastRoutesCache_[route.getProtocolId()];
  auto prefix = route.getDestination();
  auto ifName = route.getRouteIfName().value();
  auto key = std::make_pair(prefix, ifName);
  if (mcastRoutes.count(key)) {
    // This could be kernel proto or our proto. we dont care
    LOG(WARNING)
        << "Multicast route: " << folly::IPAddress::networkToString(prefix)
        << " exists for interface: " << ifName;
    return;
  }

  VLOG(3)
      << "Adding multicast route: " << folly::IPAddress::networkToString(prefix)
      << " for interface: " << ifName;

  int err{0};
  if (useNetlinkMessage_) {
    err = static_cast<int>(nlSock_->addRoute(route));
  } else {
    err = rtnlRouteAdd(reqSock_, route.getRtnlRouteRef(), 0);
    LOG_IF(ERROR, err != 0)
        << "Failed multicast route add: " << nl_geterror(err);
  }
  if (err != 0) {
    throw fbnl::NlException(folly::sformat(
        "Failed to add multicast route {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        err));
  }
  mcastRoutes.emplace(key, std::move(route));
}

void
NetlinkSocket::checkMulticastRoute(const Route& route) {
  auto prefix = route.getDestination();
  if (not prefix.first.isMulticast()) {
    throw fbnl::NlException(folly::sformat(
        "Invalid multicast address {}",
        folly::IPAddress::networkToString(prefix)));
  }
  if (not route.getRouteIfName().hasValue()) {
    throw fbnl::NlException(folly::sformat(
        "Need set Iface name for multicast address {}",
        folly::IPAddress::networkToString(prefix)));
  }
}

void
NetlinkSocket::doDeleteMulticastRoute(Route route) {
  checkMulticastRoute(route);

  auto& mcastRoutes = mcastRoutesCache_[route.getProtocolId()];
  auto prefix = route.getDestination();
  auto ifName = route.getRouteIfName().value();
  auto key = std::make_pair(prefix, ifName);
  auto iter = mcastRoutes.find(key);
  if (iter == mcastRoutes.end()) {
    // This could be kernel proto or our proto. we dont care
    LOG(WARNING)
        << "Multicast route: " << folly::IPAddress::networkToString(prefix)
        << " doesn't exists for interface: " << ifName;
    return;
  }

  VLOG(3) << "Deleting multicast route: "
          << folly::IPAddress::networkToString(prefix)
          << " for interface: " << ifName;

  int err{0};
  if (useNetlinkMessage_) {
    err = static_cast<int>(nlSock_->deleteRoute(iter->second));
  } else {
    err = rtnlRouteDelete(reqSock_, iter->second.getRtnlRouteKeyRef(), 0);
    LOG_IF(ERROR, err != 0)
        << "Failed multicast route delete: " << nl_geterror(err);
  }
  if (err != 0) {
    throw fbnl::NlException(folly::sformat(
        "Failed to delete multicast route {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        err));
  }

  mcastRoutes.erase(iter);
}

folly::Future<folly::Unit>
NetlinkSocket::syncUnicastRoutes(
    uint8_t protocolId, NlUnicastRoutes newRouteDb) {
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this,
                                     p = std::move(promise),
                                     syncDb = std::move(newRouteDb),
                                     protocolId]() mutable {
    try {
      LOG(INFO) << "Syncing " << syncDb.size() << " routes";
      doSyncUnicastRoutes(protocolId, std::move(syncDb));
      p.setValue();
      LOG(INFO) << "Sync done.";
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error syncing unicast routeDb with Fib: "
                 << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

void
NetlinkSocket::doSyncUnicastRoutes(uint8_t protocolId, NlUnicastRoutes syncDb) {
  auto& unicastRoutes = unicastRoutesCache_[protocolId];

  // Go over routes that are not in new routeDb, delete
  std::unordered_set<folly::CIDRNetwork> toDelete;
  for (auto const& kv : unicastRoutes) {
    if (syncDb.find(kv.first) == syncDb.end()) {
      toDelete.insert(kv.first);
    }
  }
  // Delete routes from kernel
  LOG(INFO) << "Sync: number of routes to delete: " << toDelete.size();
  for (auto it = toDelete.begin(); it != toDelete.end(); ++it) {
    auto const& prefix = *it;
    auto iter = unicastRoutes.find(prefix);
    if (iter == unicastRoutes.end()) {
      continue;
    }
    doDeleteUnicastRoute(iter->second);
  }

  // Go over routes in new routeDb, update/add
  LOG(INFO) << "Sync: number of routes to add: " << syncDb.size();
  for (auto& kv : syncDb) {
    doAddUpdateUnicastRoute(kv.second);
  }
}

folly::Future<folly::Unit>
NetlinkSocket::syncLinkRoutes(uint8_t protocolId, NlLinkRoutes newRouteDb) {
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this,
                                     p = std::move(promise),
                                     syncDb = std::move(newRouteDb),
                                     protocolId]() mutable {
    try {
      doSyncLinkRoutes(protocolId, std::move(syncDb));
      p.setValue();
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error syncing unicast routeDb with Fib: "
                 << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

void
NetlinkSocket::doSyncLinkRoutes(uint8_t protocolId, NlLinkRoutes syncDb) {
  auto& linkRoutes = linkRoutesCache_[protocolId];
  std::vector<std::pair<folly::CIDRNetwork, std::string>> toDel;
  for (const auto& route : linkRoutes) {
    if (!syncDb.count(route.first)) {
      toDel.emplace_back(route.first);
    }
  }
  for (const auto& routeToDel : toDel) {
    auto iter = linkRoutes.find(routeToDel);
    if (iter == linkRoutes.end()) {
      continue;
    }

    int err{0};
    if (useNetlinkMessage_) {
      err = static_cast<int>(nlSock_->deleteRoute(iter->second));
    } else {
      err = rtnlRouteDelete(reqSock_, iter->second.getRtnlRouteKeyRef(), 0);
      LOG_IF(ERROR, err != 0) << "Failed route delete: " << nl_geterror(err);
    }
    if (err != 0) {
      throw fbnl::NlException(folly::sformat(
          "Could not del link Route to: {} dev {} Error: {}",
          folly::IPAddress::networkToString(routeToDel.first),
          routeToDel.second,
          err));
    }
  }

  for (auto& routeToAdd : syncDb) {
    if (linkRoutes.count(routeToAdd.first)) {
      continue;
    }

    int err{0};
    if (useNetlinkMessage_) {
      err = static_cast<int>(nlSock_->addRoute(routeToAdd.second));
    } else {
      err = rtnlRouteAdd(
          reqSock_, routeToAdd.second.getRtnlRouteRef(), NLM_F_REPLACE);
      LOG_IF(ERROR, err != 0) << "Failed route add: " << nl_geterror(err);
    }
    if (err != 0) {
      throw fbnl::NlException(folly::sformat(
          "Could not add link Route to: {} dev {} Error: {}",
          folly::IPAddress::networkToString(routeToAdd.first.first),
          routeToAdd.first.second,
          err));
    }
  }
  linkRoutes.swap(syncDb);
}

folly::Future<NlUnicastRoutes>
NetlinkSocket::getCachedUnicastRoutes(uint8_t protocolId) const {
  VLOG(3) << "NetlinkSocket getCachedUnicastRoutes by protocol "
          << (int)protocolId;
  folly::Promise<NlUnicastRoutes> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), protocolId]() mutable {
        auto iter = unicastRoutesCache_.find(protocolId);
        if (iter != unicastRoutesCache_.end()) {
          p.setValue(iter->second);
        } else {
          p.setValue(NlUnicastRoutes{});
        }
      });
  return future;
}

folly::Future<NlMulticastRoutes>
NetlinkSocket::getCachedMulticastRoutes(uint8_t protocolId) const {
  VLOG(3) << "NetlinkSocket getCachedMulticastRoutes by protocol "
          << (int)protocolId;
  folly::Promise<NlMulticastRoutes> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), protocolId]() mutable {
        auto iter = mcastRoutesCache_.find(protocolId);
        if (iter != mcastRoutesCache_.end()) {
          p.setValue(iter->second);
        } else {
          p.setValue(NlMulticastRoutes());
        }
      });
  return future;
}

folly::Future<NlLinkRoutes>
NetlinkSocket::getCachedLinkRoutes(uint8_t protocolId) const {
  VLOG(3) << "NetlinkSocket getCachedLinkRoutes by protocol "
          << (int)protocolId;

  folly::Promise<NlLinkRoutes> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), protocolId]() mutable {
        auto iter = linkRoutesCache_.find(protocolId);
        if (iter != linkRoutesCache_.end()) {
          p.setValue(iter->second);
        } else {
          p.setValue(NlLinkRoutes{});
        }
      });
  return future;
}

folly::Future<int64_t>
NetlinkSocket::getRouteCount() const {
  VLOG(3) << "NetlinkSocket get routes number";

  folly::Promise<int64_t> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this, p = std::move(promise)]() mutable {
    int64_t count = 0;
    for (const auto& routes : unicastRoutesCache_) {
      count += routes.second.size();
    }
    p.setValue(count);
  });
  return future;
}

folly::Future<int>
NetlinkSocket::getIfIndex(const std::string& ifName) {
  folly::Promise<int> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), ifStr = ifName.c_str()]() mutable {
        p.setValue(rtnl_link_name2i(linkCache_, ifStr));
      });
  return future;
}

folly::Future<folly::Optional<int>>
NetlinkSocket::getLoopbackIfindex() {
  folly::Promise<folly::Optional<int>> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop([this, p = std::move(promise)]() mutable {
    p.setValue(loopbackIfIndex_);
  });
  return future;
} // namespace fbnl

folly::Future<std::string>
NetlinkSocket::getIfName(int ifIndex) const {
  folly::Promise<std::string> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), ifIndex]() mutable {
        std::array<char, IFNAMSIZ> ifNameBuf;
        std::string ifName(rtnl_link_i2name(
            linkCache_, ifIndex, ifNameBuf.data(), ifNameBuf.size()));
        p.setValue(ifName);
      });
  return future;
}

folly::Future<folly::Unit>
NetlinkSocket::addIfAddress(IfAddress ifAddress) {
  VLOG(3) << "NetlinkSocket add IfAddress...";

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), addr = std::move(ifAddress)]() mutable {
        try {
          doAddIfAddress(addr.getRtnlAddrRef());
          p.setValue();
        } catch (const std::exception& ex) {
          p.setException(ex);
        }
      });
  return future;
}

void
NetlinkSocket::doAddIfAddress(struct rtnl_addr* addr) {
  if (nullptr == addr) {
    throw fbnl::NlException("Can't get rtnl_addr");
  }
  int err = rtnl_addr_add(reqSock_, addr, 0);
  // NLE_EXIST means duplicated address
  // we treat it as success for backward compatibility
  if (NLE_SUCCESS != err && -NLE_EXIST != err) {
    throw fbnl::NlException(
        folly::sformat("Failed to add address Error: {}", nl_geterror(err)));
  }
}

folly::Future<folly::Unit>
NetlinkSocket::delIfAddress(IfAddress ifAddress) {
  VLOG(3) << "Netlink delete IfAddress...";

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  if (!ifAddress.getPrefix().hasValue()) {
    fbnl::NlException ex("Prefix must be set");
    promise.setException(std::move(ex));
    return future;
  }
  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), ifAddr = std::move(ifAddress)]() mutable {
        struct rtnl_addr* addr = ifAddr.getRtnlAddrRef();
        try {
          doDeleteAddr(addr);
          p.setValue();
        } catch (const std::exception& ex) {
          p.setException(ex);
        }
      });
  return future;
}

folly::Future<folly::Unit>
NetlinkSocket::syncIfAddress(
    int ifIndex, std::vector<IfAddress> addresses, int family, int scope) {
  VLOG(3) << "Netlink sync IfAddress...";

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop([this,
                                     p = std::move(promise),
                                     addrs = std::move(addresses),
                                     ifIndex,
                                     family,
                                     scope]() mutable {
    try {
      doSyncIfAddress(ifIndex, std::move(addrs), family, scope);
      p.setValue();
    } catch (const std::exception& ex) {
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<std::vector<IfAddress>>
NetlinkSocket::getIfAddrs(int ifIndex, int family, int scope) {
  VLOG(3) << "Netlink get IfaceAddrs...";

  folly::Promise<std::vector<IfAddress>> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), ifIndex, family, scope]() mutable {
        try {
          std::vector<IfAddress> addrs;
          doGetIfAddrs(ifIndex, family, scope, addrs);
          p.setValue(std::move(addrs));
        } catch (const std::exception& ex) {
          p.setException(ex);
        }
      });
  return future;
}

void
NetlinkSocket::doSyncIfAddress(
    int ifIndex, std::vector<IfAddress> addrs, int family, int scope) {
  // Check ifindex and prefix
  std::vector<folly::CIDRNetwork> newPrefixes;
  for (const auto& addr : addrs) {
    if (addr.getIfIndex() != ifIndex) {
      throw fbnl::NlException("Inconsistent ifIndex in addrs");
    }
    if (!addr.getPrefix().hasValue()) {
      throw fbnl::NlException("Prefix must be set when sync addresses");
    }
    newPrefixes.emplace_back(addr.getPrefix().value());
  }

  std::vector<folly::CIDRNetwork> oldPrefixes;
  auto oldAddrs = getIfAddrs(ifIndex, family, scope).get();
  for (const auto& addr : oldAddrs) {
    oldPrefixes.emplace_back(addr.getPrefix().value());
  }

  PrefixCmp cmp;
  sort(newPrefixes.begin(), newPrefixes.end(), cmp);
  sort(oldPrefixes.begin(), oldPrefixes.end(), cmp);

  // get a list of prefixes need to be deleted
  std::vector<folly::CIDRNetwork> toDeletePrefixes;
  std::set_difference(
      oldPrefixes.begin(),
      oldPrefixes.end(),
      newPrefixes.begin(),
      newPrefixes.end(),
      std::inserter(toDeletePrefixes, toDeletePrefixes.begin()));

  // Do add first, because in Linux deleting the only IP will cause if down.
  // Add new address, existed addresses will be ignored
  for (auto& addr : addrs) {
    doAddIfAddress(addr.getRtnlAddrRef());
  }

  // Delete deprecated addresses
  fbnl::IfAddressBuilder builder;
  for (const auto& toDel : toDeletePrefixes) {
    auto delAddr =
        builder.setIfIndex(ifIndex).setPrefix(toDel).setScope(scope).build();
    doDeleteAddr(delAddr.getRtnlAddrRef());
  }
}

void
NetlinkSocket::doDeleteAddr(struct rtnl_addr* addr) {
  if (nullptr == addr) {
    throw fbnl::NlException("Can't get rtnl_addr");
  }
  int err = rtnl_addr_delete(reqSock_, addr, 0);
  // NLE_NOADDR means delete invalid address
  // we treat it as success for backward compatibility
  if (NLE_SUCCESS != err && -NLE_NOADDR != err) {
    throw fbnl::NlException(
        folly::sformat("Failed to delete address Error: {}", nl_geterror(err)));
  }
}

void
NetlinkSocket::doGetIfAddrs(
    int ifIndex, int family, int scope, std::vector<IfAddress>& addrs) {
  GetAddrsFuncCtx funcCtx(ifIndex, family, scope);
  auto getFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    GetAddrsFuncCtx* ctx = static_cast<GetAddrsFuncCtx*>(arg);
    struct rtnl_addr* toAdd = reinterpret_cast<struct rtnl_addr*>(obj);

    if (ctx->family != AF_UNSPEC &&
        ctx->family != rtnl_addr_get_family(toAdd)) {
      return;
    }
    if (ctx->scope != RT_SCOPE_NOWHERE &&
        ctx->scope != rtnl_addr_get_scope(toAdd)) {
      return;
    }
    if (ctx->ifIndex != rtnl_addr_get_ifindex(toAdd)) {
      return;
    }
    struct nl_addr* ipaddr = rtnl_addr_get_local(toAdd);
    if (!ipaddr) {
      return;
    }

    folly::IPAddress ipAddress = folly::IPAddress::fromBinary(folly::ByteRange(
        static_cast<const unsigned char*>(nl_addr_get_binary_addr(ipaddr)),
        nl_addr_get_len(ipaddr)));
    folly::CIDRNetwork prefix =
        std::make_pair(ipAddress, rtnl_addr_get_prefixlen(toAdd));
    fbnl::IfAddressBuilder ifBuilder;
    auto tmpAddr = ifBuilder.setPrefix(prefix)
                       .setIfIndex(ctx->ifIndex)
                       .setScope(ctx->scope)
                       .build();
    ctx->addrs.emplace_back(std::move(tmpAddr));
  };

  nl_cache_refill(reqSock_, addrCache_);
  nl_cache_foreach(addrCache_, getFunc, &funcCtx);
  funcCtx.addrs.swap(addrs);
}

folly::Future<NlLinks>
NetlinkSocket::getAllLinks() {
  VLOG(3) << "NetlinkSocket get all links...";
  folly::Promise<NlLinks> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop([this, p = std::move(promise)]() mutable {
    try {
      updateLinkCache();
      updateAddrCache();
      p.setValue(links_);
    } catch (const std::exception& ex) {
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<NlNeighbors>
NetlinkSocket::getAllReachableNeighbors() {
  VLOG(3) << "NetlinkSocket get neighbors...";
  folly::Promise<NlNeighbors> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop([this, p = std::move(promise)]() mutable {
    try {
      // Neighbor need linkcache to map ifIndex to name
      updateLinkCache();
      updateAddrCache();
      updateNeighborCache();
      p.setValue(std::move(neighbors_));
    } catch (const std::exception& ex) {
      p.setException(ex);
    }
  });
  return future;
}

bool
NetlinkSocket::checkObjectType(
    struct nl_object* obj, folly::StringPiece expectType) {
  CHECK_NOTNULL(obj);
  const char* objectStr = nl_object_get_type(obj);
  if (objectStr && objectStr != expectType) {
    LOG(ERROR) << "Invalid nl_object type, expect: " << expectType
               << ",  actual: " << objectStr;
    return false;
  }
  return true;
}

void
NetlinkSocket::updateLinkCache() {
  auto linkFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    CHECK(arg) << "Opaque context does not exist";
    reinterpret_cast<NetlinkSocket*>(arg)->handleLinkEvent(
        obj, NL_ACT_GET, false);
  };
  nl_cache_refill(reqSock_, linkCache_);
  nl_cache_foreach_filter(linkCache_, nullptr, linkFunc, this);
}

void
NetlinkSocket::updateAddrCache() {
  auto addrFunc = [](struct nl_object * obj, void* arg) noexcept {
    CHECK(arg) << "Opaque context does not exist";
    reinterpret_cast<NetlinkSocket*>(arg)->handleAddrEvent(
        obj, NL_ACT_GET, false);
  };
  nl_cache_refill(reqSock_, addrCache_);
  nl_cache_foreach_filter(addrCache_, nullptr, addrFunc, this);
}

void
NetlinkSocket::updateNeighborCache() {
  auto neighborFunc = [](struct nl_object * obj, void* arg) noexcept {
    CHECK(arg) << "Opaque context does not exist";
    reinterpret_cast<NetlinkSocket*>(arg)->handleNeighborEvent(
        obj, NL_ACT_GET, false);
  };
  nl_cache_foreach_filter(neighborCache_, nullptr, neighborFunc, this);
}

void
NetlinkSocket::updateRouteCache() {
  auto routeFunc = [](struct nl_object * obj, void* arg) noexcept {
    CHECK(arg) << "Opaque context does not exist";
    reinterpret_cast<NetlinkSocket*>(arg)->handleRouteEvent(
        obj, NL_ACT_GET, false, true);
  };
  nl_cache_foreach_filter(routeCache_, nullptr, routeFunc, this);
}

void
NetlinkSocket::subscribeEvent(NetlinkEventType event) {
  if (event >= MAX_EVENT_TYPE) {
    return;
  }
  if (event == ROUTE_EVENT) {
    CHECK(!useNetlinkMessage_);
  }
  eventFlags_.set(event);
}

void
NetlinkSocket::unsubscribeEvent(NetlinkEventType event) {
  if (event >= MAX_EVENT_TYPE) {
    return;
  }
  eventFlags_.reset(event);
}

void
NetlinkSocket::subscribeAllEvents() {
  for (size_t i = 0; i < MAX_EVENT_TYPE; ++i) {
    eventFlags_.set(i);
  }
}

void
NetlinkSocket::unsubscribeAllEvents() {
  for (size_t i = 0; i < MAX_EVENT_TYPE; ++i) {
    eventFlags_.reset(i);
  }
}

void
NetlinkSocket::setEventHandler(EventsHandler* handler) {
  handler_ = handler;
}

int
NetlinkSocket::rtnlRouteAdd(
    struct nl_sock* sock, struct rtnl_route* route, int flags) {
  tickEvent();
  VLOG(1) << "Adding route : " << route;
  return rtnl_route_add(sock, route, flags);
}

int
NetlinkSocket::rtnlRouteDelete(
    struct nl_sock* sock, struct rtnl_route* route, int flags) {
  tickEvent();
  VLOG(1) << "Deleting route : " << route;
  return rtnl_route_delete(sock, route, flags);
}

void
NetlinkSocket::tickEvent() {
  if (++eventCount_ == 0) {
    nl_cache_mngr_poll(cacheManager_, 0 /* timeout */);
  }
}

} // namespace fbnl
} // namespace openr
