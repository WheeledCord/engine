# Documentation standard

Documentation serves two audiences:

- A **user guide** answers a task such as “create a scene” with prerequisites, a complete minimal example, variations, and pitfalls.
- An **API reference** states the exact public contract in XML. It is for lookup, not a tutorial.

Use one XML file per coherent C-facing service, modeled after Godot class-reference XML. The XML is generated, not hand-written: add a service to `docs/api/manifest.json`, then write a `/** ... */` block immediately above each exported declaration. Every block needs `@brief`, one `@param name` for every parameter, and `@return`; prose after a blank line becomes the detailed description. `tools/generate_api_docs.py` extracts the declarations and comments, while `tools/check_api_docs.py` validates the generated XML.

The complete command and tag guide is [Generate API reference from public headers](api-reference-generation.md). When adding a method to a reference-covered service, update its header comment in the same change and run `make api-docs-update`. When adding a new public service, add its manifest entry and a task-focused user guide before calling it supported. Run `make docs-check`.
