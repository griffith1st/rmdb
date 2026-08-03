# RMDB

RMDB is an educational relational database implemented in C++17. It includes
page storage, a buffer pool, record management, SQL parsing and analysis,
query planning and execution, indexes, transactions, locking, and WAL recovery.

## Build the server

```bash
cmake -S . -B build
cmake --build build -j2
./build/bin/rmdb /tmp/rmdb-study-db
```

The server listens on TCP port `8765`.

## Build the client

```bash
cmake -S rmdb_client -B build-client
cmake --build build-client -j2
./build-client/rmdb_client -h 127.0.0.1 -p 8765
```

## Tests

```bash
./build/bin/unit_test
ctest --test-dir build --output-on-failure
```
