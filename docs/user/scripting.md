# Scripting

Scheme and Pawn share one checked binding table. A scripted entity is an ordinary entity class whose callbacks call script functions. A game can add calls of its own before opening a script frontend.

Use the [scripting README](../../gameplay/script/README.md) for language setup, entity declaration, Pawn include generation, and custom binding examples. Keep new engine calls in `gameplay/script/script_api.def`; do not hand-write one language's binding independently.
