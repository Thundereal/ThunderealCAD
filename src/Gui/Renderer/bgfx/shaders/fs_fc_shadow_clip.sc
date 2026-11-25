$input v_wpos

/*
 * Shadow map caster fragment shader, clipped variant: section-clipped
 * geometry does not cast shadows from its clipped-away parts.
 */

#include <bgfx_shader.sh>
#include "fc_clip.sh"

uniform vec4 u_evsm;

void main()
{
	clipDiscard(v_wpos);
	// plain (z, z^2) when the warp is off, as fs_fc_shadow.sc: without
	// it exp(0) = 1 put every clipped caster at the far plane -- no
	// shadow from a cut part in a plain VSM map, and a cut translucent
	// part's near depth behind everything
	float z = gl_FragCoord.z;
	if (u_evsm.x < 0.5)
	{
		gl_FragColor = vec4(z, z * z, 0.0, 1.0);
	}
	else
	{
		float e = exp(u_evsm.x * z);
		gl_FragColor = vec4(e, e * e, 0.0, 1.0);
	}
}
