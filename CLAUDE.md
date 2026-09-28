# Working on Trench Engine

## What it is

A general-purpose engine for 2D and 3D games, in C on raylib, targeting modest hardware (OpenGL 2.1,
GLSL 120). The engine does the heavy lifting — rendering, animation, collision, audio, input, saving,
networking — and games decide what happens. Games are meant to be written in Scheme (s7), with
scripting as easy as Godot's and powerful enough to build new features without writing C; C is the
engine's language and a game drops into it only when it truly needs to. Everything an engine should
provide is meant to be built in, with nothing picked or compiled in by hand and whatever a game does
not use staying idle. Simple, readable, easy to use, small enough for one person to understand.

It is built alongside real games (`../Games/`). When a game needs something an engine would normally
provide, it goes into the engine for every game to use. Game rules — stats, costs, turn structure,
genre conventions, game names — never go in the engine.

## How to work

- Before designing or changing a subsystem, read how the reference engines in `../references/`
  actually do it (the `reference-engines` skill maps subsystems to files) and cite file:line. Do not
  invent a design and present it as matching them. For s7 work, use the `s7-embedding` skill.
- Read the existing code and every caller before changing it. Never report status from memory.
- Every bug fix gets a regression check that fails before the fix. Every feature gets success and
  expected-failure coverage.
- `make smoke` must pass and the build must have zero warnings before a commit.
- Report "built", "tested" and "verified in play" separately. You cannot see the screen: never open
  screenshots or images. Anything that changes what a game draws is checked numerically with the
  `trench-shots` skill (pixel diff against a baseline), and is still unverified in play until the
  user has looked.
- Prefer the simplest thing that holds up. Fix forward; do not revert unless asked.
- Documentation changes with the code it describes, and never describes something unfinished as
  supported.
