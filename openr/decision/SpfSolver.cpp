/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/logging/xlog.h>

#include <openr/common/LsdbUtil.h>
#include <openr/common/MplsUtil.h>
#include <openr/decision/RibEntry.h>
#include <openr/decision/SpfSolver.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>

namespace fb303 = facebook::fb303;

namespace openr {

DecisionRouteUpdate
DecisionRouteDb::calculateUpdate(DecisionRouteDb&& newDb) const {
  DecisionRouteUpdate delta;

  // unicastRoutesToUpdate
  for (auto& [prefix, entry] : newDb.unicastRoutes) {
    const auto& search = unicastRoutes.find(prefix);
    if (search == unicastRoutes.end() || search->second != entry) {
      // new prefix, or prefix entry changed
      delta.addRouteToUpdate(std::move(entry));
    }
  }

  // unicastRoutesToDelete
  for (auto const& [prefix, _] : unicastRoutes) {
    if (!newDb.unicastRoutes.count(prefix)) {
      delta.unicastRoutesToDelete.emplace_back(prefix);
    }
  }

  // mplsRoutesToUpdate
  for (auto& [label, entry] : newDb.mplsRoutes) {
    const auto& search = mplsRoutes.find(label);
    if (search == mplsRoutes.end() || search->second != entry) {
      delta.addMplsRouteToUpdate(std::move(entry));
    }
  }

  // mplsRoutesToDelete
  for (auto const& [label, _] : mplsRoutes) {
    if (!newDb.mplsRoutes.count(label)) {
      delta.mplsRoutesToDelete.emplace_back(label);
    }
  }
  return delta;
}

void
DecisionRouteDb::update(DecisionRouteUpdate const& update) {
  for (auto const& prefix : update.unicastRoutesToDelete) {
    unicastRoutes.erase(prefix);
  }
  for (auto const& [_, entry] : update.unicastRoutesToUpdate) {
    unicastRoutes.insert_or_assign(entry.prefix, entry);
  }
  for (auto const& label : update.mplsRoutesToDelete) {
    mplsRoutes.erase(label);
  }
  for (auto const& [_, entry] : update.mplsRoutesToUpdate) {
    mplsRoutes.insert_or_assign(entry.label, entry);
  }
}

SpfSolver::SpfSolver(
    const std::string& myNodeName,
    bool enableV4,
    bool enableNodeSegmentLabel,
    bool enableAdjacencyLabels,
    bool enableBestRouteSelection,
    bool v4OverV6Nexthop)
    : myNodeName_(myNodeName),
      enableV4_(enableV4),
      enableNodeSegmentLabel_(enableNodeSegmentLabel),
      enableAdjacencyLabels_(enableAdjacencyLabels),
      enableBestRouteSelection_(enableBestRouteSelection),
      v4OverV6Nexthop_(v4OverV6Nexthop) {
  // Initialize stat keys
  fb303::fbData->addStatExportType("decision.adj_db_update", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "decision.incompatible_forwarding_type", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.no_route_to_label", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.no_route_to_prefix", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.path_build_ms", fb303::AVG);
  fb303::fbData->addStatExportType("decision.prefix_db_update", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.route_build_ms", fb303::AVG);
  fb303::fbData->addStatExportType("decision.route_build_runs", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "decision.get_route_for_prefix", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.skipped_mpls_route", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "decision.duplicate_node_label", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "decision.skipped_unicast_route", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.spf_ms", fb303::AVG);
  fb303::fbData->addStatExportType("decision.spf_runs", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.errors", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "decision.incorrect_redistribution_route", fb303::COUNT);
}

SpfSolver::~SpfSolver() = default;

void
SpfSolver::updateStaticUnicastRoutes(
    const std::unordered_map<folly::CIDRNetwork, RibUnicastEntry>&
        unicastRoutesToUpdate,
    const std::vector<folly::CIDRNetwork>& unicastRoutesToDelete) {
  // Process IP routes to add or update
  XLOG_IF(INFO, unicastRoutesToUpdate.size())
      << "Adding/Updating " << unicastRoutesToUpdate.size()
      << " static unicast routes.";
  for (const auto& [prefix, ribUnicastEntry] : unicastRoutesToUpdate) {
    staticUnicastRoutes_.insert_or_assign(prefix, ribUnicastEntry);

    XLOG(DBG1) << "> " << folly::IPAddress::networkToString(prefix)
               << ", NextHopsCount = " << ribUnicastEntry.nexthops.size();
    for (auto const& nh : ribUnicastEntry.nexthops) {
      XLOG(DBG2) << " via " << toString(nh);
    }
  }

  XLOG_IF(INFO, unicastRoutesToDelete.size())
      << "Deleting " << unicastRoutesToDelete.size()
      << " static unicast routes.";
  for (const auto& prefix : unicastRoutesToDelete) {
    // mark unicast entry to be deleted
    staticUnicastRoutes_.erase(prefix);

    XLOG(DBG1) << "> " << folly::IPAddress::networkToString(prefix);
  }
}

std::optional<RibUnicastEntry>
SpfSolver::createRouteForPrefixOrGetStaticRoute(
    const std::string& myNodeName,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState,
    folly::CIDRNetwork const& prefix) {
  // route output from `PrefixState` has higher priority over
  // static unicast routes
  if (auto maybeRoute = createRouteForPrefix(
          myNodeName, areaLinkStates, prefixState, prefix)) {
    return maybeRoute;
  }

  // check `staticUnicastRoutes_`
  auto it = staticUnicastRoutes_.find(prefix);
  if (it != staticUnicastRoutes_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<RibUnicastEntry>
SpfSolver::createRouteForPrefix(
    const std::string& myNodeName,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState,
    folly::CIDRNetwork const& prefix) {
  fb303::fbData->addStatValue("decision.get_route_for_prefix", 1, fb303::COUNT);

  // Sanity check for V4 prefixes
  const bool isV4Prefix = prefix.first.isV4();
  if (isV4Prefix and (not enableV4_) and (not v4OverV6Nexthop_)) {
    XLOG(WARNING) << "Received v4 prefix "
                  << folly::IPAddress::networkToString(prefix)
                  << " while v4 is not enabled, and "
                  << "we are not allowing v4 prefix over v6 nexthop.";
    fb303::fbData->addStatValue(
        "decision.skipped_unicast_route", 1, fb303::COUNT);
    return std::nullopt;
  }

  auto search = prefixState.prefixes().find(prefix);
  if (search == prefixState.prefixes().end()) {
    return std::nullopt;
  }
  auto const& allPrefixEntries = search->second;

  // Clear best route selection in prefix state
  bestRoutesCache_.erase(prefix);

  //
  // Create list of prefix-entries from reachable nodes only
  // NOTE: We're copying prefix-entries and it can be expensive. Using
  // pointers for storing prefix information can be efficient (CPU & Memory)
  //
  auto prefixEntries = folly::copy(allPrefixEntries);
  bool localPrefixConsidered{false};
  for (auto& [area, linkState] : areaLinkStates) {
    auto const& mySpfResult = linkState.getSpfResult(myNodeName);

    // Delete entries of unreachable nodes from prefixEntries
    for (auto it = prefixEntries.cbegin(); it != prefixEntries.cend();) {
      const auto& [prefixNode, prefixArea] = it->first;
      // TODO: remove this when tie-breaking process completely done in Decision
      // instead of PrefixManager
      // This indicates that when we calculated the
      // best path, we have considered locally originated prefix as well
      if (myNodeName == prefixNode) {
        localPrefixConsidered = true;
      }
      // Only check reachability within the area that prefixNode belongs to.
      if (area != prefixArea || mySpfResult.count(prefixNode)) {
        ++it; // retain
      } else {
        it = prefixEntries.erase(it); // erase the unreachable prefix entry
      }
    }
  }

  // Skip if no valid prefixes
  if (prefixEntries.empty()) {
    XLOG(DBG3) << "Skipping route to "
               << folly::IPAddress::networkToString(prefix)
               << " with no reachable node.";
    fb303::fbData->addStatValue("decision.no_route_to_prefix", 1, fb303::COUNT);
    return std::nullopt;
  }

  /*
   * [Best Route Selection]
   *
   * Prefix can be advertised from multiple places:
   *  - locally originated;
   *  - re-distributeed from BGP speaker;
   *  - re-advertised across multiple areas;
   *  - etc.;
   *
   * route selection procedure will find the best candidate(NodeAndArea) to run
   * Dijkstra(SPF) or K-Shortest Path Forwarding algorithm against.
   */
  auto routeSelectionResult =
      selectBestRoutes(myNodeName, prefix, prefixEntries, areaLinkStates);
  if (routeSelectionResult.allNodeAreas.empty()) {
    XLOG(WARNING) << "No route to prefix "
                  << folly::IPAddress::networkToString(prefix);
    fb303::fbData->addStatValue("decision.no_route_to_prefix", 1, fb303::COUNT);
    return std::nullopt;
  }

  // Set best route selection in prefix state
  bestRoutesCache_.insert_or_assign(prefix, routeSelectionResult);

  /*
   * ATTN:
   * Skip adding route if one prefix is advertised by local node.
   */
  if (routeSelectionResult.hasNode(myNodeName)) {
    XLOG(DBG3) << "Skip adding route for prefixes advertised by " << myNodeName
               << " " << folly::IPAddress::networkToString(prefix);

    return std::nullopt;
  }

  // Match the best route's attributes (tags & area stack) to an SR Policy.
  // If the route doesn't match to any, default rules are returned
  auto routeComputationRules = getRouteComputationRules(
      prefixEntries, routeSelectionResult, areaLinkStates);

  /*
   * [Route Computation]
   *
   * For each area:
   * - Switch on algorithm type:
   *   - Compute paths, algorithm type influences this step (SP or KSP2);
   *   - Create next-hops from paths, forwarding type influences this step;
   *   - Only use the next-hop set if it has the shortest metric;
   *   - Combine shortest metric next-hops from all areas;
   */
  std::unordered_set<thrift::NextHopThrift> totalNextHops;
  std::unordered_set<thrift::NextHopThrift> ksp2NextHops;
  Metric shortestMetric = std::numeric_limits<Metric>::max();

  // TODO: simplify the areaPathComputationRules usage. No more SR policy.
  for (const auto& [area, areaRules] :
       *routeComputationRules.areaPathComputationRules()) {
    const auto& linkState = areaLinkStates.find(area);
    if (linkState == areaLinkStates.end()) {
      // Possible if the route computation rules are based of a configured SR
      // Policy which contains area path computation rules for an invalid area.
      //
      // If the route computation rules are default then area path computation
      // rules will only contains valid areas.
      continue;
    }

    switch (*areaRules.forwardingAlgo()) {
    case thrift::PrefixForwardingAlgorithm::SP_ECMP:
    case thrift::PrefixForwardingAlgorithm::SP_UCMP_ADJ_WEIGHT_PROPAGATION:
    case thrift::PrefixForwardingAlgorithm::SP_UCMP_PREFIX_WEIGHT_PROPAGATION: {
      auto spfAreaResults = selectBestPathsSpf(
          myNodeName, prefix, routeSelectionResult, area, linkState->second);

      // Only use next-hops in areas with the shortest IGP metric
      if (shortestMetric >= spfAreaResults.bestMetric) {
        if (shortestMetric > spfAreaResults.bestMetric) {
          shortestMetric = spfAreaResults.bestMetric;
          totalNextHops.clear();
        }
        totalNextHops.insert(
            spfAreaResults.nextHops.begin(), spfAreaResults.nextHops.end());
      }
    } break;
    case thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP: {
      // T96779848: selectBestPathsKsp2() should only use selected
      // routes with the best IGP metrics (similar to selectBestPathsSpf)
      // Also next-hops returned by selectBestPathsKsp2() should only be
      // used if they have the best IGP metrics compared to other areas.
      // Comment above for T96776309 also applies here as well.
      auto areaNextHops = selectBestPathsKsp2(
          myNodeName,
          prefix,
          routeSelectionResult,
          prefixEntries,
          *areaRules.forwardingType(),
          area,
          linkState->second);
      ksp2NextHops.insert(areaNextHops.begin(), areaNextHops.end());
    } break;
    default:
      XLOG(ERR)
          << "Unknown prefix algorithm type "
          << apache::thrift::util::enumNameSafe(*areaRules.forwardingAlgo())
          << " for prefix " << folly::IPAddress::networkToString(prefix);
      break;
    }

    // Merge nexthops from SP and KSP2 path computations.
    totalNextHops.insert(ksp2NextHops.begin(), ksp2NextHops.end());
  }

  return addBestPaths(
      myNodeName,
      prefix,
      routeSelectionResult,
      prefixEntries,
      std::move(totalNextHops),
      shortestMetric,
      localPrefixConsidered);
}

std::optional<DecisionRouteDb>
SpfSolver::buildRouteDb(
    const std::string& myNodeName,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState) {
  bool nodeExist{false};
  for (const auto& [_, linkState] : areaLinkStates) {
    nodeExist |= linkState.hasNode(myNodeName);
  }
  if (not nodeExist) {
    return std::nullopt;
  }

  const auto startTime = std::chrono::steady_clock::now();
  fb303::fbData->addStatValue("decision.route_build_runs", 1, fb303::COUNT);

  DecisionRouteDb routeDb{};

  // Clear best route selection cache
  bestRoutesCache_.clear();

  // Create IPv4, IPv6 routes (includes IP -> MPLS routes)
  for (const auto& [prefix, _] : prefixState.prefixes()) {
    if (auto maybeRoute = createRouteForPrefix(
            myNodeName, areaLinkStates, prefixState, prefix)) {
      routeDb.addUnicastRoute(std::move(maybeRoute).value());
    }
  }

  // Create static unicast routes
  for (auto [prefix, ribUnicastEntry] : staticUnicastRoutes_) {
    if (routeDb.unicastRoutes.count(prefix)) {
      // ignore prefixes as prefixState has higher priority
      continue;
    }
    routeDb.addUnicastRoute(RibUnicastEntry(ribUnicastEntry));
  }

  //
  // Create MPLS routes for all nodeLabel
  //
  if (enableNodeSegmentLabel_) {
    std::unordered_map<int32_t, std::pair<std::string, RibMplsEntry>>
        labelToNode;
    for (const auto& [area, linkState] : areaLinkStates) {
      for (const auto& [_, adjDb] : linkState.getAdjacencyDatabases()) {
        const auto topLabel = *adjDb.nodeLabel();
        const auto& nodeName = *adjDb.thisNodeName();
        // Top label is not set => Non-SR mode
        if (topLabel == 0) {
          XLOG(INFO) << "Ignoring node label " << topLabel << " of node "
                     << nodeName << " in area " << area;
          fb303::fbData->addStatValue(
              "decision.skipped_mpls_route", 1, fb303::COUNT);
          continue;
        }
        // If mpls label is not valid then ignore it
        if (not isMplsLabelValid(topLabel)) {
          XLOG(ERR) << "Ignoring invalid node label " << topLabel << " of node "
                    << nodeName << " in area " << area;
          fb303::fbData->addStatValue(
              "decision.skipped_mpls_route", 1, fb303::COUNT);
          continue;
        }

        // There can be a temporary collision in node label allocation.
        // Usually happens when two segmented networks allocating labels from
        // the same range join together. In case of such conflict we respect
        // the node label of bigger node-ID
        auto iter = labelToNode.find(topLabel);
        if (iter != labelToNode.end()) {
          XLOG(INFO) << "Found duplicate label " << topLabel << "from "
                     << iter->second.first << " " << nodeName << " in area "
                     << area;
          fb303::fbData->addStatValue(
              "decision.duplicate_node_label", 1, fb303::COUNT);
          if (iter->second.first < nodeName) {
            continue;
          }
        }

        // Install POP_AND_LOOKUP for next layer
        if (*adjDb.thisNodeName() == myNodeName) {
          thrift::NextHopThrift nh;
          nh.address() = toBinaryAddress(folly::IPAddressV6("::"));
          nh.area() = area;
          nh.mplsAction() =
              createMplsAction(thrift::MplsActionCode::POP_AND_LOOKUP);
          labelToNode.erase(topLabel);
          labelToNode.emplace(
              topLabel,
              std::make_pair(myNodeName, RibMplsEntry(topLabel, {nh})));
          continue;
        }

        // Get best nexthop towards the node
        auto metricNhs = getNextHopsWithMetric(
            myNodeName, {{adjDb.thisNodeName().value(), area}}, linkState);
        if (metricNhs.second.empty()) {
          XLOG(WARNING) << "No route to nodeLabel " << std::to_string(topLabel)
                        << " of node " << nodeName;
          fb303::fbData->addStatValue(
              "decision.no_route_to_label", 1, fb303::COUNT);
          continue;
        }

        // Create nexthops with appropriate MplsAction (PHP and SWAP). Note
        // that all nexthops are valid for routing without loops. Fib is
        // responsible for installing these routes by making sure it programs
        // least cost nexthops first and of same action type (based on HW
        // limitations)
        labelToNode.erase(topLabel);
        labelToNode.emplace(
            topLabel,
            std::make_pair(
                adjDb.thisNodeName().value(),
                RibMplsEntry(
                    topLabel,
                    getNextHopsThrift(
                        myNodeName,
                        {{adjDb.thisNodeName().value(), area}},
                        false /* isV4 */,
                        metricNhs,
                        topLabel,
                        area,
                        linkState))));
      }
    }

    for (auto& [_, nodeToEntry] : labelToNode) {
      routeDb.addMplsRoute(std::move(nodeToEntry.second));
    }
  }

  //
  // Create MPLS routes for all of our adjacencies
  //
  if (enableAdjacencyLabels_) {
    for (const auto& [_, linkState] : areaLinkStates) {
      for (const auto& link : linkState.linksFromNode(myNodeName)) {
        const auto topLabel = link->getAdjLabelFromNode(myNodeName);
        // Top label is not set => Non-SR mode
        if (topLabel == 0) {
          continue;
        }
        // If mpls label is not valid then ignore it
        if (not isMplsLabelValid(topLabel)) {
          XLOG(ERR) << "Ignoring invalid adjacency label " << topLabel
                    << " of link " << link->directionalToString(myNodeName);
          fb303::fbData->addStatValue(
              "decision.skipped_mpls_route", 1, fb303::COUNT);
          continue;
        }

        routeDb.addMplsRoute(RibMplsEntry(
            topLabel,
            {createNextHop(
                link->getNhV6FromNode(myNodeName),
                link->getIfaceFromNode(myNodeName),
                link->getMetricFromNode(myNodeName),
                createMplsAction(thrift::MplsActionCode::PHP),
                link->getArea(),
                link->getOtherNodeName(myNodeName))}));
      }
    }
  }

  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  XLOG(INFO) << "Decision::buildRouteDb took " << deltaTime.count() << "ms.";
  fb303::fbData->addStatValue(
      "decision.route_build_ms", deltaTime.count(), fb303::AVG);
  return routeDb;
} // buildRouteDb

RouteSelectionResult
SpfSolver::selectBestRoutes(
    std::string const& myNodeName,
    folly::CIDRNetwork const& prefix,
    PrefixEntries& prefixEntries,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) {
  CHECK(prefixEntries.size()) << "No prefixes for best route selection";
  RouteSelectionResult ret;

  auto filteredPrefixes = filterDrainedNodes(prefixEntries, areaLinkStates);

  if (enableBestRouteSelection_) {
    // Perform best route selection based on metrics
    ret.allNodeAreas = selectRoutes(
        filteredPrefixes, thrift::RouteSelectionAlgorithm::SHORTEST_DISTANCE);
    ret.bestNodeArea = selectBestNodeArea(ret.allNodeAreas, myNodeName);
  } else {
    // If it is openr route, all nodes are considered as best nodes.
    // Except for drained
    for (auto const& [nodeAndArea, prefixEntry] : filteredPrefixes) {
      ret.allNodeAreas.emplace(nodeAndArea);
    }
    ret.bestNodeArea = *ret.allNodeAreas.begin();
  }

  if (isNodeDrained(ret.bestNodeArea, areaLinkStates)) {
    // Decision will change RibEntry's drain_metric to 1 if isBestNodeDrained is
    // true, when it creates routeDB. So nodes in other areas would know
    // that this forwarding path has a drained node when RibEntry is
    // redistributed
    ret.isBestNodeDrained = true;
  }

  return ret;
}

std::optional<int64_t>
SpfSolver::getMinNextHopThreshold(
    RouteSelectionResult nodes, PrefixEntries const& prefixEntries) {
  std::optional<int64_t> maxMinNexthopForPrefix = std::nullopt;
  for (const auto& nodeArea : nodes.allNodeAreas) {
    const auto& prefixEntry = prefixEntries.at(nodeArea);
    maxMinNexthopForPrefix = prefixEntry->minNexthop().has_value() &&
            (not maxMinNexthopForPrefix.has_value() ||
             prefixEntry->minNexthop().value() > maxMinNexthopForPrefix.value())
        ? prefixEntry->minNexthop().value()
        : maxMinNexthopForPrefix;
  }
  return maxMinNexthopForPrefix;
}

PrefixEntries
SpfSolver::filterDrainedNodes(
    PrefixEntries& prefixes,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) const {
  auto hardDrainFiltered = filterHardDrainedNodes(prefixes, areaLinkStates);
  return filterSoftDrainedNodes(hardDrainFiltered, areaLinkStates);
}

PrefixEntries
SpfSolver::filterSoftDrainedNodes(
    PrefixEntries& prefixes,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) const {
  int minVal = std::numeric_limits<int32_t>::max();
  PrefixEntries filteredEntries;
  for (auto& [nodeArea, metricsWrapper] : prefixes) {
    const auto& [node, area] = nodeArea;
    int softDrainValue = areaLinkStates.at(area).getNodeMetricIncrement(node);
    if (softDrainValue < minVal) {
      minVal = softDrainValue;
      filteredEntries.clear();
    }
    if (softDrainValue == minVal) {
      filteredEntries.emplace(nodeArea, metricsWrapper);
    }
  }
  return filteredEntries;
}

PrefixEntries
SpfSolver::filterHardDrainedNodes(
    PrefixEntries& prefixes,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) const {
  PrefixEntries filtered = folly::copy(prefixes);
  for (auto iter = filtered.cbegin(); iter != filtered.cend();) {
    const auto& [node, area] = iter->first;
    if (areaLinkStates.at(area).isNodeOverloaded(node)) {
      iter = filtered.erase(iter);
    } else {
      ++iter;
    }
  }
  // Erase hard-drained nodes as candidates, unless everything is hard-drained
  return filtered.empty() ? prefixes : filtered;
}

bool
SpfSolver::isNodeDrained(
    const NodeAndArea& nodeArea,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) const {
  const auto& [node, area] = nodeArea;
  const auto& linkState = areaLinkStates.at(area);
  return linkState.isNodeOverloaded(node) or
      linkState.getNodeMetricIncrement(node) != 0;
}

SpfSolver::SpfAreaResults
SpfSolver::selectBestPathsSpf(
    std::string const& myNodeName,
    folly::CIDRNetwork const& prefix,
    RouteSelectionResult const& routeSelectionResult,
    const std::string& area,
    const LinkState& linkState) {
  /*
   * [Next hop Calculation]
   *
   * This step will calcuate the NH set with metric:
   *
   *  current node(myNodeName) ->
   *  dst node(prefix originator selected inside `RouteSelectionResult`)
   *
   * NOTE: the returned result contains best metric along with NH set.
   */
  SpfAreaResults result;
  const BestNextHopMetrics nextHopsWithMetric = getNextHopsWithMetric(
      myNodeName, routeSelectionResult.allNodeAreas, linkState);

  // Populate the SPF result
  result.bestMetric = nextHopsWithMetric.first;
  if (nextHopsWithMetric.second.empty()) {
    XLOG(DBG3) << "No route to prefix "
               << folly::IPAddress::networkToString(prefix);
    fb303::fbData->addStatValue("decision.no_route_to_prefix", 1, fb303::COUNT);
    return result;
  }

  result.nextHops = getNextHopsThrift(
      myNodeName,
      routeSelectionResult.allNodeAreas,
      prefix.first.isV4(), /* isV4Prefix */
      nextHopsWithMetric,
      std::nullopt /* swapLabel */,
      area,
      linkState);

  return result;
}

std::unordered_set<thrift::NextHopThrift>
SpfSolver::selectBestPathsKsp2(
    const std::string& myNodeName,
    const folly::CIDRNetwork& prefix,
    RouteSelectionResult const& routeSelectionResult,
    PrefixEntries const& prefixEntries,
    thrift::PrefixForwardingType const& forwardingType,
    const std::string& area,
    const LinkState& linkState) {
  std::unordered_set<thrift::NextHopThrift> nextHops;
  std::vector<LinkState::Path> paths;

  // Sanity check for forwarding type
  if (forwardingType != thrift::PrefixForwardingType::SR_MPLS) {
    XLOG(ERR) << "Incompatible forwarding type "
              << apache ::thrift::util::enumNameSafe(forwardingType)
              << " for algorithm KSPF2_ED_ECMP of "
              << folly::IPAddress::networkToString(prefix);

    fb303::fbData->addStatValue(
        "decision.incompatible_forwarding_type", 1, fb303::COUNT);
    return nextHops;
  }

  // find shortest and sec shortest routes towards each node.
  for (const auto& [node, bestArea] : routeSelectionResult.allNodeAreas) {
    // if ourself is considered as ECMP nodes.
    if (node == myNodeName and bestArea == area) {
      continue;
    }
    for (auto const& path : linkState.getKthPaths(myNodeName, node, 1)) {
      paths.push_back(path);
    }
  }

  // when get to second shortes routes, we want to make sure the shortest
  // route is not part of second shortest route to avoid double spraying
  // issue
  size_t const firstPathsSize = paths.size();
  for (const auto& [node, bestArea] : routeSelectionResult.allNodeAreas) {
    if (area != bestArea) {
      continue;
    }
    for (auto const& secPath : linkState.getKthPaths(myNodeName, node, 2)) {
      bool add = true;
      for (size_t i = 0; i < firstPathsSize; ++i) {
        // this could happen for anycast VIPs.
        // for example, in a full mesh topology contains A, B and C. B and C
        // both annouce a prefix P. When A wants to talk to P, it's shortes
        // paths are A->B and A->C. And it is second shortest path is
        // A->B->C and A->C->B. In this case,  A->B->C containser A->B
        // already, so we want to avoid this.
        if (LinkState::pathAInPathB(paths[i], secPath)) {
          add = false;
          break;
        }
      }
      if (add) {
        paths.push_back(secPath);
      }
    }
  }

  if (paths.size() == 0) {
    return nextHops;
  }

  for (const auto& path : paths) {
    Metric cost = 0;
    std::list<int32_t> labels;
    std::vector<std::string> invalidNodes;
    auto nextNodeName = myNodeName;
    for (auto& link : path) {
      cost += link->getMetricFromNode(nextNodeName);
      nextNodeName = link->getOtherNodeName(nextNodeName);
      auto& adjDb = linkState.getAdjacencyDatabases().at(nextNodeName);
      labels.push_front(*adjDb.nodeLabel());
      if (not isMplsLabelValid(*adjDb.nodeLabel())) {
        invalidNodes.emplace_back(*adjDb.thisNodeName());
      }
    }
    // Ignore paths including nodes with invalid node labels.
    if (invalidNodes.size() > 0) {
      XLOG(WARNING) << fmt::format(
          "Ignore path for {} through [{}] because of invalid node label.",
          folly::IPAddress::networkToString(prefix),
          folly::join(", ", invalidNodes));
      continue;
    }
    labels.pop_back(); // Remove first node's label to respect PHP

    // Create nexthop
    CHECK_GE(path.size(), 1);
    auto const& firstLink = path.front();
    std::optional<thrift::MplsAction> mplsAction;
    if (labels.size()) {
      std::vector<int32_t> labelVec{labels.cbegin(), labels.cend()};
      mplsAction = createMplsAction(
          thrift::MplsActionCode::PUSH, std::nullopt, std::move(labelVec));
    }

    bool isV4Prefix = prefix.first.isV4();
    nextHops.emplace(createNextHop(
        isV4Prefix and not v4OverV6Nexthop_
            ? firstLink->getNhV4FromNode(myNodeName)
            : firstLink->getNhV6FromNode(myNodeName),
        firstLink->getIfaceFromNode(myNodeName),
        cost,
        mplsAction,
        firstLink->getArea(),
        firstLink->getOtherNodeName(myNodeName)));
  }

  return nextHops;
}

std::optional<RibUnicastEntry>
SpfSolver::addBestPaths(
    const std::string& myNodeName,
    const folly::CIDRNetwork& prefix,
    const RouteSelectionResult& routeSelectionResult,
    const PrefixEntries& prefixEntries,
    std::unordered_set<thrift::NextHopThrift>&& nextHops,
    const Metric shortestMetric,
    const bool localPrefixConsidered) {
  // Check if next-hop list is empty
  if (nextHops.empty()) {
    return std::nullopt;
  }

  // Apply min-nexthop requirements. Ignore the route from programming if
  // min-nexthop requirement is not met.
  auto minNextHop = getMinNextHopThreshold(routeSelectionResult, prefixEntries);
  if (minNextHop.has_value() and minNextHop.value() > nextHops.size()) {
    XLOG(WARNING) << "Ignore programming of route to "
                  << folly::IPAddress::networkToString(prefix)
                  << " because of min-nexthop requirement. "
                  << "Minimum required " << minNextHop.value() << ", got "
                  << nextHops.size();
    return std::nullopt;
  }

  auto entry =
      *(prefixEntries.at(routeSelectionResult.bestNodeArea)); // copy intended
  // We don't modify original prefixEntries (referenced from prefixState)
  // because they reflect the prefix entries we received from others.
  if (routeSelectionResult.isBestNodeDrained) {
    *entry.metrics()->drain_metric() = 1;
  }

  // Create RibUnicastEntry and add it the list
  return RibUnicastEntry(
      prefix,
      std::move(nextHops),
      std::move(entry),
      routeSelectionResult.bestNodeArea.second,
      false, /* doNotInstall */
      shortestMetric,
      std::nullopt, /* ucmp weight */
      localPrefixConsidered);
}

/*
 * This util function will return the pair of:
 *  - min metric from SRC to DST node;
 *  - a map of NH node and the shortest distance from NH -> DST node;
 *
 * ATTN: the metric in pair.first is DIFFERENT from metric inside pair.second!
 */
BestNextHopMetrics
SpfSolver::getNextHopsWithMetric(
    const std::string& myNodeName,
    const std::set<NodeAndArea>& dstNodeAreas,
    const LinkState& linkState) {
  // build up next hop nodes that are along a shortest path to the prefix
  std::unordered_map<
      std::string /* nextHopNodeName */,
      Metric /* the distance from the nexthop to the dest */>
      nextHopNodes;
  Metric shortestMetric = std::numeric_limits<Metric>::max();

  auto const& spfResult = linkState.getSpfResult(myNodeName);

  // find the set of the closest nodes to our destination
  std::unordered_set<std::string> minCostNodes;
  for (const auto& [dstNode, _] : dstNodeAreas) {
    auto it = spfResult.find(dstNode);
    if (it == spfResult.end()) {
      continue;
    }
    const auto nodeDistance = it->second.metric();
    if (shortestMetric >= nodeDistance) {
      if (shortestMetric > nodeDistance) {
        shortestMetric = nodeDistance;
        minCostNodes.clear();
      }
      minCostNodes.emplace(dstNode);
    }
  }

  // Add neighbors with shortest path to the prefix
  for (const auto& dstNode : minCostNodes) {
    for (const auto& nhName : spfResult.at(dstNode).nextHops()) {
      nextHopNodes[nhName] = shortestMetric -
          linkState.getMetricFromAToB(myNodeName, nhName).value();
    }
  }

  return std::make_pair(shortestMetric, nextHopNodes);
}

std::unordered_set<thrift::NextHopThrift>
SpfSolver::getNextHopsThrift(
    const std::string& myNodeName,
    const std::set<NodeAndArea>& dstNodeAreas,
    bool isV4,
    const BestNextHopMetrics& bestNextHopMetrics,
    std::optional<int32_t> swapLabel,
    const std::string& area,
    const LinkState& linkState) const {
  // Use reference to avoid copy
  const auto& nextHopNodes = bestNextHopMetrics.second;
  const auto& minMetric = bestNextHopMetrics.first;
  CHECK(not nextHopNodes.empty());

  std::unordered_set<thrift::NextHopThrift> nextHops;
  for (const auto& link : linkState.linksFromNode(myNodeName)) {
    const auto neighborNode = link->getOtherNodeName(myNodeName);
    const auto search = nextHopNodes.find(neighborNode);

    // Ignore overloaded links or nexthops
    if (search == nextHopNodes.end() or not link->isUp()) {
      continue;
    }

    // Ignore nexthops that are not shortest
    Metric distOverLink = link->getMetricFromNode(myNodeName) + search->second;
    if (distOverLink != minMetric) {
      continue;
    }

    // Create associated mpls action if swapLabel is provided
    std::optional<thrift::MplsAction> mplsAction;
    if (swapLabel.has_value()) {
      CHECK(not mplsAction.has_value());
      bool isNextHopAlsoDst = dstNodeAreas.count({neighborNode, area});
      mplsAction = createMplsAction(
          isNextHopAlsoDst ? thrift::MplsActionCode::PHP
                           : thrift::MplsActionCode::SWAP,
          isNextHopAlsoDst ? std::nullopt : swapLabel);
    }

    nextHops.emplace(createNextHop(
        isV4 and not v4OverV6Nexthop_ ? link->getNhV4FromNode(myNodeName)
                                      : link->getNhV6FromNode(myNodeName),
        link->getIfaceFromNode(myNodeName),
        distOverLink,
        mplsAction,
        link->getArea(),
        link->getOtherNodeName(myNodeName),
        0 /* ucmp weight */));
  }
  return nextHops;
}

thrift::RouteComputationRules
SpfSolver::getRouteComputationRules(
    const PrefixEntries& prefixEntries,
    const RouteSelectionResult& routeSelectionResult,
    const std::unordered_map<std::string, LinkState>& areaLinkStates) const {
  // Construct default route computation rules.
  //
  // 1. Best route selection = SHORTEST_DISTANCE
  // 2. forwarding algorithm and forwarding type based on PrefixEntry
  //    attributes
  // 3. Prepend label = None
  thrift::RouteComputationRules defaultRules;
  defaultRules.routeSelectionAlgo() =
      thrift::RouteSelectionAlgorithm::SHORTEST_DISTANCE;
  for (const auto& [areaId, _] : areaLinkStates) {
    const auto areaPathComputationRules = getPrefixForwardingTypeAndAlgorithm(
        areaId, prefixEntries, routeSelectionResult.allNodeAreas);
    if (not areaPathComputationRules) {
      // There are no best routes in this area
      continue;
    }

    thrift::AreaPathComputationRules areaRules;
    areaRules.forwardingType() = areaPathComputationRules->first;
    areaRules.forwardingAlgo() = areaPathComputationRules->second;
    defaultRules.areaPathComputationRules()->emplace(
        areaId, std::move(areaRules));
  }

  return defaultRules;
}

} // namespace openr
