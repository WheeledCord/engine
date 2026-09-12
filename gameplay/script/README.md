# Scripting

Two languages, one binding table. The script-facing API is described once, as data, in
`script_api.def`: name, argument types, result type, and the C function behind it. A frontend is a
loop over that table plus the conversions its language needs — it names no engine function of its
own. Adding a call is one row, and every language gets it.

That is the whole point of the arrangement. Hand-written bindings for two languages would mean
writing and maintaining each call twice, and the node editor later would make it three times.

```
script_api.def   every call a script can make, one row each
script_api.c     the implementations, and the table built from the same rows
script.c         scripted entity classes and the callbacks the world sees
script_s7.c      Scheme: trampolines generated from the same rows, and a REPL
script_pawn.c    Pawn: the same rows as natives, and the declarations Pawn needs
```

## The table

```c
SCRIPT_BINDING(move_world, "move-world!", SCRIPT_NONE, "move along the world axes",
               (SCRIPT_NONE, SCRIPT_VECTOR2))
```

One macro emits both the C prototype and the table row, so an implementation cannot drift from what
the table says it is. Types are `none bool int float vector2 vector3 string entity`. Every call is
checked against its declaration — arity and types — before it runs, so no frontend has to trust what
a script passed it. The argument list is parenthesised and starts with `SCRIPT_NONE` only because
C99 cannot pass an empty one; the sentinel is dropped when the table is built.

## Scripted entities

A scripted class is an ordinary `EntityClass` whose Spawn, Think, Draw and Destroy call functions the
script named, so the world cannot tell it from a class written in C. Each one carries a transform,
where it was a step ago so drawing can interpolate, and the fields it declared. Those fields are
`EntityField` metadata like any other, which is what lets a scene file place and configure a scripted
entity without the script being involved:

```
entity "mover" {
    "position" "360 260"
    "speed" "220"
}
```

A scripted class brings its own size and alignment when it registers, like any other class, so a
project does not have to reserve room for classes that do not exist yet.

## Scheme

`projects/script_demo` is the authoring demo's Mover, moved into `mover.scm` with no C left that
knows what a mover is. The same file also answers the REPL: while the demo runs, an expression typed
at the terminal is evaluated against the world on screen, so `(spawn "mover" (vec 200 200))` puts
another one in front of you.

`define-entity`, `vec` and the other conveniences are Scheme written in the prelude, on top of the
`class-*` rows. The shape of a declaration is a language's business; the engine's side of it is
still only the table.

## Pawn

`mover.pwn` is the same entity again, and `script_demo --pawn` runs it. The frontend is the same
loop over the same table; what it adds is only what Pawn needs to be told:

- Names are spelled the way a Pawn identifier can be: `move-world!` is `move_world`, `alive?` is
  `alive`. One rule, applied to every row.
- Pawn has no compound values, so a vector argument arrives as its components side by side, and a
  vector answer comes back through reference parameters, since a native returns one cell.
- `engine.inc`, the declarations a Pawn script includes, is written from the table during the build
  by `script_demo --write-pawn-include`. It is generated rather than kept by hand for the same
  reason the table exists at all.

Cells are pinned to 32 bits so the VM agrees with what `pawncc` emits, which also makes a cell hold
an IEEE float. The assembly core is 32-bit x86 only; every other target runs the portable C VM, and
`PAWN_CORE=asm` on a machine it cannot serve stops the build rather than quietly ignoring the ask.

## Adding a call

Add the row. Both languages have it on the next build, and so will the node editor:

```
make -f Makefile.core run-script        # Scheme, with the REPL
make -f Makefile.core run-script-pawn   # the same entity in Pawn
```
