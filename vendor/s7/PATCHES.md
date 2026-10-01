# Local changes to the vendored s7

`s7.c` here is s7 11.9 (8-Sep-2026) with the one change below. Reapply it after every s7 update,
then run `make smoke`: its game checks run a game in three fresh processes and fail if a hash
table's iteration order differs between them.

## 1. Symbols hash by name, not by address

File `s7.c`, section "hash symbols", two functions:

- `hash_map_symbol` returns
  `raw_string_hash((const uint8_t *)symbol_name(key), symbol_name_length(key))`
  instead of `pointer_map(key)`.
- `hash_symbol` computes its bucket with
  `(is_symbol(key) ? hash_map_symbol(sc, table, key) : pointer_map(key))`
  instead of `pointer_map(key)`.

Why: s7 put a symbol key in the bucket its address picked, so a default table's iteration order
followed where the symbol was allocated. With address-space randomization that differs between
processes and machines. E2 saw 4 orders in 6 runs of one program; E5 saw 4 in 5 runs, and 1 in 5
with this patch (design proposal B5.4; experiments e2_s7_hash_order and e5_s7_determinism). A game
that iterates such a table would then diverge in a replay or between peers. Hashing by name
also covers keys built from symbols (`hash_map_pair` hashes each element through the table's map),
and `eqv?` tables, which use the default map.

Why the `is_symbol` test: `hash_symbol` is a default table's lookup while all its keys are
symbols, and `s7_hash_table_set` calls it with the next key whatever that key's type, before it
switches the table to `hash_equal`. E5's patch called `hash_map_symbol` on that key, which
crashed on the first non-symbol key added to a table of symbols (a thing, say). A non-symbol key
keeps the stock lookup.

Cost: `raw_string_hash` over the name instead of a pointer cast on each symbol-keyed lookup;
not measured, since no hot path uses symbol-keyed tables.

`eq?` tables are unchanged. They hash every key by address (`hash_map_eq`, `hash_eq`), so they
iterate in memory order; game code cannot make one (`gameplay/script/game_s7.c`, `NoEqTables`).

## Not changed: c-objects

B5.4 also proposed hashing c-objects by their value word "instead of by address". In 11.9 a
default table does not hash c-objects by address: `init_hash_maps` sets `hash_map_eq` only for
types from `T_OUTPUT_PORT` on, and `T_C_OBJECT` comes before it, so a c-object keeps `hash_map_nil`,
which returns the type number. Every c-object key lands in one bucket, in an order set only by
when each was added, which is the same in every process. Hashing by value word would also be wrong for types whose
`equal?` compares contents: the engine's map and grid views hold a separate `malloc`ed View per
value, so two `equal?` views would land in different buckets and a lookup would miss. So this
line is left as upstream has it. The cost is a linear scan of one bucket per c-object lookup.
