/****************************************************************************
 *   Copyright (c) 2021 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of the FreeCAD CAx development system.               *
 *                                                                          *
 *   This library is free software; you can redistribute it and/or          *
 *   modify it under the terms of the GNU Library General Public            *
 *   License as published by the Free Software Foundation; either           *
 *   version 2 of the License, or (at your option) any later version.       *
 *                                                                          *
 *   This library  is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *   GNU Library General Public License for more details.                   *
 *                                                                          *
 *   You should have received a copy of the GNU Library General Public      *
 *   License along with this library; see the file COPYING.LIB. If not,     *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,          *
 *   Suite 330, Boston, MA  02111-1307, USA                                 *
 *                                                                          *
 ****************************************************************************/

#ifndef RENDERER_RENDERER_H
#define RENDERER_RENDERER_H

#include <FCConfig.h>

#ifdef FreeCADRenderer_STATIC
#   define RendererExport
#elif defined(FreeCADRenderer_EXPORTS)
#   define RendererExport   FREECAD_DECL_EXPORT
#else
#   define RendererExport   FREECAD_DECL_IMPORT
#endif

#include "RenderConfig.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class QOpenGLWidget;
class QColor;

namespace Render {

class RenderLib;
class DrawDevice;
class DrawSurface;
/// A graphics device the renderer did not create (DeviceAdopt.h).
struct AdoptedDevice;

/// CPU-side snapshot of one geometry cache (SoFCVertexCache on the Gui side).
/// All array pointers stay valid for as long as `owner` is held. Backends key
/// GPU uploads on `cacheId`: the same id always refers to identical content.
struct MeshData {
    uint64_t cacheId = 0;
    std::shared_ptr<const void> owner;
    /// Opaque identity of the geometry that fed this mesh (the Gui
    /// bridge stores the shape node, proto node preferred so color
    /// variants share it). Never dereferenced by the renderer — it is
    /// the tag a shape-backed level generator was registered under
    /// (MeshSource.h), carried so the publisher can associate the
    /// mesh's content key with its source.
    const void *sourceTag = nullptr;
    /// What this mesh itself is on its ladder (§7 coarse-first): 0 =
    /// the exact tessellation, else the level error (relative to the
    /// shape diagonal) it was deliberately built coarse at — in which
    /// case the serializer declares the exact mesh as an unbuilt rung
    /// above it. Stamped by the bridge from the source registry.
    float levelError = 0.0f;

    /// Bumped every time the arrays behind this mesh are REPLACED in
    /// place — a level ladder's rungs all fill one mesh object under
    /// one cacheId (docs/SceneStreaming.md §7), and a release empties
    /// it the same way. A GPU cache keyed by cacheId must compare this
    /// before trusting its upload: "a cache id always refers to
    /// identical content" stopped being true the day meshes grew
    /// rungs — measured as exact geometry resident in every book while
    /// the screen kept drawing the coarse upload, with
    /// "glDrawElementsInstanced: Insufficient buffer size" where the
    /// refined index counts overran the stale buffers. Zero for
    /// bridge-built meshes, which really are immutable.
    uint32_t generation = 0;

    int numVertices = 0;
    const float *positions = nullptr;   ///< xyz per vertex, never null
    const float *normals = nullptr;     ///< xyz per vertex, may be null
    const uint8_t *colors = nullptr;    ///< rgba8 per vertex, may be null
    /// Bytes per vertex of the material stream below.
    static constexpr int MaterialStride = 12;

    /// Per-vertex material stream of a per-face-material cache, 12 bytes
    /// per vertex: rgba8 emissive, then rgb8 specular with the
    /// shininess (0..1) quantized in the last byte, then the surface
    /// finish palette index (Material::finishpalette) in one byte, the
    /// projection frame palette index (Material::framepalette) in the
    /// next, the per-face TEXTURE layer (Material::texturepalette, 0 =
    /// untextured) in the third and one reserved after it. Null for
    /// uniform objects (then the Material scalars apply). Draws consume
    /// it only when their material sets perfacematerial.
    const uint8_t *materials = nullptr;

    const int32_t *triangleIndices = nullptr;
    int numTriangleIndices = 0;
    const int32_t *lineIndices = nullptr;   ///< GL_LINES style vertex pairs
    int numLineIndices = 0;
    const int32_t *pointIndices = nullptr;
    int numPointIndices = 0;

    /// Line indices with seam lines (e.g. the closing edge of a cylinder
    /// face) filtered out; null when the cache has no seams. Used by the
    /// hidden-line draw style's hideSeam option.
    const int32_t *noSeamLineIndices = nullptr;
    int numNoSeamLineIndices = 0;

    /// Per-face-part {start, count} ranges into triangleIndices, filled
    /// for hidden-line outline materials: the GL renderer outlines
    /// geometry face part by face part under clip planes and in the
    /// perFaceOutline hidden-line mode. Empty when the cache carries no
    /// face part table (then those modes outline nothing, like GL).
    std::vector<std::pair<int, int>> triangleParts;
    /// The subset of triangleParts belonging to non-flat (curved) faces;
    /// the perFaceOutline mode outlines only these when combined with
    /// sceneOutline or a zero outline width (GL: getNonFlatParts()).
    std::vector<std::pair<int, int>> nonFlatParts;

    /// Per-edge-part {start, count} ranges into lineIndices, and per-vertex-
    /// part ranges into pointIndices (stride 1), filled for the whole-object
    /// line/point meshes so the browser viewer maps a hovered edge/vertex to
    /// its element (like triangleParts for faces). Empty when the cache
    /// carries no such part table.
    std::vector<std::pair<int, int>> lineParts;
    std::vector<std::pair<int, int>> pointParts;

    /// Solid-geometry knowledge for section capping (SoFCShapeInfo):
    /// 0 = none, 1 = some face parts belong to solids (solidParts holds
    /// their {start, count} ranges into triangleIndices), 2 = the whole
    /// triangle set is solid. Filled for clipped triangle materials.
    int hasSolid = 0;
    std::vector<std::pair<int, int>> solidParts;

    bool hasTransparency = false;   ///< some per-vertex colors are transparent
    bool hasOpaqueParts = false;    ///< some per-vertex colors are opaque

    /// Every element of this drawable is ATTACHED to higher-dimensional
    /// geometry of the same shape: every vertex of a point set is an
    /// edge endpoint, or every edge of a line set bounds a face
    /// (docs/SceneStreaming.md #13b). Such a drawable may be suppressed
    /// under memory pressure, because what makes it redundant is drawn
    /// anyway -- a vertex sits on an edge already on screen, an edge
    /// runs along a face silhouette already on screen.
    ///
    /// All or nothing per drawable, and false by default so anything
    /// the producer has not classified always draws. One floating
    /// element -- a point cloud's points, a wire, a sketch, a datum
    /// line -- makes the whole drawable unsuppressable, because nothing
    /// else on screen would show it.
    bool attachedOnly = false;

    /// Texture coordinates, xyzw per vertex (Coin's SbVec4f layout; the
    /// default texgen and 2D texcoord nodes produce (s, t, 0, 1)). Null
    /// when the cache was built without an active texture.
    const float *texCoords = nullptr;

    /// Screen-space offsets, xyzw per vertex (w unused): the vertex is
    /// drawn at its position PLUS xyz times the world length of one
    /// screen pixel at that vertex, xyz being in the mesh's own
    /// coordinates with a length in pixels. What lets geometry that
    /// GL sizes in pixels every frame -- a datum's arrowheads, the gap
    /// its line leaves for the number -- be captured once and still be
    /// right at every zoom and under any camera, the browser's
    /// included. Resolved by the backend against the view it draws
    /// with (scene-camera feeds only). Null for ordinary meshes.
    const float *screenOffsets = nullptr;

    /// Point markers (SoMarkerSet): the bitmap each point is drawn as
    /// in place of a square of the point size, as GL's glBitmap draws
    /// it -- a fixed pixel pattern centred on the projected point.
    /// \ref markers is the palette of distinct bitmaps the mesh uses,
    /// carried by content so that a consumer with no Coin (the browser)
    /// draws the same thing; \ref pointMarkers gives, per point index,
    /// the palette entry, or NoMarker for a point GL draws nothing for
    /// (SoMarkerSet::NONE). Null for ordinary point sets.
    struct PointMarker {
        uint16_t width = 0;
        uint16_t height = 0;
        /// One byte per pixel, nonzero = drawn; rows bottom-up, as GL
        /// bitmaps are.
        std::vector<uint8_t> mask;
    };
    static constexpr uint8_t NoMarker = 0xff;
    std::vector<PointMarker> markers;
    const uint8_t *pointMarkers = nullptr;
};

/// Flattened per-draw render state, translated from the Coin-side material
/// (SoFCRenderCache::Material). Colors are packed 0xRRGGBBAA.
struct Material {
    enum Type : uint8_t { Triangle, Line, Point };
    enum DepthFunc : uint8_t {
        Never, Always, Less, LEqual, Equal, GEqual, Greater, NotEqual
    };

    uint8_t type = Triangle;
    uint8_t depthfunc = LEqual;
    bool depthtest = true;
    bool depthwrite = true;
    bool pervertexcolor = false;
    /// Whole triangle draw of a cache whose mesh carries the per-face
    /// material stream (MeshData::materials), with the material arrays
    /// still authoritative (no scalar override on top): the backend
    /// shades emissive/specular/shininess from the stream instead of
    /// the scalars below. Never set on partial draws — those resolve
    /// their face's values into the scalars at translate time.
    bool perfacematerial = false;
    /// That stream's two alpha slots carry the PBR factor pair — the
    /// metallic where the emissive alpha is otherwise a constant 1, the
    /// roughness where the shininess sits — instead of a constant and
    /// the shininess (a per-face PBR appearance). Meaningful only with
    /// perfacematerial, and only the PBR shading branch reads them; the
    /// Phong branch shades a per-face PBR object from the same stream's
    /// colors, which are its Phong derivation.
    bool perfacepbr = false;
    bool lighting = true;        ///< false = flat base color (no light model)
    bool twoside = false;
    bool culling = false;
    bool ccw = true;             ///< front face vertex ordering
    bool transparent = false;    ///< uniform-color / texture transparency
    bool ontop = false;          ///< render after (over) the normal scene
    /// Where an on-top draw falls among the others. 0 is a plain
    /// annotation (SoAnnotation); anything else is the rendering order
    /// of the SoFCPathAnnotation priority the draw is under, which GL
    /// draws after every plain annotation, an order at a time, lowest
    /// first, and each whole -- fills and lines in traversal order (the
    /// late delayed paths of SoBoxSelectionRenderAction). It is what
    /// keeps a pattern's instance toggles over the dimension crossing
    /// them. Meaningful only with ontop.
    int16_t ontoplayer = 0;
    /// The draw is under an SoFCSelectionRoot whose style is Unpickable
    /// (SoFCRenderCache::Material::Unpickable): a click on it does not
    /// pick it on the desktop. No backend draws it differently; a remote
    /// viewer that picks for itself reads it -- a finger on a dress-up's
    /// see-through preview orbits, as a mouse press there goes past it.
    bool unpickable = false;
    bool polygonoffset = false;  ///< glPolygonOffset on filled triangles
    /// SoDrawStyleElement::Style as the render cache captured it. The GL
    /// renderer hands LINES/POINTS to glPolygonMode; no modern API has
    /// that state, so the backend draws the primitives instead — see
    /// BGFXView::submitTessellation. This is how the Tessellation draw
    /// style arrives (SoFCUnifiedSelection overrides the element to
    /// LINES for it), and also how a plain SoDrawStyle node in the scene
    /// graph asks for a wireframe -- which drawstyleoverride tells apart.
    enum DrawStyle : uint8_t {
        DrawFilled = 0, DrawLines = 1, DrawPoints = 2, DrawInvisible = 3
    };
    uint8_t drawstyle = DrawFilled;
    /// The draw style above arrived as a scene-wide OVERRIDE, which is
    /// what the Tessellation display mode is -- SoFCUnifiedSelection sets
    /// SoOverrideElement's DRAW_STYLE for it, the only place in the tree
    /// that does. That mode wants the faces filled in the background
    /// colour to occlude what is behind them (Coin gets the same from
    /// SoRenderManager::HIDDEN_LINE); a lone SoDrawStyle node asks for a
    /// wireframe and nothing more, and filling it hides whatever it was
    /// drawn around.
    bool drawstyleoverride = false;
    uint32_t diffuse = 0xCCCCCCFF;
    uint32_t emissive = 0;
    uint32_t specular = 0;
    uint32_t ambient = 0;
    /// Edge color override of a triangle material (hidden-line outline
    /// color falls back to diffuse when 0, like the GL renderer).
    uint32_t linecolor = 0;
    float shininess = 0.0f;
    float linewidth = 1.0f;
    float pointsize = 1.0f;
    /// Line stipple, glLineStipple encoding: low 16 bits = pattern (LSB
    /// drawn first), bits 16+ = pixel repeat factor (0/1 = one pixel per
    /// bit). 0xffff in the low bits = solid.
    uint32_t linepattern = 0xffff;
    /// Pattern of the depth-occluded (dimmed) pass of on-top lines; GL
    /// substitutes ViewParams::SelectionLinePattern there when the
    /// material has no pattern of its own.
    uint32_t hiddenlinepattern = 0xffff;
    /// glPolygonOffset(factor, units); positive pushes away from the viewer
    float polygonoffsetfactor = 0.0f;
    float polygonoffsetunits = 0.0f;
    /// Alpha used to dim the depth-occluded part of on-top lines/points
    /// (ViewParams::TransparencyOnTop); 1 = no dimming.
    float hiddenlinealpha = 1.0f;
    /// Line/point draw of a selection/preselection highlight (GL renders
    /// it under RenderPassHighlight): drawn after the uncolored on-top
    /// companion lines (GL bucket order selsontop -> selslineontop), and
    /// its depth-tested passes get a tiny toward-viewer bias so the
    /// thickened quad wins the depth tie against the object's own scene
    /// line the way GL's width-independent line rasterization does.
    bool highlightline = false;

    /// Stencil face outline of partial (per-face) triangle draws in the
    /// selection/highlight feeds (GL: RenderPassSelectionOutline): the
    /// face is drawn into the stencil buffer, then its triangle edges
    /// redraw as thick lines where the stencil does not match, leaving
    /// the boundary. The outline color is the material's emissive.
    bool faceoutline = false;   ///< outline partial triangle draws
    bool outlineonly = false;   ///< and skip their face fill
    float outlinewidth = 1.0f;  ///< outline width in pixels

    /// Hidden-line draw style material (SoFCRenderCache::Material::outline):
    /// whole-cache triangle draws get a stencil outline, and the active
    /// HiddenLineConfig's hideFace/hideSeam/hideVertex rules apply.
    bool outline = false;

    /// Shadow participation of a triangle draw, the Coin
    /// SoShadowStyleElement bitmask: 1 = casts shadows, 2 = receives
    /// (is shadowed). Only meaningful while a scene light is fed
    /// (LightConfig::valid).
    uint8_t shadowstyle = 3;

    /// Coin shape hints declare the geometry a closed solid
    /// (SoShapeHintsElement::SOLID); together with MeshData::hasSolid this
    /// gates the stencil section cap of clipped draws.
    bool solidshape = false;

    /// Per-object PBR parameters of a triangle draw (SoFCRenderMaterial,
    /// typically fed from ViewProvider Render_* properties); < 0 = unset,
    /// the frame's PBRConfig values apply. Only used while the PBR
    /// shading path is active.
    float metallic = -1.0f;
    float roughness = -1.0f;

    /// Machined surface finish of a triangle draw (App::SurfaceFinish,
    /// carried by SoFCRenderMaterial, either authored on the appearance
    /// or stated by the ViewProvider Render_Finish* properties): a
    /// procedural pattern the backend shades as a perturbed normal, and
    /// as added roughness once its features fall below the pixel
    /// footprint. finish is the App::SurfaceFinish::Pattern value
    /// (0 = none, and an unrecognized one shades as none); finishpitch
    /// (feature spacing) and finishdepth (peak to valley) are in
    /// millimetres of the draw's OBJECT space, so an instanced or scaled
    /// copy keeps the finish attached to its geometry; finishangle is
    /// the lay direction in degrees. Only meaningful while lighting is
    /// on: a pattern is a shading fact, so the depth, shadow and
    /// picking passes ignore it.
    uint8_t finish = 0;
    float finishpitch = 0.0f;
    float finishdepth = 0.0f;
    float finishangle = 0.0f;

    /// Per-face form of that finish (null = the scalars above are the
    /// whole story). A finish is four numbers, so a per-face one rides a
    /// PALETTE of the distinct finishes plus one index per vertex in the
    /// material stream's third slot — not four more per-face arrays.
    /// Entry 0 is what the scalars repeat, which is what an unbound index
    /// attribute (a mesh with no stream) resolves to. Meaningful on whole
    /// triangle draws only, and only together with perfacematerial: a
    /// partial draw resolves its face's entry into the scalars at
    /// translate time and carries no palette. Shared and immutable, so
    /// pointer identity is a batch key (like usershader).
    std::shared_ptr<const FinishPalette> finishpalette;

    /// The frame the finish above is laid out in (Unframed = triplanar).
    /// The draw's own, which is also what the backend uploads as palette
    /// entry 0 -- so a partial draw that resolved one face's frame here
    /// needs no palette, exactly as it needs none for the finish.
    SurfaceFrame frame;

    /// Per-face form of that frame (null = the frame above is the whole
    /// story), indexed by the second byte of the material stream's third
    /// slot. Same rules
    /// as finishpalette: whole triangle draws with perfacematerial only,
    /// shared and immutable, pointer identity is a batch key. Unlike the
    /// finish this comes from the GEOMETRY rather than the appearance,
    /// so it is published only when a finish is stated somewhere -- a
    /// shape nobody finished must not pay for a stream it cannot use.
    std::shared_ptr<const FramePalette> framepalette;

    /// Per-face texture palette of a triangle draw (null = the draw's
    /// faces carry no images of their own, and `texture` above is the
    /// whole texturing story).
    ///
    /// One draw binds one sampler, so per-face images cannot be one
    /// texture each: the palette's entries become the LAYERS of a single
    /// array texture and the material stream's third slot, third byte,
    /// names a face's layer -- 0 being the untextured face, entry i
    /// being layer i + 1 (TexturePalette). Sampled on top of everything
    /// `texture` does: a face's own image modulates the fragment colour
    /// after the unit-0 texture has, so a shape can carry both.
    ///
    /// Shared and immutable, so pointer identity is a batch key and the
    /// key the backend caches the uploaded array under.
    std::shared_ptr<const TexturePalette> texturepalette;

    /// Where the per-face images above are laid out, in millimetres of
    /// OBJECT space per tile -- the size the image is printed at, which
    /// is what a physical decal or a machined marking has, rather than
    /// a fraction of a bounding box that changes when the part does.
    ///
    /// The coordinates themselves come from the face's own projection
    /// frame (`framepalette`, the same frames the finish is laid out
    /// in): a planar face in the plane's own axes, a turned one
    /// unwrapped about its axis, and a face with no analytic surface
    /// triplanarly off the object-space normal.
    ///
    /// <= 0 means the mesh's own texture coordinates instead, which is
    /// what a shape that was really UV mapped wants. A mesh that carries
    /// none then reads (0, 0) -- the image's corner texel -- exactly as
    /// the unit-0 texture does in the same situation.
    float facetexscale = 0.0f;

    /// The layer every fragment of this draw samples, or < 0 to read the
    /// stream. A single-face draw (a selection or preselection highlight)
    /// has no material stream to read an index out of, so the producer
    /// resolves that one face's layer here -- the same way it resolves
    /// the face's finish and PBR pair into the scalars.
    int8_t facetexlayer = -1;

    /// Water body flag of a triangle draw (SoFCRenderMaterial, typically
    /// fed from a ViewProvider Render_Water property): while the
    /// volumetric lighting pass is active, the draw's closed volume
    /// becomes a scattering medium tinted by the diffuse color instead
    /// of an ordinary surface — it neither ends volumetric rays nor
    /// casts shadows, and its front/back depths bound the underwater
    /// stretch of each view ray. waterdensity is the extinction density
    /// in inverse world units, <= 0 = automatic (from the draw bounds).
    bool water = false;
    float waterdensity = 0.0f;

    /// Glass body flag of a triangle draw (SoFCRenderMaterial, typically
    /// fed from a ViewProvider Render_Glass property): the draw leaves
    /// the ordinary (transparent) path and renders with screen-space
    /// refraction, Fresnel-blended environment reflection and
    /// per-channel Beer-Lambert absorption tinted by the diffuse color
    /// over the body thickness (front/back depth interval). glassior
    /// <= 0 = default 1.5; glassdensity <= 0 = automatic (from the draw
    /// bounds); glassroughness in 0..1 blurs the reflection.
    bool glass = false;
    float glassior = 0.0f;
    float glassdensity = 0.0f;
    float glassroughness = 0.0f;
    /// The glass claim came from the draw's MaterialX surface, not from
    /// Render_Glass: the producer resolved the document's transmission
    /// into the four fields above (docs/MaterialStorage.md sec 17.21).
    /// Two readings change with it. The body colour is glasscolor --
    /// LINEAR, as the document states it, never the authored diffuse
    /// and never decoded -- and a glassdensity of 0 is not "automatic"
    /// but NONE: OpenPBR's transmission_depth 0 means the colour tints
    /// the transmitted light once at the surface instead of absorbing
    /// over the body.
    bool glassmtlx = false;
    float glasscolor[3] = {1.0f, 1.0f, 1.0f};

    /// Cloud body flag of a triangle draw (SoFCRenderMaterial, typically
    /// fed from a ViewProvider Render_Cloud property): while the
    /// volumetric lighting pass is active the draw's closed volume
    /// raymarches as a procedural-density (FBM) scattering medium and
    /// the geometry itself is not rendered. clouddensity/clouddetail
    /// <= 0 = automatic (from the draw bounds); cloudspeed scales the
    /// drift animation of the noise domain.
    bool cloud = false;
    float clouddensity = 0.0f;
    float clouddetail = 0.0f;
    float cloudspeed = 1.0f;

    /// Fire body flag of a triangle draw (SoFCRenderMaterial, typically
    /// fed from a ViewProvider Render_Fire property): while the
    /// volumetric lighting pass is active the draw's closed volume
    /// raymarches as an emissive flame medium (rising FBM noise through
    /// a blackbody-style color ramp, added on top of the scene) and the
    /// geometry itself is not rendered. fireintensity <= 0 = 1 (a plain
    /// brightness multiplier); firedetail <= 0 = automatic (from the
    /// draw bounds); firespeed scales the rise animation.
    bool fire = false;
    float fireintensity = 0.0f;
    float firedetail = 0.0f;
    float firespeed = 1.0f;
    /// Fountain body: the closed volume raymarches as a water-spray
    /// scattering medium of the volumetric pass (a rising jet plus a
    /// parabolic fall envelope with streak noise, splash rings fed to
    /// the water surface below) and the geometry itself is not
    /// rendered. fountaindensity/fountaindetail <= 0 = automatic (from
    /// the draw bounds); fountainspeed scales the flow animation.
    bool fountain = false;
    float fountaindensity = 0.0f;
    float fountaindetail = 0.0f;
    float fountainspeed = 1.0f;

    /// Light-source body flag of a triangle draw (SoFCRenderMaterial,
    /// typically fed from a ViewProvider Render_Light property): the
    /// geometry renders unshaded at its diffuse color (a glowing bulb /
    /// sun disc), feeds the bloom pass at lightintensity (an HDR
    /// multiplier — the halo scales with it even though the scene
    /// target clips at 1), and acts as an unshadowed point light on
    /// lit surfaces around it (the fire-light shortcut — no shadow map
    /// from it). lightintensity <= 0 = 1; lightrange <= 0 = automatic
    /// (from the draw bounds).
    bool lightsource = false;
    float lightintensity = 0.0f;
    float lightrange = 0.0f;
    /// The light-source body casts shadows from its point light: the
    /// backend renders a cached shadow-map tile for it (a downward
    /// wide-cone view; static scenes re-render it only on change).
    /// Off by default — a tile costs a scene depth render on every
    /// invalidation.
    bool lightshadow = false;
    /// Extended (omnidirectional) light shadow: the tile becomes six
    /// cube faces around the bulb, so geometry in any direction can
    /// shadow the light (not just below it). Costs up to six cached
    /// tile renders per invalidation; off by default.
    bool lightshadowext = false;

    /// User "material"-stage shader of a triangle draw (a scene
    /// SoShaderProgram node with stage="material" captured in the same
    /// render cache as the shape, docs/RenderDebug.md §6): the backend
    /// substitutes the program for the standard mesh fragment stage in
    /// the beauty passes (depth prepass, shadows and picking keep the
    /// stock shaders). Shared with the producing cache; pointer
    /// identity doubles as the draw-batch key.
    std::shared_ptr<const UserShader> usershader;

    /// Texture of a triangle draw (unit 0 only; GL applies further units
    /// on top, a known deviation) with its texture matrix, applied to
    /// MeshData::texCoords. Only sampled when the mesh carries texture
    /// coordinates.
    std::shared_ptr<const TextureImage> texture;
    float texmatrix[16];        ///< GL-layout, valid when !texidentity
    bool texidentity = true;

    /// Bump map of a triangle draw (unit-0 SoBumpMap): 1/2-component
    /// images perturb as grayscale height maps (parallax-occlusion
    /// mapped when BumpConfig::parallax), 3/4-component images are
    /// tangent-space normal maps (Coin's convention). The tangent frame
    /// comes from screen-space derivatives, so any UV source works —
    /// but the mesh must carry MeshData::texCoords, which only an
    /// enabled texture unit provides (pair bump-only scenes with a
    /// plain white texture). The wrap fields apply; model/blendColor
    /// are ignored.
    std::shared_ptr<const TextureImage> bumpmap;

    /// Emissive/occlusion material maps of a triangle draw
    /// (SoFCRenderTexture, typically fed from ViewProvider
    /// Render_EmissiveMap/Render_OcclusionMap properties). The emissive
    /// map's rgb adds to the lit (and textured) fragment color; the
    /// occlusion map's first channel multiplies the ambient/environment
    /// light contribution. Sampled with MeshData::texCoords like the
    /// bump map (same white-stand-in caveat); wrap fields apply,
    /// model/blendColor are ignored.
    std::shared_ptr<const TextureImage> emissivemap;
    std::shared_ptr<const TextureImage> occlusionmap;

    /// glTF metallic-roughness map of a triangle draw (SoFCRenderTexture,
    /// typically fed from a ViewProvider Render_MetallicRoughnessMap
    /// property): the green channel multiplies the roughness factor and
    /// the blue channel the metallic factor (glTF semantics) — only used
    /// while the PBR shading path is active. Sampling caveats as above.
    std::shared_ptr<const TextureImage> metallicroughnessmap;

    /// Autozoom transforms (SoAutoZoomTranslation): the draw's model
    /// matrix is rebuilt every frame by replaying these entries like the
    /// GL renderer's setupMatrix — accumulate each entry's matrix (or
    /// reset to it), then substitute the accumulated scale with
    /// scaleFactor times the per-frame world-to-screen scale fed through
    /// Renderer::setAutoZoomScale() (a scaleFactor of 0 keeps scale 1) —
    /// and DrawCall::model multiplies in last. Matrices are GL-layout
    /// like DrawCall::model.
    struct AutoZoomEntry {
        float matrix[16];
        float scaleFactor = 1.0f;
        bool identity = true;
        bool resetmatrix = false;
        /// Screen-align (billboard) the draw: the backend substitutes the
        /// upper 3x3 with the camera basis (so the quad always faces the
        /// viewer, like SoText2) instead of keeping the model rotation. Used
        /// by SoTextImage glyph quads.
        bool billboard = false;
        /// On-screen pixels per emitted geometry unit, resolved by the
        /// backend against the view it draws with. On a billboard, 0 keeps
        /// the backend's glyph-legibility text factor; image quads emitted
        /// in native pixels (SoImage capture companions) use 1 for raw-GL
        /// pixel parity. On an entry that is not a billboard, 0 keeps
        /// scaleFactor * the autozoom scale, and a value sizes it in pixels
        /// instead (a datum's number, SoDatumLabel).
        float pixelscale = 0.0f;
        /// Datum-label auto-flip: keep the glyph in its dimension plane, but
        /// mirror its local X/Y per frame so the number always reads
        /// left-to-right / upright from the current viewpoint (SoDatumLabel's
        /// GLRender does this via a projected-axis + backfacing test). `normal`
        /// is the world-space plane normal used for the backfacing decision.
        bool datumFlip = false;
        float normal[3] = {0.f, 0.f, 1.f};
    };
    std::vector<AutoZoomEntry> autozoom;

    /// World-space clip plane equations (sections). A fragment survives
    /// when dot(pos, plane.xyz) + plane.w >= 0 holds for every plane, or,
    /// in concave mode, for at least one plane (GL parity: SectionConcave
    /// renders one enabled plane per pass, i.e. the union of half-spaces).
    static constexpr int MaxClipPlanes = 6;
    uint8_t numclipplanes = 0;
    bool clipconcave = false;
    float clipplanes[MaxClipPlanes][4];
};

/// Which primitive buckets a Class-A display style draws
/// (docs/CoinRetirement.md 5.7). Bit i is Material::Type i, so a style
/// admits a draw when `(mask >> mat.type) & 1`.
///
/// The masks are read straight off what the ViewProviders put under
/// each display-mode child of their SoFCSwitch, which the stage-5
/// survey found to be nested subsets of one set of nodes: Shaded is the
/// faces, Wireframe is the lines AND the points (Part's Wireframe root
/// contains its Points root), Flat Lines is all three.
///
/// StyleAsIs (zero) means *no* style override -- every draw the feed
/// captured is drawn. That is what "As Is" means, and it is what every
/// frame outside a unified canvas uses, because there the style is
/// still applied by the Coin traversal that produced the capture.
enum DrawStyleMask : uint8_t {
    StyleAsIs      = 0,
    StyleFaces     = 1 << Material::Triangle,
    StyleLines     = 1 << Material::Line,
    StylePoints    = 1 << Material::Point,
    StyleShaded    = StyleFaces,
    StyleWireframe = StyleLines | StylePoints,
    StyleFlatLines = StyleFaces | StyleLines | StylePoints,
    /// Not a mask: a display mode no mask can describe. See
    /// DrawCall::ownStyle.
    StyleUnknown   = 0xff,
};

/// Which bucket a draw actually RENDERS as, which is not always its
/// Material::Type.
///
/// The mask above reads a draw's bucket off `mat.type`, which is right
/// only while a ViewProvider builds its display-mode children out of
/// separate face, line and point geometry -- which is what Part does.
/// Mesh does not: its "Wireframe" and "Point" children are the SAME
/// mesh node re-styled by an SoDrawStyle (Mod/Mesh/Gui/ViewProvider.cpp
/// -- pcLineStyle is LINES, pcPointStyle is POINTS), so the cache emits
/// them as Material::Triangle carrying a drawstyle. Classifying those
/// by type alone files a wireframe rendering under faces, and a
/// Wireframe filter then drops the mesh entirely.
///
/// A scene-wide drawstyle OVERRIDE is not reclassified: that is the
/// Tessellation display mode, whose filled faces still occupy their
/// faces bucket (they are drawn to occlude), and `drawstyleoverride` is
/// exactly what tells it from a plain SoDrawStyle node in the graph.
inline uint8_t styleBitOf(const Material &mat)
{
    if (mat.type == Material::Triangle && !mat.drawstyleoverride) {
        if (mat.drawstyle == Material::DrawLines)
            return StyleLines;
        if (mat.drawstyle == Material::DrawPoints)
            return StylePoints;
    }
    return static_cast<uint8_t>(1u << mat.type);
}

/// One draw of (a part of) a mesh with a material and model transform.
struct DrawCall {
    Material material;
    /// Index of `material` in the snapshot's material table, or -1.
    /// Load-side only (SceneDump v31): when the table itself is served
    /// out of band, it is what lets the draws be built before their
    /// materials have arrived. Never set by the live desktop feed.
    int32_t materialIndex = -1;
    std::shared_ptr<const MeshData> mesh;
    float model[16];        ///< GL-style layout, valid when !identity
    bool identity = true;
    /// Content hash of the scene-graph node path that produced this draw;
    /// the same object yields the same key in the scene and the
    /// selection/highlight feeds. 0 = unknown.
    uint64_t objectKey = 0;
    /// True when this draw covers the object's whole geometry of its
    /// primitive type (a whole-object on-top selection/highlight draw
    /// replaces — hides — the object's scene draws with equal objectKey).
    bool wholeObject = false;
    int partIndex = -1;     ///< -1 = whole mesh, >= 0 = single face/edge part
    /// Index range of the draw inside the index buffer selected by
    /// material.type. Resolved from partIndex by the producer; count 0
    /// means the whole buffer.
    int indexStart = 0;
    int indexCount = 0;
    float bboxMin[3] = {0.0f, 0.0f, 0.0f};  ///< world space bounds,
    float bboxMax[3] = {0.0f, 0.0f, 0.0f};  ///< empty if min > max
    /// This draw does not define the SCENE's bounds: it is a navigation
    /// gizmo (the axis cross, the rotation-centre sphere), captured
    /// under a Gui::SoSkipBoundingGroup, which is what Coin leaves out
    /// of the scene bounding box. Its own bounds above stay valid --
    /// culling and clipping still need them; what reads this is the
    /// min/max over the published draws that sizes the shadow ground
    /// and drives the camera's auto near/far. The rotation-centre
    /// sphere moves with the spin, so a scene bound that counted it
    /// would move the ground and the clip planes while the view turns.
    bool skipbounds = false;
    /// The display mode this draw's OBJECT is in, as a DrawStyleMask,
    /// and which Class-A style NAMES its display-mode switch has a
    /// child for (SoFCOwnDisplayModeElement::StyleNameBit bits).
    ///
    /// What lets one capture serve views in different display styles
    /// (docs/CoinRetirement.md 5.8): the feed captures the SUPERSET
    /// child and each view resolves its own style per object, the way
    /// Rhino and SolidWorks do. `ownStyle` serves a view showing "As
    /// Is"; `registeredStyles` reproduces the rule that a style whose
    /// name an object's switch does not carry does not apply to that
    /// object at all.
    ///
    /// ownStyle == StyleUnknown means the object's mode is not one of
    /// the four (Mesh's "Point", FEM's "Faces & Wireframe"): its
    /// buckets cannot be named, so nothing may filter this draw.
    uint8_t ownStyle = StyleUnknown;
    uint8_t registeredStyles = 0;
    /// Additive-capture context (docs/CoinRetirement.md 5.9
    /// "Non-standard modes"), all interned mode ids / bits over the
    /// capture's CaptureInterestTable, zero everywhere outside an
    /// interest capture:
    ///
    /// - capturedMode: non-zero = this draw came from an ADDITIVELY
    ///   traversed display-mode child of that name, captured beside
    ///   the normal flow. Such a draw serves exactly one thing -- an
    ///   override resolving to that very mode -- and is dropped by
    ///   every other view, or it would double-draw the object.
    /// - traversedMode: the mode name of the child the NORMAL flow
    ///   traversed, when that name is in the interest set (else 0).
    ///   An override naming it admits the untagged draws as they are:
    ///   they already ARE the mode, and no tagged copy exists.
    /// - interestBits: which interest modes the object's switch has a
    ///   child for. What tells "the override's mode was captured
    ///   additively, suppress the normal draws" from "the object has
    ///   no such mode child, fall back to its own mode" -- the same
    ///   fallback registeredStyles gives a Class-A style.
    uint16_t capturedMode = 0;
    uint16_t traversedMode = 0;
    uint16_t interestBits = 0;
    /// This draw is a coarse stand-in for geometry that has not arrived:
    /// a unit box scaled onto the bounds above, the bottom rung of the
    /// fidelity ladder (docs/SceneStreaming.md §6). It occupies space —
    /// it writes depth, including in the prepass — but it is not the
    /// shape, so it casts no shadow and takes no outline, capping or
    /// hidden-line pass: those are shading, not occupancy.
    ///
    /// Consumer-side only. A stand-in is synthesised by whoever is
    /// waiting, from the bounding box the manifest carries anyway, and
    /// is never serialized: nothing on the wire ever claims to be the
    /// mesh it stands in for.
    bool standIn = false;
    /// A companion drawable of this draw's object was deferred by the
    /// publish's capture budget, so the object is on screen with part
    /// of itself missing or stale. The element gates read it to tell
    /// "the face/line set has not arrived yet" apart from "the display
    /// mode legitimately omits it" (docs/SceneStreaming.md #13b): an
    /// attached point or line set whose companion is merely late must
    /// wait for it, not claim the mode exemption. Publish-transient
    /// and desktop-only for now -- never serialized.
    bool objectIncomplete = false;
};

typedef std::vector<DrawCall> DrawCallList;

/// The document object behind an objectKey. Everything is a plain
/// string: the renderer and the serving path must stay free of App/Gui
/// types.
///
/// Two halves with different lifetimes, and the split is the point:
///
/// - `doc` + `obj` are the **identity**, and they are fixed. An internal
///   name never changes, so the producer reads both straight off the
///   cache key's origin (setObjectInfo()) without touching a document.
/// - `label` + `type` are **presentation**, for a viewer to show a
///   human a name. They have nothing to do with a mesh, so they are not
///   resolved on the publish path: the serving path fills them from an
///   ObjectMetaMap (setObjectMeta()) that changes only when a document
///   does. A publish that nobody serves resolves neither.
///
/// ⚠️ Every one of these strings is UTF-8 and may hold any character a
/// Python identifier may — internal names included. Nothing here may be
/// byte-inspected, case-folded or truncated.
/// One step of ObjectInfo::path: a document object on the scene-graph
/// node chain that produced a draw.
struct ObjectRef {
    std::string doc;    ///< document internal name
    std::string obj;    ///< object internal name

    bool operator==(const ObjectRef &o) const
    { return obj == o.obj && doc == o.doc; }
};

struct ObjectInfo {
    std::string doc;    ///< document internal name (identity)
    std::string obj;    ///< object internal name (identity)
    std::string label;  ///< user-visible label, presentation only
    std::string type;   ///< DocumentObject type id, e.g. "Part::Box"
    /// The chain of document objects the draw's node path passes
    /// through, outermost first, ending at {doc, obj}; consecutive
    /// duplicates collapsed. Identity only, like doc/obj -- what a
    /// per-view display mode override entry matches against
    /// (docs/CoinRetirement.md 5.9): the leaf alone cannot say which
    /// CONTAINER the draw was reached through, and an override on a
    /// Link/group/assembly must reach the child draws below it. Not
    /// serialized by SceneDump: a remote viewer holds no per-view
    /// override table to resolve against.
    std::vector<ObjectRef> path;
    /// The draw's key itself: the ids of the scene-graph nodes it was
    /// composed from (Gui's selection root ids), outermost first, as
    /// the producer reads them off the key. Identity like the rest --
    /// an id is never reused -- and opaque here: the producer resolves
    /// a view's visibility entries against it (docs/CoinRetirement.md
    /// 5.23) and hands the backend the answer (VisibilitySet). Not
    /// serialized.
    std::vector<uint32_t> nodes;
};

typedef std::unordered_map<uint64_t, ObjectInfo> ObjectInfoMap;

/// What a viewer needs to *name* an object to a human. Not identity,
/// not geometry: a rename changes this and nothing else.
struct ObjectMeta {
    std::string label;  ///< the object's Label at the time it was pushed
    std::string type;   ///< DocumentObject type id
};

/// Presentation metadata by document internal name, then object
/// internal name. Nested rather than a joined key precisely because
/// both names are arbitrary UTF-8: there is no separator byte that
/// cannot occur in a name.
typedef std::unordered_map<std::string,
        std::unordered_map<std::string, ObjectMeta>> ObjectMetaMap;

/// One per-view per-object display mode override entry
/// (docs/CoinRetirement.md 5.9), parsed by the producer from the view's
/// ObjectDisplayModes property into resolved {doc, obj} steps so the
/// backend never touches a document.
struct StyleOverride {
    /// The objects the entry names, outermost first. Rooted: the first
    /// element must BE ObjectInfo::path[0] and the rest must follow it
    /// in order (an ordered subsequence, not a contiguous run, because
    /// a subname elides objects the scene chain contains -- a Link's
    /// target has a chain step but no subname token). Bare (one
    /// element, rooted false): the element may sit anywhere on the
    /// path -- the object wherever it appears in this view.
    std::vector<ObjectRef> path;
    bool rooted = true;
    /// The style to draw the matched objects with, when the entry's
    /// mode is one of the four Class-A names and the object's switch
    /// registers that name (DrawCall::registeredStyles) -- the same
    /// rule a view style follows. pin instead means the entry is
    /// "As Is": the object follows its OWN mode, escaping the view
    /// style (SolidWorks' "Default Display").
    uint8_t mask = StyleAsIs;
    uint8_t nameBit = 0;
    bool pin = false;
    /// Non-zero when the entry's mode is NOT one of the four Class-A
    /// names (docs/CoinRetirement.md 5.9 "Non-standard modes"): the
    /// interned id (internModeName) of the mode's name. Such a mode is
    /// a different subgraph, not a mask over the superset capture --
    /// the feed captures the named child ADDITIVELY, its draws tagged
    /// with this id (DrawCall::capturedMode), and the entry admits
    /// exactly the draws so tagged. mask/nameBit are meaningless when
    /// this is set.
    uint16_t modeId = 0;
};

/// A view's override table, handed to the backend by pointer: per
/// sub-view via SubViewFrame::styleOverrides, for the plain view via
/// setMainViewStyle(). The producer owns the storage and keeps it
/// alive while the backend may render with it; version is bumped on
/// every content change and is what the backend's resolved
/// objectKey cache keys on (together with objectInfoVersion()).
struct StyleOverrideTable {
    std::vector<StyleOverride> entries;
    uint32_t version = 0;
};

/// A view's own object visibility (docs/CoinRetirement.md 5.18, 5.23),
/// as the answer per objectKey. The producer resolves the view's entries
/// -- node keys, matched by tail against each draw's key (ObjectInfo::
/// nodes), the same rule its Coin traversals follow -- and the backend
/// only looks the draw up: a key absent from \c keys is one the view has
/// no entry for. Handed by pointer, per sub-view via SubViewFrame::
/// visibilities, for the plain view via setMainViewVisibility(); the
/// producer owns it and keeps it alive while the backend may render with
/// it, and bumps \c version on every content change.
struct VisibilitySet {
    enum : uint8_t {
        /// The object, or a container it is reached through, is hidden
        /// in this view.
        Hidden = 1,
        /// Something on the draw's chain is shown in this view: what
        /// admits a per-view-shown draw.
        Shown = 2,
    };
    std::unordered_map<uint64_t, uint8_t> keys;
    uint32_t version = 0;

    uint8_t flagsOf(uint64_t objectKey) const
    {
        auto it = keys.find(objectKey);
        return it == keys.end() ? 0 : it->second;
    }
};

/// The captured-mode id (DrawCall::capturedMode) tagging the draws of
/// a HIDDEN object captured only because some view shows it on its own
/// (ObjectVisibilities). Every view -- and every snapshot -- drops such
/// a draw unless its own table shows the object; one that does draws
/// it as the object's own, untagged draw.
RendererExport uint16_t perViewShownModeId();

/// The captured-mode id tagging what is drawn for the host's own views
/// and for nobody served the document: what the host is looking at of
/// its own accord, hung in the scene every client shares
/// (SoFCPathAnnotation::hostOnly; docs/TransactionLogJournal.md sec 32.24). A
/// view draws it as an untagged draw; a snapshot leaves it out.
RendererExport uint16_t hostOnlyModeId();

/// Process-lifetime intern table for display mode NAMES outside the
/// four Class-A styles (docs/CoinRetirement.md 5.9 "Non-standard
/// modes"). A name's id is stable for the life of the process and
/// never reused, so a draw tagged with it (DrawCall::capturedMode) and
/// an override entry naming it (StyleOverride::modeId) can meet at
/// submit with an integer compare, and no Coin type crosses into
/// Render. 0 is never returned for a real name; null/empty -> 0.
RendererExport uint16_t internModeName(const char *name);
/// The name behind an interned id, or null for 0/unknown. The returned
/// pointer lives as long as the process.
RendererExport const char *internedModeName(uint16_t id);

/// The capture's additive-mode interest list (docs/CoinRetirement.md
/// 5.9 "Non-standard modes"): the interned ids of every non-standard
/// mode any override of any view sharing the capture wants, in a fixed
/// order. The ORDER is the contract: bit i of DrawCall::interestBits
/// means "this draw's display-mode switch has a child named ids[i]",
/// so the producer that pushes this list to the traversal must hand
/// the SAME list here. At most 16 entries (the bit budget); the
/// producer drops and logs the excess, whose modes stay inert.
/// version bumps on every content change; the backend's resolved
/// override cache keys on it, because the id->bit mapping moved.
struct CaptureInterestTable {
    /// The bit budget: DrawCall::interestBits is 16 bits wide, so a
    /// list longer than this cannot be expressed. The producer drops
    /// and logs the excess.
    static const size_t MaxModes = 16;

    std::vector<uint16_t> ids;
    uint32_t version = 0;

    /// The interestBits bit of \a modeId, or 0 when not listed.
    uint16_t bitOf(uint16_t modeId) const {
        for (size_t i = 0; i < ids.size(); ++i) {
            if (ids[i] == modeId)
                return uint16_t(1u << i);
        }
        return 0;
    }
};

/// Flag bits of the selection ids fed through Renderer::addSelection
/// (mirroring SoFCRenderer::SelIdBits — the producer side of the feed).
enum SelIdBits : int {
    SelIdImplicit = 0x01000000,  ///< whole-object brought along a partial
    SelIdAlt      = 0x02000000,  ///< alternative (Ctrl) selection group
    SelIdFull     = 0x04000000,  ///< explicit whole-object selection
    SelIdPartial  = 0x08000000,  ///< sub-element selection
    SelIdSelected = SelIdFull | SelIdPartial,
};

/// A consumer that draws its own passes inside a renderer's frame
/// (docs/CAMSimRenderPort.md section 8 -- the borrowed frame).
///
/// The immediate-mode facade (DrawDevice.h) lets an outside module
/// draw with the backend; on its own it only lets that module own a
/// whole widget. A FrameConsumer instead draws INSIDE a 3D view's
/// frame, on pass ids taken from that view's own block, against that
/// view's scene target -- which is what makes its geometry sort with
/// the document's by depth rather than sit on top of it.
///
/// The renderer owns the surface and the frame boundary. Register with
/// Renderer::setFrameConsumer, and draw when drawFrame() is called:
/// there is no beginFrame/endFrame to run, because the host has
/// already begun the frame and bgfx's frame boundary is process-wide.
class RendererExport FrameConsumer
{
public:
    virtual ~FrameConsumer();

    /// How many passes to reserve inside the host frame. Read once
    /// per registration, so a consumer whose pass count changes
    /// re-registers. More than the host offers is refused (nothing is
    /// drawn) rather than silently clamped, because a clamp would put
    /// the consumer's last passes in somebody else's view id.
    virtual unsigned framePasses() const = 0;

    /// Of framePasses(), how many TRAILING passes belong in the
    /// overlay run -- drawn after the transparent composite, against
    /// the frame's finished depth. The rest are the scene run, drawn
    /// while the scene is still being composed: volumetric fog, water
    /// and glass, transparent geometry and everything after them see
    /// the scene run's output and the depth it writes. Ordering
    /// within each run follows pass index. Read at registration like
    /// framePasses(), and refused the same way when a run is over
    /// what the host offers (docs/CAMSimRenderPort.md sec 10.2).
    virtual unsigned overlayPasses() const { return 0; }

    /// Draw into \a surface. Called once per host frame; each pass's
    /// draws land where its run places them (overlayPasses() above),
    /// always before the on-top, highlight and overlay-feed passes.
    ///
    /// The surface's passes are live only for the duration of this
    /// call. Nothing here may cross the frame boundary or disturb the
    /// frame in progress: no scene feeds, no resize, no repaint
    /// request, and above all no bgfx frame of its own (the facade
    /// gives an attached surface no way to ask for one).
    virtual void drawFrame(DrawSurface &surface) = 0;
};

class RendererExport Renderer
{
public:
    Renderer();
    virtual ~Renderer();

    /// Unique for the lifetime of the process, and never reused. A
    /// producer that keeps state about what a renderer has already been
    /// told has to store this next to it: a renderer's *address* is
    /// reused freely — a backend torn down and rebuilt on a preference
    /// change can land where the last one was — and a producer comparing
    /// pointers would go on sending deltas against a table the new
    /// renderer never received.
    uint64_t instanceId() const { return instanceid; }

    /// How many times this renderer's identity table has been stated
    /// whole (setObjectInfo). Together with instanceId() this is the
    /// token a producer stores beside its resident copy, so that anything
    /// replacing the table behind the producer's back is a mismatch on
    /// the next publish rather than a silently incomplete table.
    uint32_t objectInfoVersion() const { return infoversion; }

    virtual const std::string &type() const = 0;
    virtual bool render(const QColor &bg,
                        const void *viewMatrix,
                        const void *projMatrix) = 0;

    /// One sub-view of a split-view frame (docs/SplitViews.md sec 9.2):
    /// a viewport rect on the output backbuffer, in device pixels, and
    /// the camera to render the resident scene with there. \a id is a
    /// stable client token naming the sub-view across frames -- the
    /// backend keys its per-sub-view state (sized targets, temporal
    /// accumulation) on it, so a layout change that keeps a cell keeps
    /// its id.
    struct SubViewFrame {
        int id = 0;
        int x = 0, y = 0;
        int width = 0, height = 0;
        const void *viewMatrix = nullptr;
        const void *projMatrix = nullptr;
        /// The Class-A display style this sub-view draws the shared
        /// scene with (docs/CoinRetirement.md 5.7, docs/SplitViews.md
        /// sec 17): a DrawStyleMask filtering the captured draws by
        /// primitive bucket at submit. StyleAsIs -- the default and
        /// every non-canvas frame -- draws what the feed captured.
        ///
        /// This is what lets N cells of ONE backend, fed by ONE
        /// traversal, show N different display styles. A style applied
        /// in the traversal instead would be baked into the shared
        /// capture, which is the whole reason the canvas could not vary
        /// it per cell.
        uint8_t drawStyle = StyleAsIs;
        /// The style NAME above, as a StyleNameBit, and whether the
        /// feed captured the SUPERSET child rather than each object's
        /// own mode (docs/CoinRetirement.md 5.8).
        ///
        /// Under a superset capture the filter is resolved per object:
        /// this sub-view's style where the object's display-mode switch
        /// carries a child of that name, and the object's OWN mode
        /// otherwise -- which is both what "As Is" means and what
        /// already happens today to an object whose switch does not
        /// carry the style's name (Mesh's "Point" under a "Points"
        /// override). Without the superset flag a mask can only remove,
        /// so it cannot serve a style that ADDS geometry the capture
        /// does not hold.
        uint8_t drawStyleName = 0;
        bool styleFromSuperset = false;
        /// The interned id (internModeName) of that style's NAME, when
        /// the capture carries the mode ADDITIVELY as well
        /// (docs/CoinRetirement.md 5.11): a style is an override, and
        /// an override's mode is its own SUBGRAPH -- a mask over the
        /// superset child reproduces it only while the superset child
        /// happens to contain the mode's buckets, which is a
        /// Part-shaped assumption and not a rule (Mesh's "Flat Lines"
        /// holds no point draws). Where the id is in the capture's
        /// interest list the sub-view draws the mode's own tagged
        /// draws and suppresses the untagged ones, exactly as an
        /// override naming the mode does; where it is not, the mask
        /// over the superset stays the answer. Zero outside a superset
        /// capture and for "As Is".
        uint16_t drawStyleMode = 0;
        /// This sub-view's per-object display mode overrides
        /// (docs/CoinRetirement.md 5.9), resolved per draw as the FIRST
        /// clause before the style above. Only meaningful under a
        /// superset capture -- an override can ADD geometry, which a
        /// filter over any other capture cannot serve. The producer
        /// owns the table and keeps it alive across the frame; null
        /// means no overrides.
        const StyleOverrideTable *styleOverrides = nullptr;
        /// This sub-view's own object visibility: draws whose object
        /// (or a container above it) is hidden are dropped from every
        /// pass of this sub-view only. The producer owns the set; null
        /// means the sub-view hides and shows nothing of its own.
        const VisibilitySet *visibilities = nullptr;
    };
    /// Render one frame as \a count sub-views tiling the backbuffer:
    /// the same resident scene feeds every sub-view, each drawn with
    /// its own camera into its own rect, inside a single backend frame.
    /// Returns false when the backend does not support it (the
    /// default), in which case the caller renders whole via render().
    virtual bool renderSubViews(const QColor &bg,
                                const SubViewFrame *subs, int count)
    {
        (void)bg; (void)subs; (void)count;
        return false;
    }
    /// Release the per-sub-view state a vanished sub-view id holds
    /// (targets, view-id block). Never id 0 -- that is the implicit
    /// full-canvas sub-view every plain render() uses.
    virtual void dropSubView(int id) { (void)id; }
    /// The plain (whole-canvas, sub-view id 0) frame's display style
    /// context (docs/CoinRetirement.md 5.9): the view's own Class-A
    /// style as mask + name bit, whether the feed captured the
    /// superset child, and the view's per-object override table (the
    /// caller owns it and keeps it alive; null = none). A plain view
    /// normally leaves all of this at rest -- its traversal applies
    /// its style -- but a view with overrides captures the superset
    /// like a canvas cell and needs the backend to resolve the style
    /// per object the same way.
    virtual void setMainViewStyle(uint8_t styleMask, uint8_t styleNameBit,
                                  bool fromSuperset,
                                  const StyleOverrideTable *overrides,
                                  uint16_t styleMode = 0)
    {
        (void)styleMask; (void)styleNameBit;
        (void)fromSuperset; (void)overrides; (void)styleMode;
    }
    /// The plain (sub-view id 0) frame's own object visibility, the
    /// counterpart of SubViewFrame::visibilities. The caller owns the
    /// set and keeps it alive; null = none.
    virtual void setMainViewVisibility(const VisibilitySet *set)
    {
        (void)set;
    }
    /// The additive-mode interest list of the capture feeding this
    /// backend (docs/CoinRetirement.md 5.9 "Non-standard modes") --
    /// the SAME list, in the same order, that the producer pushed to
    /// the traversal, because it defines what DrawCall::interestBits'
    /// bits mean. One per renderer, not per sub-view: the interest is
    /// a property of the shared capture. The caller owns the storage
    /// and keeps it alive while the backend may render with it; null
    /// (the default and the at-rest state) means no interest capture.
    virtual void setCaptureInterest(const CaptureInterestTable *table)
    {
        (void)table;
    }
    /// Prepare the backend for a renderSubViews frame: build the sized
    /// targets of every unseen sub-view id up front, each against a
    /// freshly reclaimed handle pool, and release what the layout
    /// obsoletes (the implicit full-canvas sub-view's targets when no
    /// sub is id 0), so the frame itself allocates nothing and never
    /// bails. Idempotent and cheap once every bank is warm, so the
    /// host may simply call it at the top of every layout frame -- but
    /// it crosses backend frame boundaries, so it must run while
    /// nothing of the upcoming frame is queued (before any page-cell
    /// draw). Optional: a backend without it just heals the first
    /// frame or two after a layout change.
    virtual void prepareSubViews(const QColor &bg,
                                 const SubViewFrame *subs, int count)
    {
        (void)bg; (void)subs; (void)count;
    }

    /// Render one frame for an offscreen capture -- a screenshot or an
    /// image export -- instead of the on-screen one. Two things differ
    /// from render(): the frame is rendered at \a width x \a height
    /// whatever the host widget's size is, and the finished image is
    /// transferred into the framebuffer the caller has bound rather
    /// than the widget's. Everything else is an ordinary frame with the
    /// same feeds, and the matrices mean what they mean in render() --
    /// build them for the capture's aspect ratio, not the widget's.
    /// Returns false when the backend cannot capture (the default), in
    /// which case the caller's own render path must draw the frame.
    virtual bool renderOffscreen(const QColor &bg,
                                 const void *viewMatrix,
                                 const void *projMatrix,
                                 int width, int height)
    {
        (void)bg; (void)viewMatrix; (void)projMatrix;
        (void)width; (void)height;
        return false;
    }

    /// Restrict subsequent renderOffscreen() frames to the scene draws
    /// of the named objects ({document internal name, object internal
    /// name}, as in ObjectInfo), with the selection, preselection and
    /// overlay feeds stripped and the window background flat
    /// transparent -- the per-capture object filter of the shaded-
    /// underlay capture (docs/TechDrawPortAndSection.md sec 26.2).
    /// Returns false when the backend cannot filter (the default) or
    /// when any named object has no draw in the resident scene (a
    /// 3D-hidden source has none) -- the caller must then fall back to
    /// its own capture path. Active until clearCaptureFilter(); the
    /// caller owns that bracket.
    virtual bool setCaptureFilter(
            const std::vector<std::pair<std::string, std::string>> &objects)
    {
        (void)objects;
        return false;
    }
    virtual void clearCaptureFilter() {}

    /// Provide a transient scene for subsequent renderOffscreen()
    /// frames: the supplied draws stand in for the resident scene feed
    /// during the capture -- selection, preselection and overlay feeds
    /// stripped, flat background, default camera-aligned headlight --
    /// without disturbing the resident feeds or their GPU residency.
    /// This is the shaded-underlay derived-shape capture (docs/
    /// TechDrawPortAndSection.md sec 31): a section's cut solid or a
    /// detail's clipped region exists nowhere in the resident scene, so
    /// the caller builds a dedicated Coin scene, runs it through the
    /// render-cache pipeline (SoFCRenderCacheManager::traverse +
    /// RendererBridge::translate) and hands the translated draws here.
    /// Returns false when the backend cannot render a supplied scene
    /// (the default). Active until clearCaptureScene(); the caller owns
    /// that bracket. Takes precedence over setCaptureFilter() while
    /// both are set.
    virtual bool setCaptureScene(DrawCallList &&draws)
    {
        (void)draws;
        return false;
    }
    virtual void clearCaptureScene() {}

    virtual bool boundBox(float &xmin, float &ymin, float &zmin,
                          float &xmax, float &ymax, float &zmax) = 0;

    /// Serialize the feeds and hand them to the scene-stream server,
    /// without drawing anything (docs/HeadlessServe.md §3.1). This is
    /// what render() does on the way past on a serving process; a
    /// publish-only renderer -- one with no GL widget and no graphics
    /// device behind it -- can only do this. \a viewMatrix and \a
    /// projMatrix are the camera a joining viewer adopts before it
    /// frames the scene itself, and the viewport is what those
    /// matrices were built for. Returns false when the backend cannot
    /// publish (default) or the feeds have not changed.
    virtual bool publish(const QColor &bg,
                         const void *viewMatrix,
                         const void *projMatrix,
                         int width, int height)
    {
        (void)bg; (void)viewMatrix; (void)projMatrix;
        (void)width; (void)height;
        return false;
    }

    /// The scene feed as it stands -- the draws publish() would
    /// serialize -- or null when the backend does not keep one. What a
    /// serve source reads an object's pose from while it bakes an
    /// Animation's motion clip (docs/Animation.md sec 9, "Streaming").
    /// Valid until the feed is next set.
    virtual const DrawCallList *sceneFeed() const { return nullptr; }

    /// The manifest version of the last publish() that went out, 0
    /// before any: what a serve source names the publish by when it
    /// says something about it on the control channel.
    virtual uint64_t publishedVersion() const { return 0; }

    /// Which document group this renderer's publishes belong to on the
    /// scene-stream server (docs/MultiDocServe.md §3). Empty -- the
    /// default -- publishes into the server's default group, which is
    /// the single-document behavior; a view-less serve source names its
    /// document here so a second document is a second group rather than
    /// a fight over one. Set before the first publish.
    virtual void setPublishGroup(const std::string &doc) { (void)doc; }

    /// Whether the frame just rendered contains time-animated content
    /// (e.g. water caustics): the viewer keeps scheduling redraws while
    /// this returns true, so the animation advances without user input.
    virtual bool animating() const { return false; }

    /// Whether the frames that follow may reach the screen pipelined
    /// where the backend gets there through a read-back
    /// (docs/DeviceAdoption.md section 10): such a frame shows the
    /// newest copy that has landed, a frame or two old, and pays
    /// nothing for the wait. Off, which is the default, each frame
    /// waits for its own copy and what is on screen is what was drawn.
    /// The host says it per frame (Render/ReadbackFrameMode); a capture
    /// waits whatever this says.
    virtual void setFramePipelined(bool on) { (void)on; }
    /// Whether the last frame to reach the screen was a pipelined one,
    /// so that the screen is behind the scene and stays there until
    /// another frame is drawn. The host owes it one frame that waits
    /// once the pipelined ones stop coming.
    virtual bool frameTrails() const { return false; }

    /// Global hint whether the active backend supports GPU-instanced
    /// draws. Geometry producers (e.g. the Part tessellation) consult it
    /// before emitting shared-instance scene structure: without real
    /// instancing many small shared nodes are a net loss, so they keep
    /// flattening instead. Defaults to true; a backend publishes its
    /// actual capability once initialized.
    static void setInstancingHint(bool supported);
    static bool instancingHint();

    /// Number of live backend renderer instances in the process. This —
    /// not any preference string — is the truth about whether a backend
    /// is active: a backend can be attached with the type preference
    /// still "Default" (per-view or scripted selection), and a
    /// preference naming a backend yields none when creation fails
    /// (plain-GL fallback).
    static int activeCount();
    /// Register a callback fired whenever activeCount() or
    /// instancingHint() changes. Observers are never removed — register
    /// only from static-lifetime contexts. Fires on the thread doing the
    /// change (backend create/destroy happens on the GUI thread).
    static void addActivityObserver(std::function<void()> observer);

    /// \name Scene API
    /// Mirrors SoFCRenderer's feed. Backends that don't consume scene data
    /// keep the default no-ops and canSkipInternal() == false, so the
    /// existing GL pipeline continues to draw the scene.
    //@{
    /// Replace the whole scene. An empty list clears it.
    virtual void setScene(DrawCallList &&draws) { (void)draws; }
    /// How many times setScene() has restated the scene: a consumer
    /// that derives something from the same feed (the Cycles viewport
    /// re-translates the render cache, docs/CyclesIntegration.md sec
    /// 5.2) compares this between frames instead of the draws. Every
    /// override of setScene() owes a noteSceneStated().
    uint64_t sceneGeneration() const { return scenegen; }
    /// Which document object each objectKey renders, resolved by the
    /// scene producer (the renderer itself has no document access — this
    /// library stays App-free). Replaced wholesale alongside setScene();
    /// keys the producer could not name are simply absent. Consumed by
    /// the scene-serving snapshot so a remote viewer can name what it
    /// picks (docs/ThinClient.md §4.1).
    virtual void setObjectInfo(ObjectInfoMap &&info) { (void)info; }
    /// Add identities the renderer does not have yet, leaving the rest of
    /// the table alone. What an objectKey renders is *fixed* -- a document
    /// and an object internal name, neither of which can change -- so a
    /// publish has nothing to correct here, only new keys to announce.
    /// Rebuilding the whole map to hand it over cost an insert and two
    /// string copies per object on every publish for a table that was
    /// already right (docs/IncrementalPublish.md §4d-iv).
    /// setObjectInfo() remains the way to state the whole table: the first
    /// publish, a renderer that has just been attached, and whenever the
    /// producer drops its resident copy rather than let it grow.
    virtual void updateObjectInfo(ObjectInfoMap &&added) { (void)added; }
    /// Presentation metadata (label, type) for the objects this renderer
    /// publishes, by document and object internal name. Pushed by the
    /// serving source when a document changes it — a rename, an object
    /// added or removed — and NOT per publish: nothing here describes a
    /// mesh, and re-deriving it per publish cost a document lookup per
    /// draw for a table only a serving viewer reads. A renderer nobody
    /// serves is never given one, and its published entries carry
    /// identity alone.
    virtual void setObjectMeta(ObjectMetaMap &&meta) { (void)meta; }
    /// Apply a change to that metadata: \a changed replaces or adds the
    /// entries it names, \a removed drops {document, object} pairs. The
    /// renderer holds the resident table, so a rename in a large
    /// document sends one entry rather than all of them — and a live
    /// import announcing thousands of new objects sends what arrived
    /// since the last publish rather than everything so far, every
    /// frame. setObjectMeta() remains the way to state the whole table,
    /// for the first push and whenever the producer cannot say what
    /// changed.
    virtual void updateObjectMeta(
            ObjectMetaMap &&changed,
            const std::vector<std::pair<std::string, std::string>> &removed)
    { (void)changed; (void)removed; }
    /// Describe the window background for the next render(). The bg color
    /// passed to render() stays the clear-color fallback for backends that
    /// ignore this.
    virtual void setBackground(const Background &bg) { (void)bg; }
    /// Add/replace one selection identified by id (SoFCRenderer::SelIdBits).
    virtual void addSelection(int id, DrawCallList &&draws)
    { (void)id; (void)draws; }
    virtual void removeSelection(int id) { (void)id; }
    /// Set the preselection highlight geometry.
    virtual void setHighlight(DrawCallList &&draws, bool wholeOnTop)
    { (void)draws; (void)wholeOnTop; }
    virtual void clearHighlight() {}
    /// Add/replace one overlay draw list keyed by \a id: viewport-anchored
    /// content (corner axis cross, foreground superimposition) drawn after
    /// the whole scene against a fresh depth buffer, with viewport and
    /// camera derived from \a anchor each frame. An empty list removes the
    /// overlay, same as removeOverlay().
    virtual void setOverlay(int id, DrawCallList &&draws,
                            const OverlayAnchor &anchor)
    { (void)id; (void)draws; (void)anchor; }
    virtual void removeOverlay(int id) { (void)id; }
    /// Register (or clear, with null) the consumer that draws its own
    /// passes inside this renderer's frames (FrameConsumer above).
    /// One per sub-view: registering a second under the same \a subView
    /// replaces the first, whose surface is destroyed. Backends that do
    /// not implement the draw facade ignore this, and the consumer
    /// keeps its own path.
    ///
    /// \a subView scopes the registration to one sub-view of a
    /// renderSubViews frame (SubViewFrame::id), so that N cells of one
    /// backend can each carry a consumer of their own -- a path-traced
    /// cell beside a rasterized one (docs/CyclesIntegration.md sec
    /// 5.11). 0, the default, is the implicit full-canvas sub-view a
    /// plain render() draws, and what every non-canvas host uses. A
    /// consumer registered under a sub-view is dropped with it
    /// (dropSubView).
    virtual void setFrameConsumer(FrameConsumer *consumer, int subView = 0)
    { (void)consumer; (void)subView; }
    /// The surface the FrameConsumer registered under \a subView draws
    /// into, or null when none is registered there (or the backend
    /// serves no consumers). Stable for the lifetime of the
    /// registration: a consumer keys per-host state on it
    /// (docs/CAMSimRenderPort.md sec 11.9) and drops that state when
    /// detaching, before the surface dies.
    virtual DrawSurface *frameConsumerSurface(int subView = 0)
    { (void)subView; return nullptr; }
    /// Per-frame hidden-line draw style state (resolved from the traversal
    /// state each render, like the GL renderer does).
    virtual void setHiddenLineConfig(const HiddenLineConfig &config)
    { (void)config; }
    /// The registered FrameConsumer supplies the shaded image of the
    /// scene (a path tracer's, docs/CyclesIntegration.md sec 5.1 and
    /// 5.3): the backend rasterizes its scene triangles depth-only, so
    /// that everything it still draws -- feature lines and points,
    /// selection and preselection, on-top draws, the host's own
    /// effects -- occludes against its own depth over the consumer's
    /// colour; transparent scene triangles are not drawn at all (the
    /// consumer's image carries their alpha), and a non-on-top
    /// selection fill dims where the scene depth hides it instead of
    /// vanishing. Ignored by backends without a consumer. Scoped like
    /// the registration: \a subView names the sub-view whose consumer
    /// supplies the image, so only that cell of a split-view frame
    /// gives up its raster shading.
    virtual void setExternalBaseLayer(bool on, int subView = 0)
    { (void)on; (void)subView; }
    /// Per-frame section fill (cap) configuration.
    virtual void setSectionConfig(const SectionConfig &config)
    { (void)config; }
    /// Per-frame ambient occlusion configuration.
    virtual void setAOConfig(const AOConfig &config) { (void)config; }
    /// Per-frame screen-space cavity (curvature) shading configuration.
    virtual void setCavityConfig(const CavityConfig &config)
    { (void)config; }
    /// Per-frame matcap shading configuration.
    virtual void setMatcapConfig(const MatcapConfig &config)
    { (void)config; }
    /// Per-frame render debugging configuration (docs/RenderDebug.md).
    virtual void setRenderDebugConfig(const RenderDebugConfig &config)
    { (void)config; }
    /// Per-frame occlusion culling configuration
    /// (docs/FarFieldProxies.md §12).
    virtual void setOcclusionCullConfig(const OcclusionCullConfig &config)
    { (void)config; }
    /// User-loadable shaders captured from scene SoShaderProgram nodes
    /// (docs/RenderDebug.md §6).
    virtual void setUserShaderConfig(const UserShaderConfig &config)
    { (void)config; }
    /// Whether the last rendered frame contained time-animated content
    /// (water waves, fire, clouds, caustics) — a repeat frame with the
    /// same camera and scene would differ. Clients that skip rendering
    /// while idle must keep rendering while this is true; the default
    /// is conservatively true.
    virtual bool isSceneAnimated() const { return true; }
    /// Whether scene/config changes are pending that the next render
    /// would pick up (selection, highlight, configs, a new scene feed).
    /// Clients that skip rendering while idle must render while this is
    /// true; the default is conservatively true.
    virtual bool isSceneDirty() const { return true; }
    /// User shaders compile asynchronously on the desktop (docs/
    /// RenderDebug.md sec 6): a frame drawn while one is in flight
    /// draws the stock material in its place. Whether any compile is in
    /// flight, and a counter that advances each time one finishes
    /// (either way) -- what a consumer that rendered a stand-in frame
    /// polls to know when to render again. Defaults: never pending,
    /// never advancing.
    virtual bool shaderCompilePending() const { return false; }
    virtual int shaderCompileGeneration() const { return 0; }
    /// Per-frame physically based shading configuration.
    virtual void setPBRConfig(const PBRConfig &config) { (void)config; }

    /// The output colour transform (OutputConfig): what happens to the
    /// finished frame before it is shown.
    virtual void setOutputConfig(const OutputConfig &config) { (void)config; }
    /// Per-frame bump/normal mapping configuration.
    virtual void setBumpConfig(const BumpConfig &config) { (void)config; }
    /// Per-frame scene light (Shadow draw style).
    virtual void setLightConfig(const LightConfig &config) { (void)config; }
    /// Per-frame ordinary Coin lights (viewer headlight and backlight,
    /// document SoDirectionalLight / SoPointLight). A backend that does
    /// not implement this keeps its fixed headlight, which is also what
    /// an unfed config asks for.
    virtual void setViewLightConfig(const ViewLightConfig &config)
    { (void)config; }
    /// Per-frame volumetric lighting (light shaft) configuration.
    virtual void setVolumetricConfig(const VolumetricConfig &config)
    { (void)config; }
    /// Per-frame water surface (refraction/reflection) configuration.
    virtual void setWaterConfig(const WaterConfig &config)
    { (void)config; }
    /// Per-frame bloom (glow) configuration.
    virtual void setBloomConfig(const BloomConfig &config)
    { (void)config; }
    /// Idle temporal accumulation configuration. A backend that does not
    /// implement this simply keeps drawing single-sample frames, which is
    /// also what an unfed config asks for.
    virtual void setTemporalConfig(const TemporalConfig &config)
    { (void)config; }
    /// Preselection (hover) highlight styling from ViewParams; carried in
    /// the snapshot for the standalone/WASM viewer's local hover highlight.
    virtual void setPreselConfig(const PreselHighlightConfig &config)
    { (void)config; }
    /// Selection highlight styling from ViewParams; carried in the snapshot
    /// for the standalone/WASM viewer's local (client-side) selection.
    virtual void setSelConfig(const PreselHighlightConfig &config)
    { (void)config; }
    /// Per-frame world-to-screen scale at the world origin consumed by
    /// Material::autozoom draws (Coin: SoAutoZoomTranslation's
    /// getWorldToScreenScale((0,0,0), 0.1) / (5 * viewport aspect)).
    /// Resolved from the traversal state each render like the
    /// hidden-line config, so it applies one frame late too.
    virtual void setAutoZoomScale(float scale) { (void)scale; }
    /// Scene multisample (MSAA) sample count for the backend's own render
    /// target (0/1 = off). Backends that render into an offscreen buffer own
    /// their MSAA independently of the host GL context, so a preference change
    /// is applied here rather than by recreating the view. Takes effect when
    /// the render target is next (re)created.
    virtual void setMSAASamples(int samples) { (void)samples; }
    /// Resolution scale (0.25-1.0) of the expensive screen-space effect
    /// passes -- the planar/ground reflection scene re-render and the SSAO
    /// resolve -- relative to the main view resolution. Lowering it trades
    /// effect sharpness for speed on large windows where those per-pixel
    /// passes dominate; the main scene, geometry prepass and overlays stay
    /// full resolution. Takes effect when the targets are next (re)created.
    virtual void setEffectResolution(float scale) { (void)scale; }
    /// Resolution scale (0.25-1.0) of the SSAO resolve targets, independent
    /// of setEffectResolution -- ambient occlusion is resolution-sensitive,
    /// so it has its own control. Takes effect when the targets are next
    /// (re)created.
    virtual void setSSAOResolution(float scale) { (void)scale; }
    /// Screen-space error tolerance in pixels of the desktop mesh-level
    /// plan (docs/SceneStreaming.md §13): a coarse-first tessellation
    /// whose stated error projects to more than this many pixels is
    /// re-tessellated exactly when the camera settles on it; 0 or less
    /// refines everything as soon as it appears. Only meaningful on a
    /// backend that drivesMeshLevels().
    virtual void setLevelTolerance(float px) { (void)px; }
    /// Whether this backend runs the desktop mesh-level plan pass (§13):
    /// a coarse-first display build (MeshSourceRegistry with a refine
    /// callback) only climbs back to exact when something plans it, so
    /// the coarse-first tessellation must not engage for a desktop view
    /// whose backend answers false here — the build would simply stay
    /// coarse.
    virtual bool drivesMeshLevels() const { return false; }
    /// GPU geometry budget in bytes for the desktop mesh-level plan
    /// (§13 step 3): over it, the plan downgrades the *displayed* rung
    /// of sources the camera would not miss — their exact meshes stay
    /// in CPU RAM, so the climb back is an instant re-activation. 0 =
    /// automatic: the backend's own reported GPU memory limit where
    /// the API states one (D3D/Vulkan do), else no budget at all.
    virtual void setGpuMemoryBudget(size_t bytes) { (void)bytes; }
    /// Narrate what each mesh-level plan decides. Needed to tell a
    /// ladder that will not descend apart from one that never ran --
    /// on desktop OpenGL the automatic GPU budget is 0, so the
    /// downgrade half of the plan had never executed and nothing said
    /// so.
    virtual void setLevelDebug(bool on) { (void)on; }
    /// How much of the error the plan is holding back to fit its GPU
    /// budget survives each plan that fits (Render::PressureTolerance,
    /// sec 13c.3). Handing it all back at the first plan inside the budget
    /// is what made this ladder cycle: the tolerance fell from 51px to
    /// 2px in one step, 946 objects re-tessellated at once, and the
    /// budget broke again. 0 or less restores that snap.
    virtual void setLevelPressureRelease(float fraction) { (void)fraction; }
    /// Whether the GPU downgrade sweep carries its unlanded orders as
    /// credit against the next plans' deficits (Render_DowngradeLedger;
    /// see Render::DowngradeLedger for why a sweep without one storms).
    virtual void setDowngradeLedger(bool on) { (void)on; }
    /// The hard-ceiling admission for climbs (Render_ClimbHardLimit /
    /// Render_ClimbAdmitBatch): at or over the budget the plan admits
    /// no refine and aborts those in flight; under it, climbs are
    /// admitted in batches of \a batch so the allocator-exact total
    /// approaches the ceiling in verified steps.
    virtual void setClimbAdmission(bool hardLimit, int batch)
    { (void)hardLimit; (void)batch; }
    /// How many descents (demotes/downgrades) one plan pass may order
    /// (Render_DescentOrderBatch, the climb batch's mirror): each
    /// order enqueues a worker job but pays a GUI-thread snapshot at
    /// the hook, so a pass is bounded and the replan after the batch
    /// lands takes the rest. 0 or less removes the cap.
    virtual void setDescentOrderBatch(int batch) { (void)batch; }
    /// The rest band above the GPU budget, as a fraction of it
    /// (Render_LevelBudgetDeadband): the downgrade sweep triggers only
    /// past budget*(1+fraction) and still corrects back to the budget,
    /// so an equilibrium that lands just over the line may stand --
    /// climbs already stop at the budget, and inside the band neither
    /// direction acts. 0 restores the bare line and with it the
    /// boundary dither.
    virtual void setLevelBudgetDeadband(float fraction) { (void)fraction; }
    /// The memory level, as a fraction of the GPU budget
    /// (Render_PerViewShownEvictWatermark), above which the level plan
    /// evicts released per-view-shown objects -- hidden objects no view
    /// shows any more, kept in the capture for a quick show again --
    /// before any sweep that costs visible quality
    /// (MeshSourceRegistry::evictReleasedShown).
    virtual void setPerViewShownEvictWatermark(float fraction) { (void)fraction; }
    /// The element contract's inputs (docs/SceneStreaming.md #13b),
    /// pushed in like every other parameter -- this library knows
    /// nothing of RenderParams. The contract itself lives in the
    /// backend: an attached point set draws only while its object's
    /// line set is shown and memory allows, an attached line set only
    /// while its face set is shown and memory allows, floating sets
    /// rank with the faces, and pressure spends points -> lines ->
    /// faces, taking them back in reverse.
    /// \a shapeVertices false suppresses attached point sets outright
    /// instead of letting the contract decide; \a pressureEdges false
    /// exempts attached line sets from the pressure stages;
    /// \a loadingDrop true forces both drops while a document is still
    /// arriving -- whether one is loading is a question only the host
    /// can answer, so it is pushed as a state and not derived here.
    /// \a staggerFrames is the frame wait between pressure stages,
    /// escalating and releasing both. None of these ever touches an
    /// on-top or highlight draw.
    /// MEASUREMENT ONLY (Render_TinyElementCutoff, 0 = off): suppress
    /// every line and point draw of this many primitives or fewer,
    /// floating sets included -- which the element contract never
    /// gates, and which is exactly the population a far-field cut is
    /// left drawing (docs/FarFieldProxies.md 11.1i). It exists to
    /// price the DRAW axis in the ~12-primitives-per-draw regime,
    /// which this engine has only ever measured at ~1540.
    virtual void setTinyElementCutoff(int prims) { (void)prims; }

    virtual void setElementGates(bool shapeVertices, bool pressureEdges,
                                 bool loadingDrop, int staggerFrames)
    { (void)shapeVertices; (void)pressureEdges; (void)loadingDrop;
      (void)staggerFrames; }
    /// How much of the edge and point sets not uploaded yet one frame
    /// may take in: \a sets of them and \a kilobytes of their buffers,
    /// whichever is spent first; the rest is held for the frames after
    /// (docs/DocumentLoad.md sec 18.17). 0 is no bound of that kind, and
    /// both 0 is every set in the frame that first draws it.
    virtual void setElementTakeIn(int sets, int kilobytes)
    { (void)sets; (void)kilobytes; }
    /// Section cap hatch texture pixels; \a nc-component 8-bit rows,
    /// tightly packed. Null data clears the texture. The pixels are copied.
    virtual void setHatchImage(const void *data, int nc,
                               int width, int height)
    { (void)data; (void)nc; (void)width; (void)height; }
    /// Arm a one-shot frame capture (docs/RenderDebug.md §4): the next
    /// rendered frame reads back the backend's scene color target
    /// (pre-composite) and, if the request carries a path, writes the
    /// image there; the readback statistics land in getRenderStats()
    /// either way. Returns false when the backend has no capture path
    /// (default, and the standalone build where the host reads the
    /// backbuffer itself).
    virtual bool requestFrameDump(const FrameDumpRequest &req)
    { (void)req; return false; }
    /// True while an armed frame dump has not been consumed by a
    /// rendered frame yet — the caller pumps frames until this clears.
    virtual bool frameDumpPending() const { return false; }
    /// The host's word that the NEXT frame is not the whole scene -- a
    /// publish deferred shapes under its capture budget, so what the
    /// backend holds is a scene still arriving. A pending dump is not
    /// consumed by that frame; the backend holds it for a later one,
    /// as it does for a frame drawn with a user shader still compiling.
    /// Per frame: cleared once the frame has run.
    virtual void holdFrameDump() {}
    /// Whether the last rendered frame held a pending dump (for a
    /// compiling shader or at the host's word). A pumping caller reads
    /// it as progress: a held frame is a frame, and one that builds a
    /// dozen programs on a software driver takes longer than any quiet
    /// timeout should.
    virtual bool frameDumpHeld() const { return false; }
    /// Whether the last rendered frame was COMPLETE -- the picture as
    /// authored: every user-shader program compiled, every shape the
    /// publish deferred arrived, a frozen frame's particle warm-up
    /// reached, and no mesh refine asked for by the level ladder. The
    /// same verdict that holds a frame dump, exposed so a test (or
    /// anyone else) can wait for the picture instead of for a number
    /// of frames. Costs the frame two flag writes and a compare; every
    /// wait lives in the caller. True where the backend has nothing
    /// to say.
    virtual bool frameComplete() const { return true; }
    /// Frames rendered, and complete frames rendered, since the backend
    /// came up. A waiter records the complete count, pumps, and stops
    /// when it advances -- a verdict left over from before the request
    /// proves nothing about the scene since -- and reads the rendered
    /// count as progress, so a frame that takes longer than any quiet
    /// timeout never runs it out.
    virtual uint64_t renderedFrames() const { return 0; }
    virtual uint64_t completeFrames() const { return 0; }
    /// Statistics of the last frame readback (a consumed frame dump);
    /// false while none has run.
    virtual bool getRenderStats(RenderStats &stats) const
    { (void)stats; return false; }
    /// The GPU (and driver) this backend is actually running on, as a
    /// human-readable line; empty when the backend cannot say.
    ///
    /// type() is not this and cannot stand in for it: it names the
    /// backend a viewer SELECTED, so a software rasterizer and a real
    /// adapter both report "bgfx - OpenGL". A golden render reference
    /// blessed on one device does not compare against another
    /// (tests/render/CMakeLists.txt says so for the Cycles leg), and
    /// without this a capture carries no record of which device made
    /// it -- so the exposure is real and undetectable after the fact.
    /// Measured: llvmpipe, Mesa d3d12 under xvfb, and d3d12 on a
    /// desktop window produce three visibly different captures and one
    /// identical type() string.
    virtual std::string deviceName() const { return std::string(); }
    /// Reload the backend's shader programs from disk on the next
    /// rendered frame (docs/RenderDebug.md §3): with FC_BGFX_SHADER_DIR
    /// pointing at a development asset tree, recompiling a shader
    /// (shaders/compile.sh) followed by this call is a live
    /// edit-save-see loop. Returns false when the backend has no
    /// reloadable shaders (default).
    virtual bool reloadShaders() { return false; }
    /// True if scene data changed after the last render() and another
    /// frame should be scheduled.
    virtual bool needsRedraw() const { return false; }
    /// True when this backend has rendered the current scene and the
    /// internal fixed-function GL pass can be skipped.
    virtual bool canSkipInternal() const { return false; }
    /// Give back every render target this view holds, because nobody is
    /// looking at it (docs/RenderEngine.md #3.3). A view's targets are
    /// its dominant cost -- measured at 287MB for a 1644x653 view with
    /// all effects on -- and they are held whether or not the view is
    /// on screen, so a session with several documents open pays for all
    /// of them to show one. The scene, the programs and the uniforms
    /// stay: this is the resize path's release, and the next frame
    /// rebuilds the targets exactly as a resize does.
    ///
    /// Costs the frame that rebuilds them (~68ms measured on the same
    /// view), so the caller is expected to be sure the view is going to
    /// stay in the background rather than release on every tab click --
    /// hence Render/BackgroundReleaseDelay. The picture that comes back
    /// is byte-identical; what this trades is a hitch, never an image.
    ///
    /// Returns false when the backend holds no releasable targets.
    virtual bool releaseTargets() { return false; }
    //@}

protected:
    /// Record that the identity table has just been stated whole, so a
    /// producer holding a resident copy of it can tell. Every override of
    /// setObjectInfo() owes this call; updateObjectInfo() must not make
    /// it, because a delta leaves the producer's copy still describing
    /// what the renderer holds.
    void noteObjectInfoStated() { ++infoversion; }
    /// Record that setScene() has just restated the scene
    /// (sceneGeneration() above).
    void noteSceneStated() { ++scenegen; }

private:
    static uint64_t nextInstanceId()
    {
        static std::atomic<uint64_t> counter{0};
        return ++counter;
    }
    const uint64_t instanceid = nextInstanceId();
    uint32_t infoversion = 0;
    uint64_t scenegen = 0;
};

class RendererLib
{
public:
    virtual ~RendererLib() {}
    virtual const std::string &name() const = 0;
    virtual const std::vector<std::string> &types() const = 0;
    virtual std::unique_ptr<Renderer> create(
            const std::string &type, QOpenGLWidget *widget,
            bool publishOnly = false) const = 0;

    /// Bring the backend up before anything asks it to draw.
    ///
    /// Backend startup is one-time and per process -- a GL context and
    /// the device -- but it was paid by whoever created the first 3D
    /// view, which made the first New Document of a session visibly
    /// slower than every later one. A host that knows a backend will be
    /// wanted can call this early instead; create() then finds the
    /// device already up.
    ///
    /// \a widget supplies the pixel format to build the context from,
    /// and need not be the widget that will eventually draw.
    /// Returns false if the backend does not warm up, or could not --
    /// in which case nothing is broken and create() will try again.
    ///
    /// Milliseconds per phase, so the host can report where the time
    /// went rather than only that it was spent.
    struct WarmupTiming {
        double context = 0;   ///< graphics context and its surface
        double device = 0;    ///< the backend device itself
        double programs = 0;  ///< shader programs and render targets
        double flush = 0;     ///< handing that work to the driver
        double total = 0;
    };
    virtual bool warmup(QOpenGLWidget *, const std::string &,
                        WarmupTiming * = nullptr) { return false; }

    /// The same warm-up, on a device this process did not create
    /// (docs/DeviceAdoption.md -- Route D, Qt owns the device through
    /// QRhi and the backend adopts it).
    ///
    /// It answers the same startup-ordering requirement as the overload
    /// above and answers it for a backend that has no GL context to be
    /// built from: bgfx::init is once per process, so whichever device
    /// is up first is the session's, and it must not be the first 3D
    /// view that decides. What changes here is not WHEN the device
    /// comes up but WHOSE device it is.
    ///
    /// False when the backend cannot adopt \a device -- an unusable
    /// API, a device already up, or a backend that does not implement
    /// this at all -- in which case nothing is broken and the caller
    /// falls back to the widget form.
    virtual bool warmup(const AdoptedDevice &, const std::string &,
                        WarmupTiming * = nullptr) { return false; }

    /// Whether this backend's device is up on OpenGL through a context
    /// in Qt's global share group -- the precondition for a host to
    /// composite a backend-rendered texture into its own Qt GL widget
    /// (the 2D page engine's interactive path does exactly that).
    virtual bool deviceSharesQtGL() const { return false; }
    /// Make the device's own GL context current / release it, for
    /// engine code that must pump a backend frame outside a 3D view's
    /// paint (the single-threaded GL device executes its frame in
    /// whichever context is current). Only meaningful when
    /// deviceSharesQtGL(); the caller owns re-acquiring its own
    /// context afterwards.
    virtual bool deviceMakeCurrent() { return false; }
    virtual void deviceDoneCurrent() {}
    /// The backend's immediate-mode draw facade (DrawDevice.h,
    /// docs/CAMSimRenderPort.md section 3), or null: the backend does
    /// not implement the facade, or its graphics device is not up yet.
    /// A virtual rather than a hard-linked entry point so that any
    /// backend can implement it.
    virtual DrawDevice *drawDevice() const { return nullptr; }
};

/// CPU accounting for the part of a frame that is *not* the renderer's.
///
/// The frame line splits its CPU into three: our own C++, the bgfx call
/// it ends in, and `outside` -- everything else between one backend
/// frame and the next. `outside` is derived (frame minus ours), so it is
/// complete by construction but says nothing about what is in it, and on
/// a native run it is the largest of the three. This is what puts an
/// instrument on it.
///
/// It lives here, as a process-wide accumulator rather than a method on
/// Renderer, because the code being timed is Gui's -- Coin's composite,
/// Qt's paint, the overlays -- and runs after render() has returned. The
/// backend drains it on the same tick it prints the frame line, and the
/// remainder it cannot attribute is printed too: a breakdown of a
/// derived quantity is only trustworthy if it says how much it missed.
/// The clock every time-animated effect reads -- water waves, caustics,
/// fire, particles, a user shader's u_fcTime (docs/RenderEngine.md,
/// "Determinism") -- pinned to a time of the caller's. For whoever steps
/// time itself: an export saving a frame for each of an Animation's
/// times pins it to that time, so the effects are in the video as they
/// are at that frame and not as the wall clock had them while it was
/// being saved (docs/Animation.md sec 9). Process-wide, as the clock is.
/// While pinned nothing asks for a redraw on the clock's account.
class RendererExport AnimationClock
{
public:
    /// Every frame from now reads \a seconds
    static void pin(double seconds);
    /// Back to the wall clock
    static void release();
    /// Whether it is pinned, and to what
    static bool pinned(double *seconds = nullptr);

private:
    static std::atomic<bool> isPinned;
    static std::atomic<double> pinnedSeconds;
};

/// Exact detail, held: while anybody holds it, a shape built for display
/// is tessellated exact up front instead of coarse first with the exact
/// mesh following (Render_CoarseTessellation), is never stood in for by
/// its bounding box, and a scene update captures every changed shape in
/// the one publish instead of rationing them over frames
/// (Render_CaptureBudgetMS). For whoever keeps or records what a frame
/// shows: an Animation caching its frames keeps the meshes each was shown
/// with, and an export saves a picture of each -- a frame that is coarse
/// because it was just rebuilt is wrong in both (docs/Animation.md sec 4,
/// "The frame cache"). What memory pressure orders is not held back: it
/// is what keeps the process alive. Counted, process-wide.
class RendererExport ExactDetail
{
public:
    static void hold();
    static void release();
    static bool held();
    /// One more reason it is held, asked each time: for what this library
    /// cannot see (the document's Animation keeping its frames)
    static void also(bool (*reason)());

private:
    static std::atomic<int> holders;
    static std::atomic<bool (*)()> reason;
};

class RendererExport FrameOutside
{
public:
    enum Phase {
        /// renderScene() before the backend call: viewport, background
        /// colour resolution, the meta feed.
        Pre,
        /// The background root traversal (and the GL background fill on
        /// frames the backend did not draw).
        Background,
        /// inherited::actualRedraw() -- the whole Coin scene-graph
        /// traversal. At render-cache mode 3 the geometry went to the
        /// backend, so this ought to be compositing overlays and little
        /// else; if it is not, that is a bug, not a tuning knob.
        Coin,
        /// The foreground root traversal.
        Foreground,
        /// Overlay captures re-traversed and fed to the backend.
        Captures,
        /// The chrome after them: axis cross, dimension text, navigation
        /// redraw, graphics items, fps string, NaviCube, alpha fixup.
        Chrome,
        /// QuarterWidget::paintEvent() before the redraw, minus the
        /// delay queue below.
        PaintPre,
        /// Coin's delay queue, drained at the top of the paint event
        /// with the GL context released around it.
        DelayQueue,
        /// QGraphicsView::paintEvent() -- Qt's own painting over the GL
        /// viewport, which runs after the scene is drawn.
        GraphicsView,
        /// The tail of the paint event after that.
        PaintPost,
        PhaseCount
    };

    /// Whether anything is draining. False costs one relaxed load, which
    /// is what makes it safe to leave the calls in the render path.
    static bool enabled() { return active.load(std::memory_order_relaxed); }
    static void setEnabled(bool on);
    static void add(Phase p, double ms);
    /// Move the accumulated per-phase milliseconds out, zeroing them.
    /// \a out is indexed by Phase and always fully written.
    static void drain(double *out);

private:
    static std::atomic<bool> active;
    static double phaseMs[PhaseCount];
};

/// Times a scope into a FrameOutside phase. Reads the switch once, on
/// entry, so a scope that spans the frame where it is turned on is
/// either wholly counted or wholly not -- never half.
class RendererExport FrameOutsideScope
{
public:
    explicit FrameOutsideScope(FrameOutside::Phase p);
    ~FrameOutsideScope();
    /// End the region early and disarm. For a span that starts at the
    /// top of a function and ends in its middle, where a nested block
    /// would put the locals it declares out of reach of the rest.
    /// Several scopes may name the same phase; they add.
    void stop();
    FrameOutsideScope(const FrameOutsideScope &) = delete;
    FrameOutsideScope &operator=(const FrameOutsideScope &) = delete;

private:
    FrameOutside::Phase phase;
    bool on;
    int64_t t0;
};

class RendererExport RendererFactory
{
public:
    static std::vector<std::string> types();
    /// \a publishOnly asks for a renderer that will never draw: no
    /// graphics device is created and none is required, so the process
    /// needs no GPU and no display (docs/HeadlessServe.md §3.1). Such a
    /// renderer answers publish() and nothing else. It is a mode of its
    /// own rather than "widget == nullptr", which the standalone viewer
    /// already passes for a renderer that very much does draw.
    static std::unique_ptr<Renderer> create(const std::string &type,
                                            QOpenGLWidget *widget,
                                            bool publishOnly = false);
    /// Bring \a type's backend up ahead of the first create(), so that
    /// the first 3D view of a session does not pay for it. See
    /// RendererLib::warmup.
    static bool warmup(const std::string &type, QOpenGLWidget *widget,
                       RendererLib::WarmupTiming *timing = nullptr);
    /// Bring \a type's backend up on a device somebody else owns. See
    /// RendererLib::warmup(const AdoptedDevice &, ...).
    static bool warmup(const std::string &type, const AdoptedDevice &device,
                       RendererLib::WarmupTiming *timing = nullptr);
    /// The first registered backend's draw facade (DrawDevice.h), or
    /// null while none has its device up. DrawDevice::instance() is
    /// the public door to this.
    static DrawDevice *drawDevice();
    static void registerLib(RendererLib *);
    static void setResourcePath(const std::string &path);
    static const std::string &resourcePath();

    /// Highest number of view ids the backend should hand out, or 0 for
    /// the backend's own maximum.
    ///
    /// Every 3D view holds a block of ids for its pass sequence, so this
    /// is what decides how many viewers a session can drive on the
    /// backend at once. It is a STARTUP option: backend startup is
    /// one-time per process (RendererLib::warmup), so the value in force
    /// is whichever was set before the first create()/warmup() and a
    /// change needs a restart to take effect.
    ///
    /// The renderer library cannot read a preference itself, so the host
    /// seeds it -- the same arrangement as setResourcePath and the
    /// scene-stream thread cap.
    static void setMaxViewIds(int count);
    static int maxViewIds();

    /// The RendererLib device-context hooks, resolved over the
    /// registered backends (the process has at most one device; the
    /// first lib that answers wins). See RendererLib::deviceSharesQtGL.
    static bool deviceSharesQtGL();
    static bool deviceMakeCurrent();
    static void deviceDoneCurrent();
};

} // namespace Render

#endif // RENDERER_RENDERER_H
