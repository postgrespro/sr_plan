#!/usr/bin/env python3

import sys
import os
import tempfile
import contextlib
import shutil
import unittest
import subprocess

from testgres import get_new_node

sql_init = '''
CREATE TABLE test_table(test_attr1 int, test_attr2 int);
INSERT INTO test_table SELECT i, i + 1 FROM generate_series(1, 20) i;
'''

queries = [
    "SELECT * FROM test_table WHERE test_attr1 = _p(10);",
    "SELECT * FROM test_table WHERE test_attr1 = 10;",
    "SELECT * FROM test_table WHERE test_attr1 = 15;"
]

my_dir = os.path.dirname(os.path.abspath(__file__))
repo_dir = os.path.abspath(os.path.join(my_dir, '../'))
temp_dir = tempfile.mkdtemp()

upgrade_to = '1.2'
check_upgrade_from = ['rel_1.0', '1.1.0']

compilation = '''
make USE_PGXS=1 clean
make USE_PGXS=1 install
'''

dump_sql = '''
SELECT * FROM pg_extension WHERE extname = 'sr_plan';

SELECT pg_get_functiondef(objid)
FROM pg_catalog.pg_depend JOIN pg_proc ON pg_proc.oid = pg_depend.objid
WHERE refclassid = 'pg_catalog.pg_extension'::REGCLASS AND
          refobjid = (SELECT oid
                                  FROM pg_catalog.pg_extension
                                  WHERE extname = 'sr_plan') AND
          deptype = 'e'
ORDER BY objid::regprocedure::TEXT ASC;

\\d+ sr_plans
\\dy sr_plan_invalid_table
'''

@contextlib.contextmanager
def cwd(path):
    curdir = os.getcwd()
    os.chdir(path)

    try:
        yield
    finally:
        os.chdir(curdir)


def shell(cmd):
    subprocess.check_output(cmd, shell=True)


def copytree(src, dst):
    for item in os.listdir(src):
        s = os.path.join(src, item)
        d = os.path.join(dst, item)
        if os.path.isdir(s):
            shutil.copytree(s, d)
        else:
            shutil.copy2(s, d)


class Tests(unittest.TestCase):
    def start_node(self):
        node = get_new_node()
        node.init()
        node.append_conf("shared_preload_libraries='sr_plan'\n")
        node.start()
        node.psql('create extension sr_plan')
        node.psql(sql_init)

        return node

    def test_hash_consistency(self):
        ''' Test query hash consistency '''

        with self.start_node() as node:
            node.psql("set sr_plan.write_mode=on")
            node.psql("set sr_plan.log_usage=NOTICE")
            for q in queries:
                node.psql(q)

            node.psql("set sr_plan.write_mode=off")
            queries1 = node.psql('select query_hash from sr_plans')
            self.assertEqual(len(queries), 3)
            node.psql("delete from sr_plans")
            node.stop()

            node.start()
            node.psql("set sr_plan.write_mode=on")
            node.psql("set sr_plan.log_usage=NOTICE")
            for q in queries:
                node.psql(q)

            node.psql("set sr_plan.write_mode=off")
            queries2 = node.psql('select query_hash from sr_plans')
            node.stop()

            self.assertEqual(queries1, queries2)

    def test_update(self):
        copytree(repo_dir, temp_dir)
        dumps = []

        with cwd(temp_dir):
            for ver in check_upgrade_from:
                shell("git clean -fdx")
                shell("git reset --hard")
                shell("git checkout -q %s" % ver)
                shell(compilation)

                with self.start_node() as node:
                    node.stop()

                    shell("git clean -fdx")
                    shell("git checkout -q master")
                    shell(compilation)

                    node.start()
                    node.safe_psql("alter extension sr_plan update to '%s'" % upgrade_to)

                    p = subprocess.Popen(["psql", "postgres", "-p", str(node.port)],
                                            stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE)
                    dumps.append([ver, p.communicate(input=dump_sql.encode())[0].decode()])
                    node.stop()

            # now make clean install
            with self.start_node() as node:
                p = subprocess.Popen(["psql", "postgres", "-p", str(node.port)],
                                        stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE)
                dumped_objects_new = p.communicate(input=dump_sql.encode())[0].decode()


        self.assertEqual(len(dumps), len(check_upgrade_from))
        for ver, dump in dumps:
            self.assertEqual(dump, dumped_objects_new)


if __name__ == "__main__":
    if len(sys.argv) > 1:
        suite = unittest.TestLoader().loadTestsFromName(sys.argv[1],
                                            module=sys.modules[__name__])
    else:
        suite = unittest.TestLoader().loadTestsFromTestCase(Tests)

    unittest.TextTestRunner(verbosity=2, failfast=True).run(suite)
