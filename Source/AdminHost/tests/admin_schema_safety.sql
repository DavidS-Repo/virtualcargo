\set ON_ERROR_STOP on
SET search_path = clippy, pg_catalog;
SET statement_timeout = '15s';
SET lock_timeout = '2s';

BEGIN;

DO $$
DECLARE
    sid text := 'admin-schema-test-' || md5(clock_timestamp()::text || random()::text);
    rid text := 'root-' || md5(random()::text);
    child1 text := 'child-' || md5(random()::text);
    child2 text := 'child-' || md5(clock_timestamp()::text || random()::text);
    cid text := 'change-' || md5(random()::text);
    qid text := 'quarantine-' || md5(random()::text);
    snap text := 'snapshot-' || md5(random()::text);
    lockid text := 'lock-' || md5(random()::text);
    nowms bigint := floor(extract(epoch from clock_timestamp()) * 1000)::bigint;
    tree jsonb;
    indexed integer;
BEGIN
    IF COALESCE((SELECT max(version) FROM schema_migrations), 0) < 11 THEN
        RAISE EXCEPTION 'schema version 11 or newer is required';
    END IF;

    INSERT INTO storage_containers(storage_id,provider_id,provider_key,display_name,capacity_slots,revision,created_ms,updated_ms)
    VALUES(sid,'admin-schema-test',sid,'Admin schema test',100,0,nowms,nowms);

    tree := jsonb_build_object(
        'item_id', rid,
        'class_name', 'SeaChest',
        'quantity', 1,
        'health', 1,
        'location', jsonb_build_object('kind','cargo'),
        'adapter', jsonb_build_object('id','dayz.state-v2','version',1),
        'state', '{}'::jsonb,
        'children', jsonb_build_array(
            jsonb_build_object(
                'item_id', child1,
                'class_name', 'M4A1',
                'quantity', 1,
                'health', 0.75,
                'location', jsonb_build_object('kind','cargo'),
                'adapter', jsonb_build_object('id','dayz.state-v2','version',1),
                'state', '{}'::jsonb,
                'children', jsonb_build_array(
                    jsonb_build_object(
                        'item_id', child2,
                        'class_name', 'Mag_STANAG_30Rnd',
                        'quantity', 30,
                        'health', 1,
                        'location', jsonb_build_object('kind','attachment'),
                        'adapter', jsonb_build_object('id','dayz.state-v2','version',1),
                        'state', '{}'::jsonb,
                        'children', '[]'::jsonb
                    )
                )
            )
        )
    );

    INSERT INTO cargo_roots(storage_id,root_item_id,class_name,quantity,health,state_json,tree_json,item_ids,node_count,created_ms)
    VALUES(sid,rid,'SeaChest',1,1,'{}'::jsonb,tree,jsonb_build_array(rid,child1,child2),3,nowms);

    SELECT count(*) INTO indexed
    FROM cargo_item_index
    WHERE storage_id=sid AND root_item_id=rid;
    IF indexed <> 3 THEN
        RAISE EXCEPTION 'cargo_item_index trigger expected 3 rows, found %', indexed;
    END IF;

    IF NOT EXISTS(
        SELECT 1 FROM cargo_item_index
        WHERE storage_id=sid AND root_item_id=rid AND item_id=child2
          AND parent_item_id=child1 AND depth=2 AND class_name='Mag_STANAG_30Rnd'
          AND adapter_id='dayz.state-v2' AND location_type='attachment'
    ) THEN
        RAISE EXCEPTION 'nested cargo_item_index row is wrong';
    END IF;

    INSERT INTO admin_container_locks(storage_id,lock_id,admin_session_id,lock_reason,created_ms,expires_ms)
    VALUES(sid,lockid,'schema-test','schema safety test',nowms,nowms+60000);

    INSERT INTO admin_change_sets(change_id,admin_session_id,windows_identity,action_type,storage_id,item_id,
                                  before_revision,after_revision,reason,request_id,status,created_ms)
    VALUES(cid,'schema-test','schema-test','quarantine_item',sid,child2,0,1,'schema safety test','schema-test-request','APPLIED',nowms);

    INSERT INTO admin_change_entries(change_id,storage_id,root_item_id,item_id,entry_kind,before_state,after_state)
    VALUES(cid,sid,rid,child2,'ITEM',tree,tree);

    INSERT INTO admin_quarantine(quarantine_id,change_id,storage_id,root_item_id,item_id,parent_item_id,parent_index,tree_json,reason,created_ms)
    VALUES(qid,cid,sid,rid,child2,child1,0,tree #> '{children,0,children,0}','schema safety test',nowms);

    INSERT INTO admin_storage_snapshots(snapshot_id,storage_id,revision,root_count,node_count,reason,admin_session_id,windows_identity,created_ms)
    VALUES(snap,sid,0,1,3,'schema safety test','schema-test','schema-test',nowms);

    INSERT INTO admin_snapshot_roots(snapshot_id,root_item_id,tree_json)
    VALUES(snap,rid,tree);

    INSERT INTO admin_audit_events(admin_session_id,windows_identity,action,target_type,target_id,result,reason,request_id,change_id,detail_json,created_ms)
    VALUES('schema-test','schema-test','schema_test','container',sid,'SUCCESS','schema safety test','schema-test-request',cid,'{}'::jsonb,nowms);

    INSERT INTO players(player_id,display_name,first_seen_ms,last_seen_ms,last_snapshot_ms,last_inventory_count)
    VALUES('player-schema-test','Schema Player',nowms,nowms,nowms,1);
    INSERT INTO player_aliases(player_id,display_name,first_seen_ms,last_seen_ms)
    VALUES('player-schema-test','Schema Player',nowms,nowms);
    INSERT INTO player_inventory_snapshots(snapshot_id,player_id,captured_ms,item_count,equipment_json,inventory_json)
    VALUES('player-snapshot-schema-test','player-schema-test',nowms,1,'{"hands":"M4A1"}'::jsonb,
           jsonb_build_array(jsonb_build_object('item_id','live:test:1','class_name','M4A1','quantity',1,'health',1,
             'location',jsonb_build_object('kind','cargo'),'adapter',jsonb_build_object('id','dayz.state-v2','version',1),
             'state','{}'::jsonb,'children','[]'::jsonb)));
    INSERT INTO player_item_index(snapshot_id,player_id,item_id,parent_item_id,depth,class_name,quantity,health,adapter_id,location_type)
    VALUES('player-snapshot-schema-test','player-schema-test','live:test:1',NULL,0,'M4A1',1,1,'dayz.state-v2','cargo');
    INSERT INTO player_events(player_id,event_type,detail_json,created_ms)
    VALUES('player-schema-test','SNAPSHOT','{}'::jsonb,nowms);
    INSERT INTO admin_player_commands(command_id,idempotency_key,player_id,action,payload_json,status,admin_session_id,windows_identity,request_id,reason,created_ms,expires_ms)
    VALUES('player-command-schema-test','player-command-schema-request','player-schema-test','REQUEST_SNAPSHOT','{}'::jsonb,'PENDING','schema-test','schema-test','player-command-schema-request','schema safety test',nowms,nowms+30000);
    INSERT INTO player_quarantine(quarantine_id,command_id,player_id,item_id,tree_json,created_ms)
    VALUES('player-command-schema-test','player-command-schema-test','player-schema-test','live:test:1',
           jsonb_build_object('item_id','live:test:1','class_name','M4A1','quantity',1,'health',1,'children','[]'::jsonb),nowms);

    IF NOT EXISTS(SELECT 1 FROM admin_container_locks WHERE storage_id=sid AND lock_id=lockid) OR
       NOT EXISTS(SELECT 1 FROM admin_change_sets WHERE change_id=cid AND status='APPLIED') OR
       NOT EXISTS(SELECT 1 FROM admin_quarantine WHERE quarantine_id=qid AND restored_ms IS NULL) OR
       NOT EXISTS(SELECT 1 FROM admin_storage_snapshots WHERE snapshot_id=snap) OR
       NOT EXISTS(SELECT 1 FROM admin_audit_events WHERE change_id=cid AND result='SUCCESS') OR
       NOT EXISTS(SELECT 1 FROM players WHERE player_id='player-schema-test') OR
       NOT EXISTS(SELECT 1 FROM player_item_index WHERE snapshot_id='player-snapshot-schema-test' AND item_id='live:test:1') OR
       NOT EXISTS(SELECT 1 FROM admin_player_commands WHERE command_id='player-command-schema-test' AND status='PENDING') OR
       NOT EXISTS(SELECT 1 FROM player_quarantine WHERE quarantine_id='player-command-schema-test' AND restored_ms IS NULL) THEN
        RAISE EXCEPTION 'one or more admin schema records could not be written/read';
    END IF;

    IF EXISTS(SELECT 1 FROM pg_roles WHERE rolname='clippy_virtual_cargo_admin_edit') THEN
        IF has_table_privilege('clippy_virtual_cargo_admin_edit','clippy.operations','UPDATE') OR
           has_table_privilege('clippy_virtual_cargo_admin_edit','clippy.cargo_sessions','UPDATE') OR
           has_table_privilege('clippy_virtual_cargo_admin_edit','clippy.cargo_migrations','UPDATE') OR
           has_table_privilege('clippy_virtual_cargo_admin_edit','clippy.admin_audit_events','DELETE') OR
           has_table_privilege('clippy_virtual_cargo_admin_edit','clippy.admin_player_commands','UPDATE') OR
           has_table_privilege('clippy_virtual_cargo_admin_edit','clippy.player_quarantine','UPDATE') THEN
            RAISE EXCEPTION 'admin edit role has a forbidden broad privilege';
        END IF;
        IF NOT has_column_privilege('clippy_virtual_cargo_admin_edit','clippy.storage_containers','revision','UPDATE') OR
           NOT has_table_privilege('clippy_virtual_cargo_admin_edit','clippy.cargo_roots','INSERT') OR
           NOT has_table_privilege('clippy_virtual_cargo_admin_edit','clippy.admin_change_sets','INSERT') OR
           NOT has_table_privilege('clippy_virtual_cargo_admin_edit','clippy.admin_player_commands','INSERT') THEN
            RAISE EXCEPTION 'admin edit role is missing an expected narrow privilege';
        END IF;
    END IF;
END
$$;

ROLLBACK;

\echo 'Clippy Admin schema v10 safety test passed. The transaction was rolled back.'
