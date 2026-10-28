/*
 * Procedural matcaps: the shading of a fragment from its view-space
 * normal alone, with no scene lights involved.
 *
 * A matcap ("material capture") is normally an image of a lit sphere,
 * indexed by the screen projection of the normal. These presets compute
 * the same thing analytically instead, which is what lets the feature
 * ship with no image assets to commit, install, or fetch -- the browser
 * tier gets it for free, at any resolution, with no bundle growth.
 *
 * Every direction below is in VIEW space, i.e. the studio is welded to
 * the camera. That is the whole point of matcap shading: the light does
 * not swim when the model turns, so two faces that face the same way
 * shade the same way, anywhere in the scene, and form is comparable
 * across parts.
 *
 * Needs nothing but the normal, so it can be included anywhere.
 */

#ifndef FC_MATCAP_SH
#define FC_MATCAP_SH

// The normal `nw` (world) in the frame whose third axis is `dir`: what the
// view space normal would be to a camera looking back along `dir`, with
// world Z up where it can be.
vec3 fc_matcapFrame(vec3 nw, vec3 dir)
{
	vec3 z = normalize(dir);
	vec3 up = abs(z.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	vec3 x = normalize(cross(up, z));
	vec3 y = cross(z, x);
	return vec3(dot(nw, x), dot(nw, y), dot(nw, z));
}

// n: shading normal, normalized, view space, already flipped toward the
// viewer by the caller for two-sided draws; nw: the same in world space.
// preset: 0 studio, 1 clay, 2 metal, 3 pearl, 4 zebra, 5 draft.
// stripes: zebra only, the dark/light pairs between the room's two poles.
// dir: xyz a world direction -- the fixed zebra's eye, the draft's pull;
// w the fixed zebra's switch, or the draft's least angle in radians.
vec3 fc_matcap(float preset, vec3 n, vec3 nw, float stripes, vec4 dir)
{
	if (preset > 4.5)
	{
		// Draft analysis (docs/Surface.md sec 7): how a face releases from
		// a mold pulled along dir. Its draft is the angle between it and
		// the pull, asin(n . pull): released (green) where that is more
		// than the least angle, an undercut (red) where it is less than
		// its negative, and in between (yellow) too steep to release.
		float a = asin(clamp(dot(nw, normalize(dir.xyz)), -1.0, 1.0));
		float t = max(dir.w, 1.0e-4);
		// a pixel's worth of blend at each boundary, as the zebra's edges
		float w = max(fwidth(a), 1.0e-5);
		float up = clamp(0.5 + (a - t) / w, 0.0, 1.0);
		float down = clamp(0.5 + (-t - a) / w, 0.0, 1.0);
		vec3 released = vec3(0.20, 0.72, 0.28);
		vec3 steep = vec3(0.95, 0.80, 0.18);
		vec3 undercut = vec3(0.86, 0.22, 0.20);
		vec3 c = mix(mix(steep, released, up), undercut, down);
		// a little of the shape, so that faces of one colour still read
		return c * (0.75 + 0.25 * clamp(n.z, 0.0, 1.0));
	}
	if (preset > 3.5)
	{
		// Fixed to the model: the room is looked at along a world
		// direction, not the camera's, so a stripe stays where it is on
		// the surface as the view turns.
		if (dir.w > 0.5)
			n = fc_matcapFrame(nw, dir.xyz);
		// Zebra: the surface as a mirror in a room of parallel light
		// strips, which is the surface-continuity view of other CAD
		// systems. A stripe is a line of equal reflection, so what the
		// stripes do at an edge says how the two faces meet there: they
		// STEP where the normal jumps (a crease), they meet but kink
		// where the faces are tangent and the curvature jumps, and they
		// run straight through where the curvature is continuous too. A
		// stripe torn inside one face is that face undulating.
		//
		// r is the view ray mirrored by the surface, with the ray taken
		// along the view axis like every preset here -- so the stripes
		// are a function of the normal alone and a flat face is one
		// tone. The room's strips are rings of equal elevation, leaned
		// a little so that a surface turning about the vertical alone
		// still crosses some.
		vec3 r = 2.0 * n.z * n - vec3(0.0, 0.0, 1.0);
		float t = atan2(r.y, length(r.xz)) + 0.35 * r.x;
		float ph = t * 2.0 * stripes;
		// cos, not sin: a face seen head on then sits in the middle of a
		// light stripe instead of on an edge, where the least turn of
		// the view would flip it.
		float s = cos(ph);
		// One pixel of edge, measured rather than assumed: the phase
		// runs at any speed across the screen, and a fixed threshold
		// would alias wherever it runs fast.
		float w = fwidth(ph);
		float v = clamp(0.5 + s / max(w, 1.0e-5), 0.0, 1.0);
		// Stripes finer than the pixel grid can carry go to their mean
		// rather than to moire.
		v = mix(v, 0.5, smoothstep(1.0, 2.5, w));
		return vec3_splat(mix(0.04, 0.96, v));
	}

	// Key from the upper left, fill from the lower right — the studio
	// convention, and the one that reads as "lit from above" to the eye.
	vec3 key = normalize(vec3(-0.45, 0.60, 0.66));
	vec3 fill = normalize(vec3(0.55, -0.35, 0.75));
	float ndk = dot(n, key);
	float ndf = dot(n, fill);
	// Facing ratio: 1 head-on, 0 at the silhouette.
	float ndv = clamp(n.z, 0.0, 1.0);
	float fres = pow(1.0 - ndv, 4.0);

	if (preset < 0.5)
	{
		// Studio: broad key, weak fill, tight highlight, faint rim.
		float d = 0.42 + 0.50 * max(ndk, 0.0) + 0.16 * max(ndf, 0.0);
		vec3 h = normalize(key + vec3(0.0, 0.0, 1.0));
		float s = pow(max(dot(n, h), 0.0), 48.0);
		return vec3_splat(d + s * 0.50 + fres * 0.16);
	}
	if (preset < 1.5)
	{
		// Clay: matte hemisphere, no highlight at all. The most neutral
		// read of form — nothing competes with the shape itself.
		float d = 0.30 + 0.68 * (0.5 + 0.5 * ndk);
		return vec3_splat(d * (1.0 - 0.22 * fres));
	}
	if (preset < 2.5)
	{
		// Metal: a banded sweep across the normal, so a small change of
		// slope moves a band a long way — curvature is exaggerated on
		// purpose, which is what makes shallow defects visible.
		float band = 0.5 + 0.5 * sin(ndk * 6.0);
		float d = 0.16 + 0.50 * smoothstep(0.15, 0.95, band)
			+ 0.34 * max(ndk, 0.0);
		return vec3_splat(d + fres * 0.50);
	}
	// Pearl: warm/cool dual tone across the key axis plus a soft sheen.
	// The hue shift carries gentle undulation that a grey matcap flattens.
	vec3 warm = vec3(1.00, 0.94, 0.86);
	vec3 cool = vec3(0.62, 0.70, 0.85);
	vec3 c = mix(cool, warm, clamp(0.5 + 0.5 * ndk, 0.0, 1.0));
	float s = pow(max(ndk, 0.0), 16.0);
	return c * (0.55 + 0.45 * ndv) + vec3_splat(s * 0.28) + cool * fres * 0.38;
}

#endif // FC_MATCAP_SH
