\set ON_ERROR_STOP on
SET search_path = clippy, pg_catalog;
SET statement_timeout = '10s';
SET lock_timeout = '1s';
SET default_transaction_read_only = on;

BEGIN;
CREATE TEMP TABLE clippy_bench_params(
    item_id text,
    class_prefix text,
    warmups integer,
    iterations integer
) ON COMMIT DROP;
INSERT INTO clippy_bench_params VALUES (:'item_id', :'class_prefix', :warmups, :iterations);

DO $$
DECLARE
    exact_item text := (SELECT item_id FROM clippy_bench_params LIMIT 1);
    prefix text := (SELECT class_prefix FROM clippy_bench_params LIMIT 1);
    warmup_count integer := (SELECT warmups FROM clippy_bench_params LIMIT 1);
    iteration_count integer := (SELECT iterations FROM clippy_bench_params LIMIT 1);
    started timestamptz;
    elapsed_ms double precision;
    i integer;
BEGIN
    IF warmup_count < 0 OR warmup_count > 10000 THEN
        RAISE EXCEPTION 'warmups must be between 0 and 10000';
    END IF;
    IF iteration_count < 1 OR iteration_count > 10000 THEN
        RAISE EXCEPTION 'iterations must be between 1 and 10000';
    END IF;

    FOR i IN 1..warmup_count LOOP
        PERFORM 1 FROM (
            SELECT storage_id,root_item_id,item_id
            FROM cargo_item_index
            WHERE item_id=exact_item
            ORDER BY storage_id,root_item_id,item_id
            LIMIT 51
        ) q;
        PERFORM 1 FROM (
            SELECT storage_id,root_item_id,item_id
            FROM cargo_item_index
            WHERE lower(class_name) LIKE lower(prefix) || '%' ESCAPE E'\\'
            ORDER BY lower(class_name),storage_id,root_item_id,item_id
            LIMIT 51
        ) q;
    END LOOP;

    FOR i IN 1..iteration_count LOOP
        started := clock_timestamp();
        PERFORM 1 FROM (
            SELECT storage_id,root_item_id,item_id
            FROM cargo_item_index
            WHERE item_id=exact_item
            ORDER BY storage_id,root_item_id,item_id
            LIMIT 51
        ) q;
        elapsed_ms := extract(epoch FROM clock_timestamp()-started) * 1000.0;
        RAISE NOTICE 'CLIPPY_BENCH exact %', elapsed_ms;
    END LOOP;

    FOR i IN 1..iteration_count LOOP
        started := clock_timestamp();
        PERFORM 1 FROM (
            SELECT storage_id,root_item_id,item_id
            FROM cargo_item_index
            WHERE lower(class_name) LIKE lower(prefix) || '%' ESCAPE E'\\'
            ORDER BY lower(class_name),storage_id,root_item_id,item_id
            LIMIT 51
        ) q;
        elapsed_ms := extract(epoch FROM clock_timestamp()-started) * 1000.0;
        RAISE NOTICE 'CLIPPY_BENCH prefix %', elapsed_ms;
    END LOOP;
END
$$;
COMMIT;
