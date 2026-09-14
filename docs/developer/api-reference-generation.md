# Generate API reference from public headers

Trench Engine keeps API documentation next to the C declarations it describes. Header comments are
the source of truth; XML under `docs/api/` is generated output used by documentation readers and
future site tooling.

## Document a function

Place a `/** ... */` block immediately above the declaration in a public header:

```c
/**
 * @brief Starts an application and owns its callback lifecycle.
 *
 * The descriptor and every object it borrows must remain valid until this function returns.
 * @param application Borrowed descriptor; NULL is rejected.
 * @return Zero after normal exit, or nonzero after startup or render failure.
 */
int EngineRunApplication(const EngineApplication *application);
```

`@brief`, every `@param`, and `@return` are required for functions published in the API manifest.
The first paragraph after a blank line becomes the detailed description. Put ownership, lifetime,
valid call order, capability requirements, and failure cases there; a C signature cannot express
those contracts.

Use one block for public structs, enums, and macros as they enter reference coverage. The current
generator publishes ordinary function declarations; extend it with a C parser before publishing
complex declarations such as function-pointer parameters rather than weakening the check.

## Publish a service

Add one entry to [`docs/api/manifest.json`](../api/manifest.json). It names the C-facing service,
the public header, generated XML location, and symbols that make up this reference page. Keep
services coherent: `EngineApplication` and `GameplayWorld` are examples.

Then run:

```sh
make api-docs-update
make docs-check
```

Commit the header comments, manifest change, and generated XML together. `docs-check` regenerates
in memory and fails if XML is stale, a published declaration has no adjacent doc block, a parameter
or return description is absent, or a documented symbol no longer exists.

`smoke` runs `docs-check`, so documentation drift blocks the normal engine verification path.

## Do not edit generated XML

Edit headers and the manifest, then regenerate. A manual XML change fails `docs-check` on the next
run. User guides in `docs/user/` remain hand-written because they explain workflows and examples,
not just signatures.
