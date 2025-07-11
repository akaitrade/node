
/*
 * CREDITS ALT-LEDGER 2030 - DAG Engine C API
 * 
 * This file is auto-generated from Rust code.
 * Do not modify manually.
 */

#ifndef DAG_ENGINE_H
#define DAG_ENGINE_H

#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * Error codes for FFI interface
 */
typedef enum {
  Success = 0,
  StorageError = 1,
  SerializationError = 2,
  InvalidVertex = 3,
  ConsensusError = 4,
  ShardError = 5,
  NetworkError = 6,
  ConfigError = 7,
  ValidationError = 8,
  TimeoutError = 9,
  CryptoError = 10,
  FFIError = 11,
  IOError = 12,
  UnknownError = 99,
} DAGErrorCode;

/**
 * Opaque handle for DAG engine instances
 */
typedef void *DAGEngineHandle;

/**
 * C-compatible consensus configuration
 */
typedef struct {
  unsigned int min_validators;
  unsigned int max_validators;
  double bft_threshold;
  unsigned long round_timeout_ms;
  unsigned int max_finality_rounds;
} CConsensusConfig;

/**
 * C-compatible shard configuration
 */
typedef struct {
  unsigned int initial_shard_count;
  unsigned int max_shard_tps;
  unsigned int min_shard_tps;
  unsigned int max_shard_count;
  unsigned long rebalance_interval_secs;
} CShardConfig;

/**
 * C-compatible DAG engine configuration
 */
typedef struct {
  const char *storage_path;
  CConsensusConfig consensus_config;
  CShardConfig shard_config;
} CDAGEngineConfig;

/**
 * C-compatible transaction data
 */
typedef struct {
  uint8_t source[32];
  uint8_t target[32];
  unsigned long amount;
  unsigned int currency;
  unsigned long fee;
  unsigned long nonce;
  unsigned int user_data_len;
  const uint8_t *user_data;
} CTransactionData;

/**
 * C-compatible BLS signature
 */
typedef struct {
  uint8_t signature[48];
  uint8_t public_key[48];
} CBLSSignature;

/**
 * C-compatible DAG vertex
 */
typedef struct {
  uint8_t hash[32];
  uint8_t tx_hash[32];
  unsigned long logical_clock;
  unsigned int parent_count;
  const uint8_t (*parents)[32];
  unsigned int shard_id;
  CTransactionData transaction_data;
  CBLSSignature signature;
  unsigned long timestamp;
} CDAGVertex;

/**
 * C-compatible DAG statistics
 */
typedef struct {
  unsigned long total_vertices;
  unsigned int active_shards;
  double cache_hit_rate;
  unsigned long consensus_rounds;
} CDAGStatistics;

/**
 * Create new DAG engine instance
 */
DAGEngineHandle dag_engine_new(const CDAGEngineConfig *config);

/**
 * Destroy DAG engine instance
 */
void dag_engine_destroy(DAGEngineHandle handle);

/**
 * Insert vertex into DAG
 */
DAGErrorCode dag_engine_insert_vertex(DAGEngineHandle handle, const CDAGVertex *vertex);

/**
 * Get vertex by hash
 */
DAGErrorCode dag_engine_get_vertex(DAGEngineHandle handle,
                                   const uint8_t (*hash)[32],
                                   CDAGVertex *vertex_out);

/**
 * Get DAG statistics
 */
DAGErrorCode dag_engine_get_statistics(DAGEngineHandle handle, CDAGStatistics *stats_out);

/**
 * Get error message for error code
 */
const char *dag_error_message(DAGErrorCode error_code);

/**
 * Initialize the DAG engine library
 */
DAGErrorCode dag_engine_init(void);

/**
 * Cleanup the DAG engine library
 */
void dag_engine_cleanup(void);

#endif /* DAG_ENGINE_H */
