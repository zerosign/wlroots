#include <lcms2.h>
#include <stdlib.h>
#include <wlr/render/color.h>
#include <wlr/util/log.h>

static void handle_lcms_error(cmsContext ctx, cmsUInt32Number code, const char *text) {
	wlr_log(WLR_ERROR, "[lcms] %s", text);
}

bool wlr_color_transform_init_srgb_to_icc(struct wlr_color_transform *tr,
		const void *data, size_t size) {
	bool ok = false;

	cmsContext ctx = cmsCreateContext(NULL, NULL);
	if (ctx == NULL) {
		wlr_log(WLR_ERROR, "cmsCreateContext failed");
		return false;
	}

	cmsSetLogErrorHandlerTHR(ctx, handle_lcms_error);

	cmsHPROFILE icc_profile = cmsOpenProfileFromMemTHR(ctx, data, size);
	if (icc_profile == NULL) {
		wlr_log(WLR_ERROR, "cmsOpenProfileFromMemTHR failed");
		goto out_ctx;
	}

	if (cmsGetDeviceClass(icc_profile) != cmsSigDisplayClass) {
		wlr_log(WLR_ERROR, "ICC profile must have the Display device class");
		goto out_icc_profile;
	}

	cmsHPROFILE srgb_profile = cmsCreate_sRGBProfile();
	if (srgb_profile == NULL) {
		wlr_log(WLR_ERROR, "cmsCreate_sRGBProfile failed");
		goto out_icc_profile;
	}

	cmsHTRANSFORM lcms_tr = cmsCreateTransformTHR(ctx,
		srgb_profile, TYPE_RGB_FLT, icc_profile, TYPE_RGB_FLT,
		INTENT_RELATIVE_COLORIMETRIC, 0);
	if (lcms_tr == NULL) {
		wlr_log(WLR_ERROR, "cmsCreateTransformTHR failed");
		goto out_srgb_profile;
	}

	size_t dim_len = 33;
	float *lut_3d = calloc(3 * dim_len * dim_len * dim_len, sizeof(float));
	if (lut_3d == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		goto out_lcms_tr;
	}

	float factor = 1.0f / (dim_len - 1);
	for (size_t b_index = 0; b_index < dim_len; b_index++) {
		for (size_t g_index = 0; g_index < dim_len; g_index++) {
			for (size_t r_index = 0; r_index < dim_len; r_index++) {
				float rgb_in[3] = {
					r_index * factor,
					g_index * factor,
					b_index * factor,
				};
				float rgb_out[3];
				cmsDoTransform(lcms_tr, rgb_in, rgb_out, 1);

				size_t offset = 3 * (r_index + dim_len * g_index + dim_len * dim_len * b_index);
				// TODO: maybe clamp values to [0.0, 1.0] here?
				lut_3d[offset] = rgb_out[0];
				lut_3d[offset + 1] = rgb_out[1];
				lut_3d[offset + 2] = rgb_out[2];
			}
		}
	}

	ok = true;
	*tr = (struct wlr_color_transform){
		.lut_3d = lut_3d,
		.dim_len = dim_len,
	};

out_lcms_tr:
	cmsDeleteTransform(lcms_tr);
out_srgb_profile:
	cmsCloseProfile(srgb_profile);
out_icc_profile:
	cmsCloseProfile(icc_profile);
out_ctx:
	cmsDeleteContext(ctx);
	return ok;
}

void wlr_color_transform_finish(struct wlr_color_transform *tr) {
	free(tr->lut_3d);
}
