--source include/count_sessions.inc

CREATE TABLE t1 (pk INT, PRIMARY KEY (pk)) ENGINE=ROCKSDB;

SET rocksdb_bulk_load_allow_unsorted=1;
SET rocksdb_bulk_load=1;

INSERT INTO t1 VALUES (1);

--connect (con1,localhost,root,,)
error ER_LOCK_REFUSED_BY_ENGINE;
DROP TABLE t1;

--connection default
--disconnect con1

# This would have crashed the server prior to the fix
SET rocksdb_bulk_load=0;
SELECT * FROM t1;

DROP TABLE t1;
--error ER_NO_SUCH_TABLE
SELECT * FROM t1;

--source include/wait_until_count_sessions.inc
