# pgnparser
An extraordinary fast PGN parser with mmap-based file loading for large files and from existing buffer.

For comparison, benched with Intel Core i5-9400F and [6a6e647fc054e285ae029cc5](https://tests.stockfishchess.org/tests/view/6a6e647fc054e285ae029cc5), ignore game statistics:

| Library | Speed | 
| ----- | ----- |
| chess-library | 319.27MiB/s |
| shakmaty | 944.7MB/s |
| pgnparser | 700MB/s |
| ripgrep | 1817MB/s |

with game stats on:

| Library | Speed |
| ---- | ---- |
| shakmaty | 466.9MB/s |
| pgnparser | 700MB/s |

with lichess on month 12/2015 (without stats):

| Library | Speed |
| ---- | ---- |
| shakmaty | 985MB/s |
| pgnparser | 421MB/s |
| chess-library | 275MB/s |
| ripgrep | 1586MB/s |

with stats:

| Library | Speed |
| ---- | ---- |
| shakmaty | 297.4MB/s |
| pgnparser | 421MB/s |

| Library | Benchmark code | patches for no-stats |
| ----| ----|----|
| chess-library | https://github.com/Disservin/chess-library/blob/master/benchmarks/pgn_benchmark.cpp | skipPgn(true) on startPgn |
| shakmaty | https://github.com/niklasf/shakmaty/blob/main/pgn-reader/examples/stats.rs | begin_tags incr games and break |
| pgnparser | examples/import_example2.cpp | comment overrides except onGameStart |
| ripgrep | Measure-Command { rg -c -a -F "Event" filename } | |
