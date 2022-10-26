/* enum wlr_gles2_shader_source */
#define SOURCE_SINGLE_COLOR 1
#define SOURCE_TEXTURE_RGBA 2
#define SOURCE_TEXTURE_RGBX 3
#define SOURCE_TEXTURE_EXTERNAL 4

#if !defined(SOURCE)
#error "Missing shader preamble"
#endif

#if SOURCE == SOURCE_TEXTURE_EXTERNAL
#extension GL_OES_EGL_image_external : require
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

vec4 sample_texture() {
#if SOURCE == SOURCE_TEXTURE_RGBA || SOURCE == SOURCE_TEXTURE_EXTERNAL
	return texture2D(tex, v_texcoord);
#elif SOURCE == SOURCE_TEXTURE_RGBX
	return vec4(texture2D(tex, v_texcoord).rgb, 1.0);
#elif SOURCE == SOURCE_SINGLE_COLOR
	return color;
#endif
}

void main() {
	gl_FragColor = sample_texture() * alpha;
}
