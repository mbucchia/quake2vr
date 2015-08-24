#include "include/r_vr_rift.h"
#include "../vr/include/vr_rift.h"
#include "include/r_local.h"
#ifdef OCULUS_DYNAMIC
#include "../vr/oculus_dynamic/oculus_dynamic.h"
#else
#include "OVR_CAPI_GL.h"
#endif
#include "../../backends/sdl2/sdl2quake.h"


void Rift_FrameStart(void);
void Rift_Present(fbo_t *destination, qboolean loading);
int32_t Rift_Enable(void);
void Rift_Disable(void);
int32_t Rift_Init(void);
void Rift_GetState(vr_param_t *state);
void Rift_PostPresent(void);
void Rift_SetOffscreenSize(uint32_t width, uint32_t height);
void Rift_DrawToHMD(fbo_t *source);
void Rift_DrawToScreen(fbo_t *destination);

hmd_render_t vr_render_rift =
{
	HMD_RIFT,
	Rift_Init,
	Rift_Enable,
	Rift_Disable,
	Rift_FrameStart,
	Rift_SetOffscreenSize,
	Rift_GetState,
	Rift_Present,
	Rift_DrawToHMD,
	Rift_DrawToScreen
};

rift_render_export_t renderExport;


extern ovrHmd hmd;
extern ovrEyeRenderDesc eyeDesc[2];
extern ovrTrackingState trackingState;
extern ovrFrameTiming frameTime;

static vec4_t cameraFrustum[4];

extern void VR_Rift_GetFOV(float *fovx, float *fovy);
extern int32_t VR_Rift_RenderLatencyTest(vec4_t color);


static vr_param_t currentState;

static fbo_t swapFBO;
static uint32_t currentFBO = 0;
static ovrSwapTextureSet *swapTextures = NULL;
static ovrGLTexture *mirrorTexture = NULL;
ovrLayerEyeFov swapLayer;

// this should probably be rearranged
typedef struct {
	fbo_t eyeFBO;
	ovrSizei renderTarget;
	ovrFovPort eyeFov;
	ovrVector2f UVScaleOffset[2];

} ovr_eye_info_t;

static ovr_eye_info_t renderInfo[2];

static int currentFrame = 0;

int32_t R_GenFBOWithoutTexture(int32_t width, int32_t height, GLenum format, fbo_t *FBO)
{
	GLuint fbo, dep;
	int32_t err;
	glGetError();

	glGenFramebuffersEXT(1, &fbo);
	glGenRenderbuffersEXT(1, &dep);

	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, dep);
	glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, width, height);
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
	err = glGetError();
	if (err != GL_NO_ERROR)
		VID_Printf(PRINT_ALL, "R_GenFBO: Depth buffer creation: glGetError() = 0x%x\n", err);
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);

	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, dep);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, dep);
	err = glGetError();
	if (err != GL_NO_ERROR)
		VID_Printf(PRINT_ALL, "R_GenFBO: FBO creation: glGetError() = 0x%x\n", err);

	FBO->framebuffer = fbo;
	FBO->texture = 0;
	FBO->depthbuffer = dep;
	FBO->width = width;
	FBO->height = height;
	FBO->format = format;
	FBO->status = FBO_VALID | FBO_GENERATED_DEPTH;
	if (format == GL_SRGB8 || format == GL_SRGB8_ALPHA8)
		FBO->status |= FBO_SRGB;

	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, glState.currentFBO->framebuffer);
	return 1;
}


void Rift_CalculateState(vr_param_t *state)
{
	vr_param_t ovrState;
	int eye = 0;

	for (eye = 0; eye < 2; eye++) {
		unsigned int i = 0;
		if (vr_rift_maxfov->value)
		{
			renderInfo[eye].eyeFov = hmd->MaxEyeFov[eye];
		}
		else
		{
			renderInfo[eye].eyeFov = hmd->DefaultEyeFov[eye];
		}

		ovrState.eyeFBO[eye] = &renderInfo[eye].eyeFBO;

		ovrState.renderParams[eye].projection.x.scale = 2.0f / (renderInfo[eye].eyeFov.LeftTan + renderInfo[eye].eyeFov.RightTan);
		ovrState.renderParams[eye].projection.x.offset = (renderInfo[eye].eyeFov.LeftTan - renderInfo[eye].eyeFov.RightTan) * ovrState.renderParams[eye].projection.x.scale * 0.5f;
		ovrState.renderParams[eye].projection.y.scale = 2.0f / (renderInfo[eye].eyeFov.UpTan + renderInfo[eye].eyeFov.DownTan);
		ovrState.renderParams[eye].projection.y.offset = (renderInfo[eye].eyeFov.UpTan - renderInfo[eye].eyeFov.DownTan) * ovrState.renderParams[eye].projection.y.scale * 0.5f;

		// set up rendering info
		eyeDesc[eye] = ovrHmd_GetRenderDesc(hmd, (ovrEyeType) eye, renderInfo[eye].eyeFov);

		VectorSet(ovrState.renderParams[eye].viewOffset,
			-eyeDesc[eye].HmdToEyeViewOffset.x,
			eyeDesc[eye].HmdToEyeViewOffset.y,
			eyeDesc[eye].HmdToEyeViewOffset.z);
	}
	{
		// calculate this to give the engine a rough idea of the fov
		float combinedTanHalfFovHorizontal = max(max(renderInfo[0].eyeFov.LeftTan, renderInfo[0].eyeFov.RightTan), max(renderInfo[1].eyeFov.LeftTan, renderInfo[1].eyeFov.RightTan));
		float combinedTanHalfFovVertical = max(max(renderInfo[0].eyeFov.UpTan, renderInfo[0].eyeFov.DownTan), max(renderInfo[1].eyeFov.UpTan, renderInfo[1].eyeFov.DownTan));
		float horizontalFullFovInRadians = 2.0f * atanf(combinedTanHalfFovHorizontal);
		float fovX = RAD2DEG(horizontalFullFovInRadians);
		float fovY = RAD2DEG(2.0 * atanf(combinedTanHalfFovVertical));
		ovrState.aspect = combinedTanHalfFovHorizontal / combinedTanHalfFovVertical;
		ovrState.viewFovY = fovY;
		ovrState.viewFovX = fovX;
		ovrState.pixelScale = vid.width / (float) hmd->Resolution.w;
	}
	*state = ovrState;
}

void Rift_SetOffscreenSize(uint32_t width, uint32_t height) {
	int i;
	float w, h;
	float ovrScale;
	ovrResult result;
	ovrGLTexture tex;

	Rift_CalculateState(&currentState);

	w = width / (float) hmd->Resolution.w;
	h = height / (float) hmd->Resolution.h;

	ovrScale = (w + h) / 2.0;
	if (vr_rift_debug->value)
		Com_Printf("VR_Rift: Set render target scale to %.2f\n", ovrScale);

	if (swapTextures)
		ovrHmd_DestroySwapTextureSet(hmd, swapTextures);
	if (mirrorTexture)
		ovrHmd_DestroyMirrorTexture(hmd, mirrorTexture);

	result = ovrHmd_CreateSwapTextureSetGL(hmd, GL_SRGB8, width, height, &swapTextures);
	result = ovrHmd_CreateMirrorTextureGL(hmd, GL_SRGB8, width, height, &mirrorTexture);
	currentFBO = 0;
	Com_Printf("Num swap textures: %u\n", swapTextures->TextureCount);

	R_DelFBO(&swapFBO);
	tex.Texture = swapTextures->Textures[0];

	R_GenFBOWithoutTexture(tex.OGL.Header.TextureSize.w, tex.OGL.Header.TextureSize.h, GL_SRGB8, &swapFBO);

	swapLayer.Header.Flags = 0;
	swapLayer.Header.Type = ovrLayerType_EyeFov;

	for (i = 0; i < 2; i++)
	{

		ovrRecti viewport = { { i * swapFBO.width / 2.0, 0 }, { swapFBO.width / 2.0, swapFBO.height } };
		renderInfo[i].renderTarget = ovrHmd_GetFovTextureSize(hmd, (ovrEyeType) i, renderInfo[i].eyeFov, ovrScale);

		if (renderInfo[i].renderTarget.w != renderInfo[i].eyeFBO.width || renderInfo[i].renderTarget.h != renderInfo[i].eyeFBO.height)
		{
			if (vr_rift_debug->value)
				Com_Printf("VR_Rift: Set buffer %i to size %i x %i\n", i, renderInfo[i].renderTarget.w, renderInfo[i].renderTarget.h);
			R_ResizeFBO(renderInfo[i].renderTarget.w, renderInfo[i].renderTarget.h, 1, GL_RGBA8, &renderInfo[i].eyeFBO);
			R_ClearFBO(&renderInfo[i].eyeFBO);
		}
		swapLayer.Viewport[i] = viewport;
		swapLayer.ColorTexture[i] = swapTextures;
		swapLayer.Fov[i] = renderInfo[i].eyeFov;

	}



}

void Rift_FrameStart()
{
	qboolean changed = false;
	if (vr_rift_maxfov->modified)
	{
		int newValue = vr_rift_maxfov->value ? 1 : 0;
		if (newValue != (int) vr_rift_maxfov->value)
			Cvar_SetInteger("vr_rift_maxfov", newValue);
		changed = true;
		vr_rift_maxfov->modified = (qboolean) false;
	}

	if (changed) {
		float scale = R_AntialiasGetScale();
		float w = glConfig.render_width * scale;
		float h = glConfig.render_height * scale;
		Rift_SetOffscreenSize(w, h);
	}
}

void Rift_GetState(vr_param_t *state)
{
	*state = currentState;
}

void R_Clear(void);

void VR_Rift_QuatToEuler(ovrQuatf q, vec3_t e);
void Rift_Present(fbo_t *destination, qboolean loading)
{
	int fade = vr_rift_distortion_fade->value != 0.0f;
	float desaturate = 0.0;

	if (renderExport.positionTracked && trackingState.StatusFlags & ovrStatus_PositionConnected && vr_rift_trackingloss->value > 0) {
		if (renderExport.hasPositionLock) {
			float yawDiff = (fabsf(renderExport.cameraYaw) - 105.0f) * 0.04;
			float xBound, yBound, zBound;
			vec_t temp[4][4], fin[4][4];
			int i = 0;
			vec3_t euler;
			vec4_t pos = { 0.0, 0.0, 0.0, 1.0 };
			vec4_t out = { 0, 0, 0, 0 };
			ovrPosef camera, head;
			vec4_t quat;
			camera = trackingState.CameraPose;
			head = trackingState.HeadPose.ThePose;

			pos[0] = -(head.Position.x - camera.Position.x);
			pos[1] = head.Position.y - camera.Position.y;
			pos[2] = -(head.Position.z - camera.Position.z);

			VR_Rift_QuatToEuler(camera.Orientation, euler);
			EulerToQuat(euler, quat);
			QuatToRotation(quat, temp);
			MatrixMultiply(cameraFrustum, temp, fin);

			for (i = 0; i < 4; i++) {
				out[i] = fin[i][0] * pos[0] + fin[i][1] * pos[1] + fin[i][2] * pos[2] + fin[i][3] * pos[3];
			}

			xBound = (fabsf(out[0]) - 0.6f) * 6.25f;
			yBound = (fabsf(out[1]) - 0.45f) * 6.25f;
			zBound = (fabsf(out[2] - 0.5f) - 0.5f) * 10.0f;

			yawDiff = clamp(yawDiff, 0.0, 1.0);
			xBound = clamp(xBound, 0.0, 1.0);
			yBound = clamp(yBound, 0.0, 1.0);
			zBound = clamp(zBound, 0.0, 1.0);

			desaturate = max(max(max(xBound, yBound), zBound), yawDiff);
		}
		else {
			desaturate = 1.0;
		}
	}

	{
		R_SetupBlit();
		glViewport(0, 0, destination->width / 2.0, destination->height);
		R_BlitTextureToScreen(renderInfo[0].eyeFBO.texture);
		glViewport(destination->width / 2.0, 0, destination->width / 2.0, destination->height);
		R_BlitTextureToScreen(renderInfo[1].eyeFBO.texture);
		R_TeardownBlit();

	}
}

void Rift_DrawToHMD(fbo_t *source)
{
	ovrGLTextureData *tex = (ovrGLTextureData *) &swapTextures->Textures[currentFBO];
	ovrLayerHeader* layers = &swapLayer.Header;
	ovrResult result;
	swapLayer.RenderPose[0] = trackingState.HeadPose.ThePose;
	swapLayer.RenderPose[1] = trackingState.HeadPose.ThePose;

	swapTextures->CurrentIndex = currentFBO;
	R_BindFBO(&swapFBO);
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, tex->TexId, 0);
	R_Clear();
	R_SetupBlit();
	glViewport(0, 0, swapFBO.width / 2.0, swapFBO.height);
	R_BlitWithGammaFlipped(renderInfo[0].eyeFBO.texture, vid_gamma);
	glViewport(swapFBO.width / 2.0, 0, swapFBO.width / 2.0, swapFBO.height);
	R_BlitWithGammaFlipped(renderInfo[0].eyeFBO.texture, vid_gamma);
	R_TeardownBlit();
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, 0, 0);


	result = ovrHmd_SubmitFrame(hmd, 0, NULL, &layers, 1);
	currentFBO = (currentFBO + 1) % swapTextures->TextureCount;
}

void Rift_DrawToScreen(fbo_t *destination)
{
	R_BindFBO(destination);
	R_Clear();
	R_BlitFlipped(mirrorTexture->OGL.TexId);
}

int32_t Rift_Enable(void)
{
	int i;
	eyeScaleOffset_t camera;

	if (!glConfig.arb_texture_float)
		return 0;


	for (i = 0; i < 2; i++)
	{
		if (renderInfo[i].eyeFBO.status)
			R_DelFBO(&renderInfo[i].eyeFBO);
	}

	camera.x.offset = 0.0;
	camera.x.scale = 1.0 / tanf(hmd->CameraFrustumHFovInRadians * 0.5);
	camera.y.offset = 0.0;
	camera.y.scale = 1.0 / tanf(hmd->CameraFrustumVFovInRadians * 0.5);
	R_MakePerspectiveFromScale(camera, hmd->CameraFrustumNearZInMeters, hmd->CameraFrustumFarZInMeters, cameraFrustum);

	Cvar_ForceSet("vr_hmdstring", (char *) hmd->ProductName);
	return true;
}

void Rift_Disable(void)
{
	int i;

	if (swapTextures)
		ovrHmd_DestroySwapTextureSet(hmd, swapTextures);
	if (mirrorTexture)
		ovrHmd_DestroyMirrorTexture(hmd, mirrorTexture);

	for (i = 0; i < 2; i++)
	{
		if (renderInfo[i].eyeFBO.status)
			R_DelFBO(&renderInfo[i].eyeFBO);
	}
}

int32_t Rift_Init(void)
{
	int i;
	for (i = 0; i < 2; i++)
	{
		R_InitFBO(&renderInfo[i].eyeFBO);
	}
	return true;
}