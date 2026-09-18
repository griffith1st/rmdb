"""SQL requirements and failure atomicity against the real server."""
import sys
import unittest

from sql_harness import Server

BINARY = sys.argv.pop(1) if len(sys.argv) > 1 else "build/bin/rmdb"


class SqlRegressionTest(unittest.TestCase):
    def setUp(self):
        self.server = Server(BINARY)
        self.addCleanup(self.server.close)
        self.client = self.server.start()

    def q(self, sql, **kwargs):
        return self.server.query(self.client, sql, **kwargs)

    def rows(self, sql):
        return self.server.rows(self.client, sql)

    def test_second_unique_index_failure_preserves_first_index(self):
        self.q("create table t (id int, code int);")
        self.q("insert into t values (1, 10);")
        self.q("insert into t values (2, 20);")
        self.q("create index t(id);")
        self.q("create index t(code);")
        self.q("update t set id=3, code=20 where id=1;", error=True)
        self.assertEqual(self.rows("select * from t where id=1;"), [("1", "10")])
        self.assertEqual(self.rows("select * from t where id=3;"), [])
        self.assertEqual(self.rows("select * from t where code=10;"), [("1", "10")])

    def test_multirow_unique_failure_rolls_back_earlier_rows(self):
        self.q("create table t (id int, code int);")
        self.q("insert into t values (1, 10);")
        self.q("insert into t values (2, 20);")
        self.q("create index t(code);")
        self.q("update t set code=30;", error=True)
        self.assertEqual(self.rows("select * from t order by id;"), [("1", "10"), ("2", "20")])

    def test_update_arithmetic_uses_original_row_values(self):
        self.q("create table t (id int, score int, other int, big bigint, f float);")
        self.q("insert into t values (1, 85, 3, 2147483648, 1.5);")
        self.q("update t set score=score+5, other=score-2, big=big + 3, f=f-0.5;")
        self.assertEqual(self.rows("select score,other,big,f from t;"),
                         [("90", "83", "2147483651", "1.000000")])

    def test_update_arithmetic_overflow_is_atomic(self):
        self.q("create table t (id int, score int);")
        self.q("insert into t values (1, 10);")
        self.q("insert into t values (2, 2147483647);")
        self.q("update t set score=score+1;", error=True)
        self.assertEqual(self.rows("select * from t order by id;"), [("1", "10"), ("2", "2147483647")])

    def test_unique_key_shift_and_rollback(self):
        self.q("create table t (id int);")
        for value in (2, 3, 4):
            self.q(f"insert into t values ({value});")
        self.q("create index t(id);")
        self.q("begin;")
        self.q("update t set id=id-1;")
        self.assertEqual(self.rows("select * from t order by id;"), [("1",), ("2",), ("3",)])
        self.q("abort;")
        self.assertEqual(self.rows("select * from t where id>=2 order by id;"), [("2",), ("3",), ("4",)])

    def test_bigint_datetime_and_validation(self):
        self.q("create table t (id bigint, stamp datetime);")
        self.q("insert into t values (-9223372036854775808, '2000-02-29 23:59:59');")
        self.q("insert into t values (9223372036854775807, '9999-12-31 23:59:59');")
        self.q("insert into t values (9223372036854775808, '2000-01-01 00:00:00');", error=True)
        self.q("insert into t values (1, '1900-02-29 00:00:00');", error=True)
        self.assertEqual(self.rows("select * from t order by id;"),
                         [("-9223372036854775808", "2000-02-29 23:59:59"),
                          ("9223372036854775807", "9999-12-31 23:59:59")])

    def test_join_sort_limit_aggregate_and_delete(self):
        self.q("create table a (id int, score int);")
        self.q("create table b (id int, label char(8));")
        for value in (1, 2, 3):
            self.q(f"insert into a values ({value}, {value * 10});")
            self.q(f"insert into b values ({value}, 'item{value}');")
        self.assertEqual(self.rows("select b.label,a.score from a,b where a.id=b.id order by a.score desc limit 2;"),
                         [("item3", "30"), ("item2", "20")])
        for expression, expected in (("count(*)", "3"), ("sum(score)", "60"),
                                     ("max(score)", "30"), ("min(score)", "10")):
            self.assertEqual(self.rows(f"select {expression} as result from a;"), [(expected,)])
        self.q("delete from a where id>=2;")
        self.assertEqual(self.rows("select * from a;"), [("1", "10")])

    def test_no_wait_locks_allow_readers_and_reject_writer(self):
        self.q("create table t (id int);")
        self.q("insert into t values (1);")
        self.q("begin;")
        self.assertEqual(self.rows("select * from t;"), [("1",)])
        second = self.server.connect()
        self.server.query(second, "begin;")
        self.assertEqual(self.server.rows(second, "select * from t;"), [("1",)])
        self.assertEqual(self.server.query(second, "insert into t values (2);", error=True).strip(), "abort")
        self.q("commit;")
        self.server.query(second, "insert into t values (2);")
        self.assertEqual(self.rows("select * from t order by id;"), [("1",), ("2",)])

    def test_show_index_reaches_client(self):
        self.q("create table t (id int);")
        self.q("create index t(id);")
        result = self.q("show index from t;")
        self.assertIn("unique", result)
        self.assertIn("id", result)

    def test_drop_and_recreate_table_survives_restart(self):
        self.q("create table t (id int);")
        self.q("insert into t values (7);")
        self.q("drop table t;")
        self.q("create table t (id int);")
        self.server.stop(crash=True)
        self.client = self.server.start()
        self.assertEqual(self.rows("select * from t;"), [])

    def test_ddl_cannot_destroy_another_transactions_table(self):
        self.q("create table t (id int);")
        self.q("insert into t values (1);")
        self.q("begin;")
        self.q("update t set id=2;")
        second = self.server.connect()
        self.server.query(second, "drop table t;", error=True)
        self.q("abort;")
        self.assertEqual(self.rows("select * from t;"), [("1",)])

    def test_ddl_in_explicit_transaction_is_rejected_without_losing_rows(self):
        self.q("create table t (id int);")
        self.q("insert into t values (1);")
        self.q("begin;")
        self.q("update t set id=2;")
        self.q("drop table t;", error=True)
        self.assertEqual(self.rows("select * from t;"), [("1",)])

    def test_out_of_range_limit_and_char_length_leave_server_usable(self):
        self.q("create table t (id int);")
        self.q("select * from t limit 9999999999999999999999999;", error=True)
        self.q("create table invalid (s char(99999999999999999999));", error=True)
        self.q("insert into t values (1);")
        self.assertEqual(self.rows("select * from t;"), [("1",)])


if __name__ == "__main__":
    unittest.main(verbosity=2)
