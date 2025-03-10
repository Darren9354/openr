/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace go openr.KvStore
namespace py openr.KvStore
namespace py3 openr.thrift
namespace lua openr.KvStore
namespace wiki Open_Routing.Thrift_APIs.KvStore

include "fb303/thrift/fb303_core.thrift"

/*
 * Events in OpenR initialization process.
 * Ref: https://openr.readthedocs.io/Protocol_Guide/Initialization_Process.html
 */
enum InitializationEvent {
  /**
   * Open/R initialization process starts.
   */
  INITIALIZING = 0,
  /**
   * Platform agent is ready to accept FIB route programming.
   */
  AGENT_CONFIGURED = 1,
  /**
   * All links configured locally has been discovered via Netllink.
   */
  LINK_DISCOVERED = 2,
  /**
   * All neighbor has been discovered.
   */
  NEIGHBOR_DISCOVERED = 3,
  /**
   * KvStore has completed initial full sync with initial peer set.
   */
  KVSTORE_SYNCED = 4,
  /**
   * Initial RIB computation based on link-state database has completed.
   */
  RIB_COMPUTED = 5,
  /**
   * Initial FIB programming based on the RIB computation has completed.
   */
  FIB_SYNCED = 6,
  /**
   * Initial prefix advertisement and redistribution has completed.
   */
  PREFIX_DB_SYNCED = 7,
  /**
   * Open/R initialization process has completed.
   */
  INITIALIZED = 8,
  /**
   * All peers(not necessarily neighbors) has been discovered and reported.
   * ATTN: multiple neighbors can lead to single peer, aka, parallel
   * adjacencies. PEER_DISCOVERED happens after NEIGHBOR_DISCOVERED.
   */
  PEERS_DISCOVERED = 9,
  /**
   * ErrorCode: failures happen during the peer discovery process.
   */
  PEER_DISCOVERY_ERROR = 11,
  /**
   * ErrorCode: failures happen during initial KvStore sync process.
   */
  KVSTORE_SYNC_ERROR = 12,
  /**
   * Non-blocking: Initial Vlan state received from FSDB.
   */
  FSDB_SUBSCRIBED = 13,
}

exception KvStoreError {
  1: string message;
} (message = "message")

/**
 * `V` of `KV` Store. It encompasses the data that needs to be synchronized
 * along with few attributes that helps ensure eventual consistency.
 *
 * NOTE: Version 0 is undefined - Treat the struct as uninitialized.
 * Please set version > 0 for valid input.
 */
struct Value {
  /**
   * Current version of this value. Higher version value replaces the lower one.
   * Applications updating the data of an existing KV will always bump up the
   * version.
   *
   * 1st tie breaker - Prefer higher
   */
  1: i64 version;

  /**
   * The node that originate this Value. Higher value replaces the lower one if
   * (version) is same.
   *
   * 2nd tie breaker - Prefer higher
   */
  3: string originatorId;

  /**
   * Application data. This is opaque to KvStore itself. It is upto the
   * applications to define encoding/decoding of data. Within Open/R, we uses
   * thrift structs to avoid burden of encoding/decoding.
   *
   * 3rd tie breaker - Prefer higher
   *
   * KV update with no application data is considered as TTL update. See below
   * for TTL and TTL version.
   */
  2: optional binary value;

  /**
   * TTL in milliseconds associated with this Value. Originator sets the value.
   * An associated timer if fired will purge the value, if there is no ttl
   * update received.
   */
  4: i64 ttl;

  /**
   * Current version of the TTL. KV update with same (version, originator) but
   * higher ttl-version will reset the associated TTL timer to the new TTL value
   * in the update. Should be reset to 0 when the version increments.
   */
  5: i64 ttlVersion = 0;

  /**
   * Hash associated with `tuple<version, originatorId, value>`. Clients
   * should leave it empty and as will be computed by KvStore on `KEY_SET`
   * operation.
   */
  6: optional i64 hash;
} (cpp.minimize_padding)

/**
 * Map of key to value. This is a representation of KvStore data-base. Using
 * `std::unordered_map` in C++ for efficient lookups.
 */
typedef map<string, Value> (
  cpp.type = "std::unordered_map<std::string, openr::thrift::Value>",
) KeyVals

/*
 * The struct KvStoreNoMergeReasonStats contains the statistics of reasons why
 * the incoming kvs are not merged
 */
enum KvStoreNoMergeReason {
  UNKNOWN = 0,
  NO_MATCHED_KEY = 1,
  INVALID_TTL = 2,
  OLD_VERSION = 3,
  NO_NEED_TO_UPDATE = 4,
  LOOP_DETECTED = 5,
  INCONSISTENCY_DETECTED = 6,
}

struct KvStoreUpdateStats {
  1: i64 ttlUpdateCnt = 0;
  2: i64 valUpdateCnt = 0;
}

typedef map<string, KvStoreNoMergeReason> (
  cpp.type = "std::unordered_map<std::string, openr::thrift::KvStoreNoMergeReason>",
) NoMergeMap

struct KvStoreNoMergeReasonStats {
  // per-key reasons
  1: NoMergeMap noMergeReasons;

  // per-reason stats
  // the incoming key does not match the filtered keys
  2: i64 numberOfNoMatchedKeys = 0;
  // the ttl of the incoming kv is invalid
  3: list<i64> listInvalidTtls;
  // the incoming kv has an invalid or old version
  4: list<i64> listOldVersions;
  // the kv does not need to be merged
  5: i64 numberOfNoNeedToUpdates = 0;
  // detected that sender is stale and need to full sync
  6: bool inconsistencyDetetectedWithOriginator = false;
}

struct SetKeyValsResult {
  1: NoMergeMap noMergeReasons; // check empty or not
}

struct KvStoreMergeStats {
  1: KvStoreUpdateStats updateStats;
  2: KvStoreNoMergeReasonStats noMergeStats;
}

/**
 * Logical operator enum for querying
 */
enum FilterOperator {
  OR = 1,
  AND = 2,
}

/**
 * Request object for setting keys in KvStore.
 */
struct KeySetParams {
  /**
   * Entries, aka list of Key-Value, that are requested to be updated in a
   * KvStore instance.
   */
  2: KeyVals keyVals;

  /**
   * Optional attributes. List of nodes through which this publication has
   * traversed. Client shouldn't worry about this attribute. It is updated and
   * used by KvStore for avoiding flooding loops.
   */
  5: optional list<string> nodeIds;

  /**
   * Optional attribute to indicate timestamp when request is sent. This is
   * system timestamp in milliseconds since epoch
   */
  7: optional i64 timestamp_ms;

  /**
   * ID representing sender of the request.
   */
  8: optional string senderId;
} (cpp.minimize_padding)

/**
 * Request object for retrieving specific keys from KvStore
 */
struct KeyGetParams {
  1: list<string> keys;
}

/**
 * Request object for retrieving KvStore entries or subscribing KvStore updates.
 * This is more powerful version than KeyGetParams.
 */
struct KeyDumpParams {
  /**
   * This is deprecated in favor of `keys` attribute
   */
  1: string prefix (deprecated);

  /**
   * Set of originator IDs to filter on
   */

  3: set<string> originatorIds;

  /**
   * If set to true (default), ignore TTL updates. This is applicable for
   * subscriptions (aka streaming KvStore updates).
   */
  6: bool ignoreTtl = true;

  /**
   * If set to true, data attribute (`value.value`) will be removed from
   * from response. This would greatly reduces the data that need to be sent to
   * client.
   */
  7: bool doNotPublishValue = false;

  /**
   * Optional attribute to include keyValHashes information from peer.
   * 1) If NOT empty, ONLY respond with keyVals on which hash differs;
   *  2) Otherwise, respond with flooding element to signal DB change;
   */
  2: optional KeyVals keyValHashes;

  /**
   * The default is OR for dumping KV store entries for backward compatibility.
   * The default will be changed to AND later. We can also make `oper`
   * mandatory later. The default for subscription is AND now.
   */
  4: optional FilterOperator oper;

  /**
   * Keys to subscribe to in KV store so that consumers receive only certain
   * kinds of updates. For example, a consumer might be interesred in
   * getting "adj:.*" keys from open/r domain.
   */
  5: optional list<string> keys;

  /**
   * ID representing sender of the request.
   */
  8: optional string senderId;
} (cpp.minimize_padding)

/**
 * Define KvStorePeerState to maintain peer's state transition
 * during peer coming UP/DOWN for initial sync.
 */
enum KvStorePeerState {
  IDLE = 0,
  SYNCING = 1,
  INITIALIZED = 2,
}

/**
 * Peer's publication and command socket URLs
 * This is used in peer add requests and in
 * the dump results
 */
struct PeerSpec {
  /**
   * Peer address over thrift for KvStore external sync
   */
  1: string peerAddr;

  /**
   * cmd url for KvStore external sync over ZMQ
   */
  2: string cmdUrl (deprecated);

  /**
   * thrift port
   */
  4: i32 ctrlPort = 0;

  /**
   * State of KvStore peering
   */
  5: KvStorePeerState state;
}

/**
 * Unordered map for efficiency for peer to peer-spec
 */
typedef map<string, PeerSpec> (
  cpp.type = "std::unordered_map<std::string, openr::thrift::PeerSpec>",
) PeersMap

/**
 * KvStore Response specification. This is also used to respond to GET requests
 */
struct Publication {
  /**
   * KvStore entries
   */
  2: KeyVals keyVals;

  /**
   * List of expired keys. This is applicable for KvStore subscriptions and
   * flooding.
   * TODO: Expose more detailed information `expiredKeyVals` so that subscribers
   * can act on the values as well. e.g. in Decision/PrefixManager we no longer
   * need to rely on the key name to decode prefix/area/node and can use more
   * compact key formatting.
   */
  3: list<string> expiredKeys;

  /**
   * Optional attributes. List of nodes through which this publication has
   * traversed. Client shouldn't worry about this attribute.
   */
  4: optional list<string> nodeIds;

  /**
   * a list of keys that needs to be updated
   * this is only used for full-sync respone to tell full-sync initiator to
   * send back keyVals that need to be updated
   */
  5: optional list<string> tobeUpdatedKeys;

  /**
   * KvStore Area to which this publication belongs
   */
  7: string area;

  /**
   * Optional timestamp when publication is sent. This is system timestamp
   * in milliseconds since epoch
   */
  8: optional i64 timestamp_ms;
} (cpp.minimize_padding)

/**
 * Struct summarizing KvStoreDB for a given area. This is currently used for
 * sending responses to 'breeze kvstore summary'
 */

struct KvStoreAreaSummary {
  /**
   * KvStore area for this summary
   */
  1: string area;

  /**
   * Map of peer Names to peerSpec for all peers in this area
   */
  2: PeersMap peersMap;

  /**
   * Total # of Key Value pairs in KvStoreDB in this area
   */
  3: i32 keyValsCount;

  /**
   * Total size in bytes of KvStoreDB for this area
   */
  4: i32 keyValsBytes;
} (cpp.minimize_padding)

struct KvStoreFloodRate {
  1: i32 flood_msg_per_sec;
  2: i32 flood_msg_burst_size;
}

/**
 * KvStoreConfig is the centralized place to configure
 */
struct KvStoreConfig {
  /**
   * Set the TTL (in ms) of a key in the KvStore. For larger networks where
   * burst of updates can be high having high value makes sense. For smaller
   * networks where burst of updates are low, having low value makes more sense.
   */
  1: i32 key_ttl_ms = 300000;

  /**
   * Set node_name attribute to uniquely differentiate KvStore instances.
   *
   * ATTN: the behavior of multiple nodes sharing SAME node_name is NOT defined.
   */
  2: string node_name;

  3: i32 ttl_decrement_ms = 1;

  4: optional KvStoreFloodRate flood_rate;

  /**
   * Sometimes a node maybe a leaf node and have only one path in to network.
   * This node does not require to keep track of the entire topology. In this
   * case, it may be useful to optimize memory by reducing the amount of
   * key/vals tracked by the node. Setting this flag enables key prefix filters
   * defined by key_prefix_filters. A node only tracks keys in kvstore that
   * matches one of the prefixes in key_prefix_filters.
   */
  5: optional bool set_leaf_node;

  /**
   * This comma separated string is used to set the key prefixes when key prefix
   * filter is enabled (See set_leaf_node). It is also set when requesting KEY_DUMP
   * from peer to request keys that match one of these prefixes.
   */
  6: optional list<string> key_prefix_filters;
  7: optional list<string> key_originator_id_filters;

  /**
   * Mark control plane traffic with specified IP-TOS value.
   * Valid range (0, 256) for making.
   * Set this to 0 if you don't want to mark packets.
   */
  10: optional i32 ip_tos;

  // For tls thrift encryption
  11: optional string x509_cert_path;
  12: optional string x509_key_path;
  13: optional string x509_ca_path;
  /** Knob to enable/disable TLS thrift client. */
  14: bool enable_secure_thrift_client = false;
} (cpp.minimize_padding)

/**
 * Thrift service - exposes RPC APIs for interaction with KvStore module.
 */
service KvStoreService extends fb303_core.BaseService {
  /**
   * Get specific key-values from KvStore. If `filterKeys` is empty then no
   * keys will be returned
   */
  Publication getKvStoreKeyVals(1: list<string> filterKeys) throws (
    1: KvStoreError error,
  );

  /**
   * with area option
   */
  Publication getKvStoreKeyValsArea(
    1: list<string> filterKeys,
    2: string area,
  ) throws (1: KvStoreError error);

  /**
   * Get raw key-values from KvStore with more control over filter
   */
  Publication getKvStoreKeyValsFiltered(1: KeyDumpParams filter) throws (
    1: KvStoreError error,
  );

  /**
   * Get raw key-values from KvStore with more control over filter with 'area'
   * option
   */
  Publication getKvStoreKeyValsFilteredArea(
    1: KeyDumpParams filter,
    2: string area,
  ) throws (1: KvStoreError error);

  /**
   * Get kvstore metadata (no values) with filter
   */
  Publication getKvStoreHashFiltered(1: KeyDumpParams filter) throws (
    1: KvStoreError error,
  );

  /**
   * with area
   */
  Publication getKvStoreHashFilteredArea(
    1: KeyDumpParams filter,
    2: string area,
  ) throws (1: KvStoreError error);

  /**
   * Set/Update key-values in KvStore.
   */
  void setKvStoreKeyVals(1: KeySetParams setParams, 2: string area) throws (
    1: KvStoreError error,
  );

  /**
   * Get KvStore peers
   */
  PeersMap getKvStorePeers() throws (1: KvStoreError error);

  PeersMap getKvStorePeersArea(1: string area) throws (1: KvStoreError error);

  /**
   * Get KvStore Summary for each configured area (provided as the filter set).
   * The resp is a list of Summary structs, one for each area
   */
  list<KvStoreAreaSummary> getKvStoreAreaSummary(
    1: set<string> selectAreas,
  ) throws (1: KvStoreError error);
}

/**
 * Labels for initialization event time frames. If the duration of an event
 * is longer than expected, but not long enough to fail a check, it warrants a "warning".
 * If the duration is too long, it warrants a "timeout"
 *
 * Ex: If the duration of the LINK_DISCOVERED event is within [10000ms, 20000ms), it warrants a warning
 *     If the duration is >= 20000ms, it warrants a timeout
 */
enum InitializationEventTimeLabels {
  LINK_DISCOVERED_WARNING_MS = 1,
  LINK_DISCOVERED_TIMEOUT_MS = 2,
  NEIGHBOR_DISCOVERED_WARNING_MS = 3,
  NEIGHBOR_DISCOVERED_TIMEOUT_MS = 4,
  RIB_COMPUTED_WARNING_MS = 5,
  RIB_COMPUTED_TIMEOUT_MS = 6,
  KVSTORE_SYNCED_WARNING_MS = 7,
  KVSTORE_SYNCED_TIMEOUT_MS = 8,
  PREFIX_DB_SYNCED_WARNING_MS = 9,
  PREFIX_DB_SYNCED_TIMEOUT_MS = 10,
}

/**
 * Maps the labels to specific set times in ms
 */
const map<
  InitializationEventTimeLabels,
  i64
> InitializationEventTimeDuration = {
  LINK_DISCOVERED_WARNING_MS: 10000,
  LINK_DISCOVERED_TIMEOUT_MS: 20000,
  NEIGHBOR_DISCOVERED_WARNING_MS: 20000,
  NEIGHBOR_DISCOVERED_TIMEOUT_MS: 40000,
  RIB_COMPUTED_WARNING_MS: 150000,
  RIB_COMPUTED_TIMEOUT_MS: 300000,
  KVSTORE_SYNCED_WARNING_MS: 150000,
  KVSTORE_SYNCED_TIMEOUT_MS: 300000,
  PREFIX_DB_SYNCED_WARNING_MS: 150000,
  PREFIX_DB_SYNCED_TIMEOUT_MS: 300000,
};
