# Public API and ownership

Every public symbol in `core/*.h`, `gameplay/*.h`, or `gameplay/script/*.h` is an engine contract. Before exposing or changing one, document:

- what it does and when it may be called;
- its parameters and return/failure behaviour;
- ownership and lifetime of every pointer, handle, or resource;
- required cleanup and whether partial initialization is safe;
- thread, rendering-context, and capability requirements; and
- important limitations or project-owned policy.

Use explicit `Init`/`Free` pairs where appropriate. Do not return storage that can be invalidated by a routine the caller is expected to invoke without documenting that invalidation. Public documentation belongs in the matching XML reference and in a user guide when the API changes an authoring workflow.
