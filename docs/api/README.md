# XML API reference

This directory uses a compact Godot-style XML format: one `<class>` file per public C-facing service. The public-header `/** ... */` blocks are canonical; the XML is generated output and must not be edited by hand. `manifest.json` selects the services to publish, `tools/generate_api_docs.py` generates XML, and `tools/check_api_docs.py` validates it.

Run:

```sh
make api-docs-update
make docs-check
```

Current reference-covered services:

- [EngineApplication](core/EngineApplication.xml)
- [CoreCamera2D](core/CoreCamera2D.xml)
- [Collision2DWorld](core/Collision2DWorld.xml)
- [CoreUvBake](core/CoreUvBake.xml)
- [CoreInput](core/CoreInput.xml)
- [CoreFile](core/CoreFile.xml)
- [CoreTexture](core/CoreTexture.xml)
- [CoreNetwork](core/CoreNetwork.xml)
- [CoreNetClock](core/CoreNetClock.xml)

New public services must gain a manifest entry and documented public-header declarations when they become supported public API.
