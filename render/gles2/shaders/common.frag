/* enum wlr_gles2_shader_source */
#define SOURCE_SINGLE_COLOR 1
#define SOURCE_TEXTURE_RGBA 2
#define SOURCE_TEXTURE_RGBX 3
#define SOURCE_TEXTURE_EXTERNAL 4

/* enum wlr_gles2_color_transform */
#define COLOR_TRANSFORM_IDENTITY 0
#define COLOR_TRANSFORM_LUT_3D 1

#if !defined(SOURCE) || !defined(COLOR_TRANSFORM)
#error "Missing shader preamble"
#endif

#if SOURCE == SOURCE_TEXTURE_EXTERNAL
#extension GL_OES_EGL_image_external : require
#endif

#if COLOR_TRANSFORM == COLOR_TRANSFORM_3DLUT
#extension GL_OES_texture_3D : require
#endif

precision mediump float;

varying vec2 v_texcoord;

#if SOURCE == SOURCE_TEXTURE_EXTERNAL
uniform samplerExternalOES tex;
#elif SOURCE == SOURCE_TEXTURE_RGBA || SOURCE == SOURCE_TEXTURE_RGBX
uniform sampler2D tex;
#elif SOURCE == SOURCE_SINGLE_COLOR
uniform vec4 color;
#endif

#if SOURCE != SOURCE_SINGLE_COLOR
uniform float alpha;
#else
const float alpha = 1.0;
#endif

#if COLOR_TRANSFORM == COLOR_TRANSFORM_LUT_3D
uniform mediump sampler3D lut_3d;
uniform float lut_3d_offset;
uniform float lut_3d_scale;
#endif

vec4 sample_texture() {
#if SOURCE == SOURCE_TEXTURE_RGBA || SOURCE == SOURCE_TEXTURE_EXTERNAL
	return texture2D(tex, v_texcoord);
#elif SOURCE == SOURCE_TEXTURE_RGBX
	return vec4(texture2D(tex, v_texcoord).rgb, 1.0);
#elif SOURCE == SOURCE_SINGLE_COLOR
	return color;
#endif
}

vec4 transform_color(vec4 color) {
#if COLOR_TRANSFORM == COLOR_TRANSFORM_IDENTITY
	return color;
#elif COLOR_TRANSFORM == COLOR_TRANSFORM_LUT_3D
	vec3 pos = lut_3d_offset + color * lut_3d_scale;
	return texture3D(lut_3d, pos).rgb;
#endif
}

void main() {
	vec4 color = sample_texture() * alpha;

#if COLOR_TRANSFORM != COLOR_TRANSFORM_IDENTITY
	// Convert from pre-multiplied alpha to straight alpha
	if (color.a == 0.0)
		color.rgb = vec3(0.0, 0.0, 0.0);
	else
		color.rgb /= color.a;

	color = transform_color(color);

	// Convert from straight alpha to pre-multiplied alpha
	color.rgb *= color.a;
#endif

	gl_FragColor = color;
}
