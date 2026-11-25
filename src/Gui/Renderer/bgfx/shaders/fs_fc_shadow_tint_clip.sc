$input v_wpos

/*
 * Shadow tint caster, clipped variant (vs_fc_shadow_clip pair): what a
 * section plane cuts away of a glass or translucent part lets the light
 * through untouched -- the cut part does not tint, as it does not cast.
 * The transmittance as fs_fc_shadow_tint.sc gives it.
 */

#include <bgfx_shader.sh>
#include "fc_clip.sh"

uniform vec4 u_matColor;

void main()
{
	clipDiscard(v_wpos);
	if (u_matColor.a < 0.0)
		gl_FragColor = vec4(u_matColor.rgb, 1.0);
	else
		gl_FragColor = vec4(vec3_splat(0.3) + 0.5 * u_matColor.rgb, 1.0);
}
