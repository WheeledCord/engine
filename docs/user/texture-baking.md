# Bake a material into a texture

A material written as a shader is evaluated every frame, for every pixel it covers. When the
material does not actually change — grain, rust, paint, dirt — that is the same arithmetic producing
the same answer forever. Baking runs it once, into an image, and the game samples the image instead.

`core/uv_bake.h` does the baking. It needs a mesh with texture coordinates, and a fragment shader
that produces the surface colour.

## What baking needs from the shader

Split the material shader in two. The part that describes the *surface* — albedo, height, wear —
gets baked. The part that describes the *lighting* — the sun, point lights, fog, the view direction —
must stay in the live shader, because it depends on where the camera and the lights are, which the
bake cannot know.

The bake shader is the first half, ending in a write:

```glsl
#version 120
varying vec3 fragPosition;      // object space, from core/shaders/uv_unwrap.vs
varying vec3 fragNormal;
void main()
{
    vec3 albedo = /* whatever the material's surface is */;
    gl_FragColor = vec4(albedo, 1.0);
}
```

Pair it with `core/shaders/uv_unwrap.vs`. That vertex shader is what makes the bake work: it makes
the texture coordinate the clip position, so the mesh is drawn flat into its own layout, and passes
object position and normal across as varyings. A material written against object space evaluates
exactly as it does in the world — only where the result is written changes.

## Baking

```c
RenderTexture atlas = {0};
MakeRT(&atlas, 1024, 1024, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, false);
BeginTextureMode(atlas);
ClearBackground(BLANK);          // alpha zero marks what no triangle covered
EndTextureMode();

for (int i = 0; i < model.meshCount; i++)
{
    Material m = LoadMaterialDefault();
    m.shader = bakeShader[material[i]];
    // A material whose surface is itself sampled from an image binds it here, as when drawing.
    m.maps[MATERIAL_MAP_DIFFUSE].texture = inputTexture[material[i]];
    CoreBakeMeshUV(model.meshes[i], m, atlas, MatrixIdentity());
}

Image baked = LoadImageFromTexture(atlas.texture);
ImageFormat(&baked, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
CoreDilateUVSeams(&baked, 4);
CoreFillUVBackground(&baked);
ExportImage(baked, "assets/rifle_albedo.png");
```

Nothing is cleared between meshes, so several meshes sharing one layout accumulate into one image —
which is what a model with one material per part wants.

## Dilate, or you will see the seams

A triangle rasterized into UV space covers whole texels only in its interior. Along its edges, the
filtered lookups the renderer does at draw time reach past it, into texels no triangle ever wrote.
Unwritten texels read as whatever the target was cleared to, and appear as bright or dark cracks
along every UV seam on the model — worse at distance, as mipmaps pull in more of them.

`CoreDilateUVSeams` grows the written texels outward over the empty ones. Four rings covers ordinary
bilinear and a few mip levels. It averages every channel, including alpha, so a surface height stored
there continues smoothly across the padding instead of turning into a height cliff. Alpha zero marks
empty space; encode a covered texel whose payload is truly zero as the smallest nonzero value. Do the
dilation before exporting, not after loading.

A bounded number of rings is not enough once the texture is mipmapped. Every level averages the one
above it across the whole image, so whatever fraction of the sheet the islands do not cover is what
a distant surface ends up sampling — a layout that is a few percent covered goes black within two
levels, whatever the dilation. `CoreFillUVBackground` keeps growing until nothing empty is left, so
every level averages surface colour. Run it after the seam dilation and after anything that needs to
know what a triangle covered: it spreads colour everywhere, so alpha stops reporting coverage.

The better answer to a mostly-empty sheet is usually to stop making one. A model's regions share its
UV layout, so bake them all into one texture and bind that one texture to the model, the way a model
is textured anywhere else. One map per region means each map holds one region's islands and nothing
else.

## Pitfalls

- **Overlapping UVs bake twice.** Where two triangles occupy the same texels the last one drawn
  wins. This is fine and intended for mirrored parts that should look identical; it is wrong when
  the two parts were meant to differ. The bake does not unwrap, validate or repack — the model's
  layout is taken as given.
- **UVs outside 0..1** fall off the target and bake nothing. Check the model before blaming the bake.
- **Resolution is the target's size**, so it is decided per model, not per material. A small prop and
  a large one sharing a resolution spends the same texels on very different amounts of surface.
- **The result is unlit.** A baked texture that already contains a highlight will fight the lighting
  applied over it. Keep the view-dependent half in the live shader.
- Baking needs a rendering context, so it runs inside a window, not from a headless tool.

## Keep the relief: surface maps

A material usually derives two things from the same noise — its colour, and the small height
variation that makes light catch it unevenly. Baking only the colour bakes the surface flat, and the
shader still has to evaluate the noise for the relief, which was most of the cost.

Bake both. RGB carries the colour, alpha carries the height, and the normal is derived from the
height at draw time rather than stored:

```glsl
#version 120
#include "core/shaders/surface_map.glsl"
uniform sampler2D texture0;      // RGB colour, A height
varying vec2 fragTexCoord;
varying vec3 fragPosition;
varying vec3 fragNormal;
void main()
{
    vec3 albedo = texture2D(texture0, fragTexCoord).rgb;
    vec3 n = CoreSurfaceNormal(texture0, fragTexCoord, fragPosition, fragNormal, 0.05);
    /* light it with n */
}
```

`CoreSurfaceNormal` builds its own tangent frame from the derivatives of the position and the UV, so
a mesh with no tangent attribute — one that was never built for normal mapping — still takes one.

`#include` is not part of GLSL 120. `CoreLoadShaders` expands it before the driver sees the source,
reading the named file through the same data root as everything else, so shaders can share a
function instead of copying it. An included file is a fragment, not a shader, and carries no
`#version` line of its own.
