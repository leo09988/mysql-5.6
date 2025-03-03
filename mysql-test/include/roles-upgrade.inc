--source include/mysql_upgrade_preparation.inc

--echo #
--echo # Bug#24447771 ACL_INIT() RETURNS TRUE ON 5.7 DATA DIRECTORY
--echo #
CALL mtr.add_suppression("ACL table mysql.role_edges missing. Some operations may fail.");
CALL mtr.add_suppression("ACL table mysql.default_roles missing. Some operations may fail.");

DROP TABLE mysql.role_edges;
DROP TABLE mysql.default_roles;

let server_log= $MYSQLTEST_VARDIR/log/roles-upgrade.err;
let restart_parameters=restart:--log-error=$server_log;
--replace_result $MYSQLTEST_VARDIR MYSQLTEST_VARDIR
--source include/restart_mysqld.inc

--echo # let's check for the presense of the warning
# $server_log has to be processed by include/search_pattern.inc which
# contains Perl code requiring that the environment variable SEARCH_FILE points
# to this file.
--let SEARCH_FILE= $server_log

--let SEARCH_PATTERN= ACL table mysql.role_edges missing. Some operations may fail.
--source include/search_pattern.inc

--let SEARCH_PATTERN= ACL table mysql.default_roles missing. Some operations may fail.
--source include/search_pattern.inc

--echo # ACL DDLs should not work
--error ER_NO_SUCH_TABLE
CREATE USER u1;

--error ER_NO_SUCH_TABLE
CREATE ROLE r1;

--error ER_NO_SUCH_TABLE
GRANT SELECT ON *.* TO u1;

--echo # Run mysql_upgrade
# Filter out ndb_binlog_index to mask differences due to running with or without
# ndb.
--let $restart_parameters = restart:--upgrade=FORCE --log-error=$server_log
--replace_result $MYSQLTEST_VARDIR MYSQLTEST_VARDIR
--let $wait_counter= 10000
--source include/restart_mysqld.inc

SHOW CREATE TABLE mysql.role_edges;
SHOW CREATE TABLE mysql.default_roles;

CREATE USER u1;
CREATE ROLE r1;
GRANT SELECT ON *.* TO r1;
GRANT r1 to u1;
CREATE TABLE test.t1(c1 int);
INSERT INTO test.t1 VALUES(1);

connect(conn_u1, localhost, u1,,);
SET ROLE r1;
SELECT * from t1;

connection default;
disconnect conn_u1;
DROP TABLE test.t1;
DROP ROLE r1;
DROP USER u1;

--source include/mysql_upgrade_cleanup.inc

# No restart needed since MTR will in any case restart the server when
# there is a restart in the test with additional mysqld options.
--remove_file $server_log

--echo
--echo End of tests

# Restore --log-error option
--let $restart_parameters = restart:
--source include/restart_mysqld.inc
