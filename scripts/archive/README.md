# scripts/archive/

One-off scripts of closed cards that nothing else in the repository referenced. They were
moved here unedited on 2026-10-17 with `git mv`, so `git log --follow` keeps their history. The
list, with each script's own purpose line, is the last section of
[`scripts/README.md`](../README.md).

A script here is not maintained. Two things differ from its old place: a path computed from the
file's location (`ROOT` as the parent of `scripts/`) is one level short, and an import of a
sibling script from `scripts/` does not resolve. Edit those two before running one. The results
the scripts produced are recorded in the cards that used them, not here.
