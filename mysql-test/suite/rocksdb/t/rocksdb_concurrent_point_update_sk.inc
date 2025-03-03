--source include/have_rocksdb.inc
--source include/have_debug_sync.inc
--source include/count_sessions.inc

--connect(con0, localhost, root,, test)
--connection default

--echo Conn A creating table
CREATE TABLE table1 (
  row_key BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, 
  val1 TINYINT NOT NULL,
  val2 VARCHAR(128) NOT NULL,
  PRIMARY KEY (row_key),
  KEY idx_val1 (val1)
) ENGINE=RocksDB;

# Conn A: first, create a table with a few rows, keeping the val1 column all the same
INSERT INTO table1 (val1, val2) VALUES (14, 'Alfa'), (14, 'Bravo'), (14, 'Charlie'), (14, 'Delta');
--echo Conn A: `table1` created with 4 rows

--echo Conn A: Table before
SELECT * FROM table1;

# next, start a transaction from Conn A to do a locking read for the rows based on a SK condition
# to be specific, select rows matching val1 = 14
# the update command should be `sent` after activating a DEBUG_SYNC point, so that the 
# update gets frozen. 

eval SET TRANSACTION ISOLATION LEVEL $isolation_level;
eval SET SESSION rocksdb_skip_snapshot_validation=$skip_snapshot_validation;
START TRANSACTION;
--echo Conn A: Started TRANSACTION A (SELECT .. FOR UPDATE )

set DEBUG_SYNC = "rocksdb_concurrent_upd_or_delete_sk SIGNAL waiting_for_update WAIT_FOR update_done";
--echo Conn A: activate DEBUG_SYNC point rocksdb_concurrent_upd_or_delete_sk 

send SELECT * from table1 FORCE INDEX(idx_val1) WHERE row_key = 1 AND val1 = 14 AND val2 = 'Alfa' FOR UPDATE;
--echo Conn A: Sent SELECT

# Meanwhile, start another transaction from Conn B to update val1 in such a way that
# the parallel transaction from Conn A hits the conflict
--connection con0

eval SET TRANSACTION ISOLATION LEVEL $isolation_level;
eval SET SESSION rocksdb_skip_snapshot_validation=$skip_snapshot_validation;
START TRANSACTION;
--echo Conn B: Started TRANSACTION B (Concurrent update)

--echo Conn B: Waiting for Conn A to hit `waiting_for_update`
set DEBUG_SYNC = "now WAIT_FOR waiting_for_update";
--echo Conn B: Conn A triggered `waiting_for_update`
UPDATE table1 SET val1 = 15 WHERE val1 = 14 AND val2 = 'Alfa';
SELECT * FROM table1;
COMMIT;
--echo Conn B: COMMIT for update done

# Unfreeze the Conn A update so that it can hit the conflict, and error out
set DEBUG_SYNC = "now SIGNAL update_done";
--echo Conn B: signalled Conn A with event `update_done`

# The followed reaped output should be empty due to KEY_NOT_FOUND being 
# returned back by the lookup function (due to concurrent update invalidating
# the search predicates

--echo Conn A: reaping SELECT * from table1 FORCE INDEX(idx_val1) WHERE row_key = 1 AND val1 = 14 AND val2 = 'Alfa';  
--connection default
if ($isolation_level == "REPEATABLE READ")
{
  if ($skip_snapshot_validation == 0)
  {
    --echo The SELECT output should see the snapshot conflict error
    --error ER_LOCK_DEADLOCK
    reap;
  }
  if ($skip_snapshot_validation == 1)
  {
   --echo The SELECT output should be empty
   reap;
  }
}
if ($isolation_level == "READ COMMITTED")
{
   --echo The SELECT output should be empty
   reap;
}
ROLLBACK;

--echo Conn A: Table after
SELECT * FROM table1;

DROP TABLE table1;

--disconnect con0

--source include/wait_until_count_sessions.inc
