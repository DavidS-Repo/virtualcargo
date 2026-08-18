\set ON_ERROR_STOP on
SET search_path = clippy, pg_catalog;
SET statement_timeout = '10s';
SET lock_timeout = '1s';
SET default_transaction_read_only = on;

\echo 'Clippy cargo_item_index benchmark'
\echo 'Schema and index state'
SELECT max(version) AS schema_version FROM schema_migrations;
SELECT complete, indexed_roots, updated_ms, last_error FROM cargo_item_index_state WHERE state_id=1;

\echo 'Exact item ID lookup'
EXPLAIN (ANALYZE, BUFFERS, SETTINGS)
SELECT storage_id,root_item_id,item_id,parent_item_id,depth,class_name,quantity,health,
       adapter_id,location_type,updated_ms
FROM cargo_item_index
WHERE item_id = :'item_id'
ORDER BY storage_id,root_item_id,item_id
LIMIT 51;

\echo 'Class prefix lookup'
EXPLAIN (ANALYZE, BUFFERS, SETTINGS)
SELECT storage_id,root_item_id,item_id,parent_item_id,depth,class_name,quantity,health,
       adapter_id,location_type,updated_ms
FROM cargo_item_index
WHERE lower(class_name) LIKE lower(:'class_prefix') || '%' ESCAPE E'\\'
ORDER BY lower(class_name),storage_id,root_item_id,item_id
LIMIT 51;

\echo 'Class prefix with quantity and health filters'
EXPLAIN (ANALYZE, BUFFERS, SETTINGS)
SELECT storage_id,root_item_id,item_id,parent_item_id,depth,class_name,quantity,health,
       adapter_id,location_type,updated_ms
FROM cargo_item_index
WHERE lower(class_name) LIKE lower(:'class_prefix') || '%' ESCAPE E'\\'
  AND quantity >= :min_quantity::double precision
  AND quantity <= :max_quantity::double precision
  AND health >= :min_health::double precision
  AND health <= :max_health::double precision
ORDER BY lower(class_name),storage_id,root_item_id,item_id
LIMIT 51;

\echo 'Root-class fallback prefix lookup'
EXPLAIN (ANALYZE, BUFFERS, SETTINGS)
SELECT storage_id,root_item_id,class_name,quantity,health,node_count,created_ms
FROM cargo_roots
WHERE lower(class_name) LIKE lower(:'class_prefix') || '%' ESCAPE E'\\'
ORDER BY storage_id,root_item_id
LIMIT 51;
