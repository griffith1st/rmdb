"""Aggregate result limits and index bounds through the real SQL server."""
import sys
import unittest

from sql_harness import Server

BINARY = sys.argv.pop(1) if len(sys.argv) > 1 else "build/bin/rmdb"


class QueryRegressionTest(unittest.TestCase):
    def setUp(self):
        self.server = Server(BINARY)
        self.addCleanup(self.server.close)
        self.client = self.server.start()

    def q(self, sql):
        return self.server.query(self.client, sql)

    def rows(self, sql):
        return self.server.rows(self.client, sql)

    def aggregates(self):
        self.q("create table t (id int, amount float);")
        for values in ("1,1.5", "2,2.5", "3,6.0"):
            self.q(f"insert into t values ({values});")

    def test_limit_one_keeps_all_aggregate_input(self):
        self.aggregates()
        self.assertEqual(self.rows("select COUNT(*) as n from t limit 1;"), [("3",)])
        self.assertEqual(self.rows("select SUM(id) as n from t limit 1;"), [("6",)])
        self.assertEqual(self.rows("select SUM(amount) as n from t limit 1;"), [("10.000000",)])
        self.assertEqual(self.rows("select MAX(id) as n from t limit 1;"), [("3",)])

    def test_limit_zero_suppresses_aggregate_result(self):
        self.aggregates()
        for aggregate in ("COUNT(*)", "SUM(id)", "MIN(id)"):
            with self.subTest(aggregate=aggregate):
                self.assertEqual(self.rows(f"select {aggregate} as n from t limit 0;"), [])

    def test_aggregate_limit_after_where_and_order(self):
        self.aggregates()
        self.assertEqual(self.rows(
            "select COUNT(*) as n from t where id>=2 order by id desc limit 1;"), [("2",)])
        self.assertEqual(self.rows(
            "select SUM(id) as n from t where id>=2 order by id desc limit 1;"), [("5",)])
        self.assertEqual(self.rows(
            "select COUNT(*) as n from t limit 2;"), [("3",)])
        self.assertEqual(self.rows("select SUM(id) as total from t limit 2;"), [("6",)])

    def test_empty_aggregate_and_plain_select_limits(self):
        self.aggregates()
        self.assertEqual(self.rows("select COUNT(*) as n from t where id>9 limit 1;"), [("0",)])
        self.assertEqual(self.rows("select COUNT(*) as n from t where id>9 limit 0;"), [])
        self.assertEqual(self.rows("select id from t order by id desc limit 1;"), [("3",)])
        self.assertEqual(self.rows("select id from t limit 0;"), [])

    def test_single_index_repeated_and_contradictory_bounds(self):
        self.q("create table t (a int);")
        for value in (-2147483648, -1, 0, 1, 2, 3, 4, 2147483647):
            self.q(f"insert into t values ({value});")
        cases = [
            ("a=3 and a>=3", [("3",)]),
            ("a>=3 and a=3", [("3",)]),
            ("a>=3 and a>3", [("4",), ("2147483647",)]),
            ("a>3 and a>=3", [("4",), ("2147483647",)]),
            ("a<=3 and a<3 and a>=1", [("1",), ("2",)]),
            ("a<3 and a<=3 and a>=1", [("1",), ("2",)]),
            ("a>3 and a<=3", []),
            ("a=3 and a=4", []),
            ("a>=-2147483648 and a<=-2147483648", [("-2147483648",)]),
        ]
        for indexed in (False, True):
            if indexed:
                self.q("create index t(a);")
            for predicate, expected in cases:
                with self.subTest(indexed=indexed, predicate=predicate):
                    self.assertEqual(self.rows(f"select a from t where {predicate} order by a;"), expected)

    def test_composite_index_prefix_and_range_bounds(self):
        self.q("create table t (a int, b int, c int);")
        for values in ("1,9,0", "2,-2147483648,1", "2,4,1", "2,5,1", "2,5,2", "2,6,1", "3,0,1"):
            self.q(f"insert into t values ({values});")
        cases = [
            ("a=2 and b>4 and b<=5", [("2", "5", "1"), ("2", "5", "2")]),
            ("b<=5 and a>=2 and a=2 and b>4", [("2", "5", "1"), ("2", "5", "2")]),
            ("a=2 and b=5 and b>=5 and c>1", [("2", "5", "2")]),
            ("a=2 and b>5 and b<=5", []),
            ("a>2 and a<2", []),
            ("b=5 and c=2", [("2", "5", "2")]),
            ("a=2 and b<>5 and c=1", [("2", "-2147483648", "1"), ("2", "4", "1"), ("2", "6", "1")]),
            ("a=2 and b>=-2147483648 and c>=1", [("2", "-2147483648", "1"), ("2", "4", "1"), ("2", "5", "1"), ("2", "5", "2"), ("2", "6", "1")]),
        ]
        for indexed in (False, True):
            if indexed:
                self.q("create index t(a,b,c);")
            for predicate, expected in cases:
                with self.subTest(indexed=indexed, predicate=predicate):
                    self.assertEqual(self.rows(f"select * from t where {predicate} order by a,b,c;"), expected)

    def test_composite_index_string_float_boundaries(self):
        self.q("create table t (name char(4), score float);")
        for values in ("'aa',-1.0", "'aa',0.0", "'aa',1.5", "'ab',-1.0", "'ab',2.0"):
            self.q(f"insert into t values ({values});")
        self.q("create index t(name,score);")
        cases = [
            ("name='aa' and score>=0.0 and score>0.0", [("aa", "1.500000")]),
            ("name='aa' and score>0.0 and score>=0.0", [("aa", "1.500000")]),
            ("name>='aa' and name<'ab' and score<=0.0", [("aa", "-1.000000"), ("aa", "0.000000")]),
            ("name='aa' and score>=1.5 and score<1.5", []),
        ]
        for predicate, expected in cases:
            with self.subTest(predicate=predicate):
                self.assertEqual(self.rows(f"select * from t where {predicate} order by name,score;"), expected)


if __name__ == "__main__":
    unittest.main(verbosity=2)
