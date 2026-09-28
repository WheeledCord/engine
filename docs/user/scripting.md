# Scripting

Scheme scripts call the engine through one checked binding table. A scripted entity is an ordinary entity class whose callbacks call script functions. A game can add calls of its own before opening a script frontend.

Use the [scripting README](../../gameplay/script/README.md) for setup, entity declaration, and custom binding examples. Keep new engine calls in `gameplay/script/script_api.def`; do not hand-write a binding outside that table.
