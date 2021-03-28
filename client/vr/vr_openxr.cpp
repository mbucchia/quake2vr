#include <algorithm>
#include <string>
#include <vector>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

extern "C" {
#include "include/vr.h"
#include "../renderer/include/r_vr.h"
}

#define XR_USE_PLATFORM_WIN32

#define XR_USE_GRAPHICS_API_OPENGL
#include "../renderer/include/qgl.h"

// We might have to use interoperability with Direct3D depending on the runtime support.
#define XR_USE_GRAPHICS_API_D3D11
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define stringify(s) xstringify(s)
#define xstringify(s) #s

#define ThrowIfXrFailed(stmt)																	\
    do {																						\
        if (XR_FAILED((stmt))) {																\
            Com_Printf(__FILE__ ":" stringify(__LINE__) ": failed to " stringify(stmt) "\n");	\
            throw std::exception("XR call failed");												\
        }																						\
    } while (0);

#define ThrowIfHFailed(stmt)																	\
    do {																						\
        if (FAILED((stmt))) {																	\
            Com_Printf(__FILE__ ":" stringify(__LINE__) ": failed to " stringify(stmt) "\n");	\
            throw std::exception("DX call failed");												\
        }																						\
    } while (0);

#define ThrowIfNull(stmt)																	\
    do {																					\
        if (!(stmt)) {																		\
            Com_Printf(__FILE__ ":" stringify(__LINE__) ": " stringify(stmt) " is null\n");	\
            throw std::exception("call failed");											\
        }																					\
    } while (0);

#define Throw(message)                                                      \
    do {                                                                    \
            Com_Printf(__FILE__ ":" stringify(__LINE__) ": " message "\n");	\
            throw std::exception((message));                                \
    } while (0);

namespace {

	class VrOpenXr {
	private:
		VrOpenXr() {
		}

		// Create the OpenXR instance.
		int32_t Init() {
			// TODO: Need glew to be initialized before we get here.

			// Check for either OpenGL or D3D11 support.
			uint32_t extensionsCount = 0;
			ThrowIfXrFailed(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionsCount, nullptr));
			std::vector<XrExtensionProperties> extensions(extensionsCount, { XR_TYPE_EXTENSION_PROPERTIES });
			ThrowIfXrFailed(xrEnumerateInstanceExtensionProperties(nullptr, extensionsCount, &extensionsCount, extensions.data()));
			bool hasOpenGlSupport = false, hasD3DSupport = false;
			for (auto extension : extensions) {
				if (std::string(extension.extensionName) == "XR_KHR_D3D11_enable") {
					hasD3DSupport = true;
				} else if (std::string(extension.extensionName) == "XR_KHR_opengl_enable") {
					hasOpenGlSupport = true;
				}
			}

			// D3D11 interop needs OpenGL's NV_DX_interop2.
			if (!hasOpenGlSupport && !WGLEW_NV_DX_interop2) {
				Com_Printf("VR_OpenXR: OpenGL driver does not support NV_DX_interop2\n");
				return 0;
			}

			// Create the instance.
			XrInstanceCreateInfo instanceInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
			const char* extensionsWanted[1];
			if (hasOpenGlSupport) {
				extensionsWanted[0] = "XR_KHR_opengl_enable";
			} else if (hasD3DSupport) {
				extensionsWanted[0] = "XR_KHR_D3D11_enable";
				m_useD3DInterop = true;
			} else {
				Com_Printf("VR_OpenXR: Runtime has support for neither OpenGL nor D3D11\n");
				return 0;
			}
			instanceInfo.enabledExtensionCount = 1;
			instanceInfo.enabledExtensionNames = extensionsWanted;
			instanceInfo.enabledApiLayerCount = 0;
			instanceInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
			snprintf(instanceInfo.applicationInfo.applicationName, sizeof(instanceInfo.applicationInfo.applicationName), "TestXr");

			ThrowIfXrFailed(xrCreateInstance(&instanceInfo, &m_xrInstance));

			XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
			systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

			XrSystemId system;
			ThrowIfXrFailed(xrGetSystem(m_xrInstance, &systemInfo, &system));

			// Check for stereoscopic (VR) view.
			uint32_t viewConfigurationsCount = 0;
			ThrowIfXrFailed(xrEnumerateViewConfigurations(m_xrInstance, system, 0, &viewConfigurationsCount, nullptr));
			std::vector<XrViewConfigurationType> viewConfigurations(viewConfigurationsCount);
			ThrowIfXrFailed(xrEnumerateViewConfigurations(m_xrInstance, system, viewConfigurationsCount, &viewConfigurationsCount, viewConfigurations.data()));
			const bool hasStereoSupport = std::count(viewConfigurations.cbegin(), viewConfigurations.cend(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);

			if (!hasStereoSupport) {
				Com_Printf("VR_OpenXR: System does not support VR\n");
				xrDestroyInstance(m_xrInstance);
				m_xrInstance = XR_NULL_HANDLE;
				return 0;
			}

			// Check for opaque blend mode.
			uint32_t blendModesCount = 0;
			ThrowIfXrFailed(xrEnumerateEnvironmentBlendModes(m_xrInstance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &blendModesCount, nullptr));
			std::vector<XrEnvironmentBlendMode> blendModes(blendModesCount);
			ThrowIfXrFailed(xrEnumerateEnvironmentBlendModes(m_xrInstance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, blendModesCount, &blendModesCount, blendModes.data()));
			const bool hasOpaqueBlendSupport = std::count(blendModes.cbegin(), blendModes.cend(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE);

			if (!hasOpaqueBlendSupport) {
				Com_Printf("VR_OpenXR: System does not opaque blend mode\n");
				xrDestroyInstance(m_xrInstance);
				m_xrInstance = XR_NULL_HANDLE;
				return 0;
			}

			return 1;
		}

		// Cleanup everything created by Init() and Enable().
		void Shutdown() {
			for (unsigned int eye = 0; eye < 2; eye++) {
				xrDestroySwapchain(m_xrSwapchain[eye]);

				if (m_useD3DInterop) {
					m_d3dRenderBufferRtv[eye].clear();
					m_d3dIntermediateBufferSrv[eye].clear();

					m_d3dIntermediateBuffer[eye].clear();

					for (auto dxglInteropRenderBuffer : m_dxglInteropRenderBuffer[eye]) {
						wglDXUnregisterObjectNV(m_dxglInterop, dxglInteropRenderBuffer);
					}
					m_dxglInteropRenderBuffer[eye].clear();

					for (auto glRenderBuffer : m_glRenderBuffer[eye]) {
						glDeleteTextures(1, &glRenderBuffer);
					}
					m_glRenderBuffer[eye].clear();
				}
			}

			if (m_useD3DInterop) {
				wglDXCloseDeviceNV(m_dxglInterop);

				m_d3dFlipVertexShader = nullptr;
				m_d3dFlipPixelShader = nullptr;
				m_d3dSampler = nullptr;
				m_d3dRasterizer = nullptr;

				m_d3dDeviceContext = nullptr;
				m_d3dDevice = nullptr;
			}

			xrDestroySession(m_xrSession);
			m_xrSession = XR_NULL_HANDLE;

			xrDestroyInstance(m_xrInstance);
			m_xrInstance = XR_NULL_HANDLE;
		}

		// Create the OpenXR session.
		int32_t Enable() {
			if (m_xrInstance == XR_NULL_HANDLE) {
				return 0;
			}

			if (m_xrSession == XR_NULL_HANDLE) {
				XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
				systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

				XrSystemId system;
				ThrowIfXrFailed(xrGetSystem(m_xrInstance, &systemInfo, &system));

				XrSystemProperties systemProperties = { XR_TYPE_SYSTEM_PROPERTIES };
				ThrowIfXrFailed(xrGetSystemProperties(m_xrInstance, system, &systemProperties));

				Com_Printf("VR_OpenXR: Using OpenXR device %s \n", systemProperties.systemName);

				// Create the graphics resources.
				if (m_useD3DInterop) {
					Com_Printf("VR_OpenXR: Enabling D3D11 interop\n");

					PFN_xrGetD3D11GraphicsRequirementsKHR ext_xrGetD3D11GraphicsRequirementsKHR = nullptr;
					ThrowIfXrFailed(xrGetInstanceProcAddr(m_xrInstance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&ext_xrGetD3D11GraphicsRequirementsKHR));

					XrGraphicsRequirementsD3D11KHR d3dRequirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
					ThrowIfXrFailed(ext_xrGetD3D11GraphicsRequirementsKHR(m_xrInstance, systemProperties.systemId, &d3dRequirements));

					InitD3DResources(d3dRequirements.adapterLuid, D3D_FEATURE_LEVEL_11_0);
				}
				InitGLResources();

				// Create the session.
				XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
				sessionInfo.systemId = systemProperties.systemId;

				int64_t preferredSwapchainFormat;
				if (m_useD3DInterop) {
					XrGraphicsBindingD3D11KHR d3dBindings = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
					d3dBindings.device = m_d3dDevice.Get();

					sessionInfo.next = &d3dBindings;
					preferredSwapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
				}
				else {
					// TODO: Support for standalone OpenGL initialization with OpenXR.
				}

				ThrowIfXrFailed(xrCreateSession(m_xrInstance, &sessionInfo, &m_xrSession));

				// Create the swapchains.
				// TODO: Add support for depth swapchains so we get better reprojection?
				uint32_t viewsCount = 0;
				ThrowIfXrFailed(xrEnumerateViewConfigurationViews(m_xrInstance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewsCount, nullptr));
				std::vector<XrViewConfigurationView> views(viewsCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
				ThrowIfXrFailed(xrEnumerateViewConfigurationViews(m_xrInstance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewsCount, &viewsCount, views.data()));
				uint32_t preferredSwapchainWidth = 0, preferredSwapchainHeight;
				for (auto view : views) {
					if (preferredSwapchainWidth == 0) {
						preferredSwapchainWidth = view.recommendedImageRectWidth;
						preferredSwapchainHeight = view.recommendedImageRectHeight;
					}
				}

				for (unsigned int eye = 0; eye < 2; eye++) {
					XrSwapchainCreateInfo swapchainInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
					swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
					swapchainInfo.format = preferredSwapchainFormat;
					swapchainInfo.sampleCount = 1;
					swapchainInfo.width = preferredSwapchainWidth;
					swapchainInfo.height = preferredSwapchainHeight;
					swapchainInfo.faceCount = 1;
					swapchainInfo.arraySize = 1;
					swapchainInfo.mipCount = 1;
					ThrowIfXrFailed(xrCreateSwapchain(m_xrSession, &swapchainInfo, &m_xrSwapchain[eye]));
					m_xrSwapchainSize.offset.x = m_xrSwapchainSize.offset.y = 0;
					m_xrSwapchainSize.extent.width = preferredSwapchainWidth;
					m_xrSwapchainSize.extent.height = preferredSwapchainHeight;
					m_xrSwapchainFormat = preferredSwapchainFormat;

					if (m_useD3DInterop) {
						uint32_t swapchainImagesCount = 0;
						ThrowIfXrFailed(xrEnumerateSwapchainImages(m_xrSwapchain[eye], 0, &swapchainImagesCount, nullptr));
						std::vector<XrSwapchainImageD3D11KHR> swapchainImages(swapchainImagesCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
						ThrowIfXrFailed(xrEnumerateSwapchainImages(m_xrSwapchain[eye], swapchainImagesCount, &swapchainImagesCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchainImages.data())));

						InitD3DSwapchainImages(eye, swapchainImages);
					} else {
						uint32_t swapchainImagesCount = 0;
						ThrowIfXrFailed(xrEnumerateSwapchainImages(m_xrSwapchain[eye], 0, &swapchainImagesCount, nullptr));
						std::vector<XrSwapchainImageOpenGLKHR> swapchainImages(swapchainImagesCount, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
						ThrowIfXrFailed(xrEnumerateSwapchainImages(m_xrSwapchain[eye], swapchainImagesCount, &swapchainImagesCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchainImages.data())));

						for (auto swapchainImage : swapchainImages) {
							m_glRenderBuffer[eye].push_back(swapchainImage.image);
						}
					}
				}

				XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
				spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
				spaceInfo.poseInReferenceSpace.orientation.w = 1;
				ThrowIfXrFailed(xrCreateReferenceSpace(m_xrSession, &spaceInfo, &m_xrSpace));
			}

			// TODO: Check whether we can invoke xrBeginSession() right away.

			return 1;
		}

		// Forcefully leave the OpenXR session.
		void Disable() {
			// TODO: Invoke xrRequestExitSession().
		}

		// Acquire the next frame.
		void FrameStart() {
			// Process session events.
			while (true) {
				XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
				const XrResult poll = xrPollEvent(m_xrInstance, &event);
				if (poll == XR_EVENT_UNAVAILABLE) {
					break;
				}
				ThrowIfXrFailed(poll);

				switch (event.type) {
				case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
						const XrEventDataSessionStateChanged* stateEvent = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
						switch (stateEvent->state) {
						case XR_SESSION_STATE_READY:
							{
								XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
								beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
								Com_Printf("VR_OpenXR: Beginning session\n");
								ThrowIfXrFailed(xrBeginSession(m_xrSession, &beginInfo));
							}
							break;

						case XR_SESSION_STATE_STOPPING:
							Com_Printf("VR_OpenXR: Session is stopping\n");
							ThrowIfXrFailed(xrEndSession(m_xrSession));
							return;

						case XR_SESSION_STATE_EXITING:
						case XR_SESSION_STATE_LOSS_PENDING:
							return;
						}
					}
					break;

				case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
					Com_Printf("VR_OpenXR: Instance lost\n");
					return;
				}
			}

			// Wait for the next frame to be ready.
			XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
			ThrowIfXrFailed(xrWaitFrame(m_xrSession, &waitInfo, &m_xrFrameState));

			XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
			ThrowIfXrFailed(xrBeginFrame(m_xrSession, &beginInfo));

			// Cache the swapchain objects for the upcoming calls to GetViewState().
			for (unsigned int eye = 0; eye < 2; eye++) {
				XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
				ThrowIfXrFailed(xrAcquireSwapchainImage(m_xrSwapchain[eye], &acquireInfo, &m_currentSwapchainImageIndex[eye]));

				XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
				waitInfo.timeout = XR_INFINITE_DURATION;
				ThrowIfXrFailed(xrWaitSwapchainImage(m_xrSwapchain[eye], &waitInfo));

				if (m_useD3DInterop) {
					wglDXLockObjectsNV(m_dxglInterop, 1, &m_dxglInteropRenderBuffer[eye][m_currentSwapchainImageIndex[eye]]);
				}

				// m_glRenderBuffer[eye][m_currentSwapchainImageIndex[eye]] is the OpenGL swapchain texture to use.
			}

			// Cache the head pose for the upcoming calls to GetViewState(), GetOrientation(), GetPosition(), etc.
			XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
			locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			locateInfo.space = m_xrSpace;
			locateInfo.displayTime = m_xrFrameState.predictedDisplayTime;

			XrViewState viewState = { XR_TYPE_VIEW_STATE };
			uint32_t viewsCount = 2;
			ThrowIfXrFailed(xrLocateViews(m_xrSession, &locateInfo, &viewState, viewsCount, &viewsCount, m_xrViews));
		}

		// Submit the current frame.
		void FrameEnd() {
			// Do the work needed for interop with D3D.
			if (m_useD3DInterop) {
				for (unsigned int eye = 0; eye < 2; eye++) {
					wglDXUnlockObjectsNV(m_dxglInterop, 1, &m_dxglInteropRenderBuffer[eye][m_currentSwapchainImageIndex[eye]]);

					// Flip the rendered image vertically.
					ID3D11RenderTargetView* rtvs[] = { m_d3dRenderBufferRtv[eye][m_currentSwapchainImageIndex[eye]].Get() };
					m_d3dDeviceContext->OMSetRenderTargets(1, rtvs, nullptr);
					m_d3dDeviceContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
					m_d3dDeviceContext->OMSetDepthStencilState(nullptr, 0);
					m_d3dDeviceContext->VSSetShader(m_d3dFlipVertexShader.Get(), nullptr, 0);
					m_d3dDeviceContext->PSSetShader(m_d3dFlipPixelShader.Get(), nullptr, 0);
					ID3D11ShaderResourceView* srvs[] = { m_d3dIntermediateBufferSrv[eye][m_currentSwapchainImageIndex[eye]].Get() };
					m_d3dDeviceContext->PSSetShaderResources(0, 1, srvs);
					ID3D11SamplerState* ss[] = { m_d3dSampler.Get() };
					m_d3dDeviceContext->PSSetSamplers(0, 1, ss);
					m_d3dDeviceContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
					m_d3dDeviceContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
					m_d3dDeviceContext->IASetInputLayout(nullptr);
					m_d3dDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

					D3D11_VIEWPORT vp;
					vp.TopLeftX = (float)m_xrSwapchainSize.offset.x;
					vp.TopLeftY = (float)m_xrSwapchainSize.offset.y;
					vp.Width = (float)m_xrSwapchainSize.extent.width;
					vp.Height = (float)m_xrSwapchainSize.extent.height;
					vp.MinDepth = D3D11_MIN_DEPTH;
					vp.MaxDepth = D3D11_MAX_DEPTH;
					m_d3dDeviceContext->RSSetViewports(1, &vp);
					m_d3dDeviceContext->RSSetState(m_d3dRasterizer.Get());

					m_d3dDeviceContext->Draw(4, 0);
				}
			}

			// Create the submission to the compositor.
			XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
			layer.space = m_xrSpace;

			XrCompositionLayerProjectionView projectionViews[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
			layer.views = projectionViews;
			layer.viewCount = 2;

			for (unsigned int eye = 0; eye < 2; eye++) {
				projectionViews[eye].pose = m_xrViews[eye].pose;
				projectionViews[eye].fov = m_xrViews[eye].fov;
				projectionViews[eye].subImage.swapchain = m_xrSwapchain[eye];
				projectionViews[eye].subImage.imageRect = m_xrSwapchainSize;

				XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				ThrowIfXrFailed(xrReleaseSwapchainImage(m_xrSwapchain[eye], &releaseInfo));
			}

			// Submit the frame!
			XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
			endInfo.displayTime = m_xrFrameState.predictedDisplayTime;
			endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			const XrCompositionLayerBaseHeader* const layers[] = { reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer) };
			endInfo.layers = layers;
			endInfo.layerCount = m_xrFrameState.shouldRender ? 1 : 0;
			endInfo.layerCount = 0;
			ThrowIfXrFailed(xrEndFrame(m_xrSession, &endInfo));
		}

		int32_t SetPredictionTime(float timeInMs) {
			// TODO.
			return 0;
		}

		int32_t GetOrientation(float euler[3]) {
			// TODO.
			return 0;
		}

		int32_t GetHeadOffset(float offset[3]) {
			// TODO.
			return 0;
		}

		void GetPosition(int32_t* xpos, int32_t* ypos) {
			// TODO.
		}

		void GetResolution(int32_t* width, int32_t* height) {
			// TODO.
		}

		void GetViewState(vr_param_t* state) {
			// TODO.
		}

		// Initialize the D3D resources needed for interop with OpenGL.
		void InitD3DResources(const LUID requestedAdapter, const D3D_FEATURE_LEVEL requestedFeatureLevel) {
			// Create the D3D device that we will interop with.
			ComPtr<IDXGIFactory1> dxgi;
			ThrowIfHFailed(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi)));

			ComPtr<IDXGIAdapter> dxgiAdapter;
			bool adapterValid = false;
			UINT adapterIndex = 0;
			while (true) {
				if (dxgi->EnumAdapters(adapterIndex, dxgiAdapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) {
					break;
				}

				DXGI_ADAPTER_DESC adapterDesc;
				ThrowIfHFailed(dxgiAdapter->GetDesc(&adapterDesc));
				if (!memcmp(&adapterDesc.AdapterLuid, &requestedAdapter, sizeof(requestedAdapter))) {
					adapterValid = true;
					break;
				}

				adapterIndex++;
			}

			if (!adapterValid) {
				Throw("Cannot find adapter");
			}

			UINT deviceFlags = 0;
#ifdef _DEBUG
			deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
			ThrowIfHFailed(D3D11CreateDevice(dxgiAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, deviceFlags, &requestedFeatureLevel, 1, D3D11_SDK_VERSION, m_d3dDevice.ReleaseAndGetAddressOf(), nullptr, m_d3dDeviceContext.ReleaseAndGetAddressOf()));

			// Create the shaders to flip the rendered image vertically.
			static const char flipShadersSource[] = R"_(
Texture2D src;
SamplerState smpler;

void vsMain(uint vertexID : SV_VertexID,
            out float4 position : SV_Position,
            out float2 texcoord : TEXCOORD0) {
    /*
     * UV coordinates are 0, 0 for top-left corner and 1, 1 for bottom-right corner.
     * Create a quad with texture mapped upside-down:
     *
     *                     ^ u
     *                     |
     *   v0 (0, 1)         |         v1 (1, 1)
     *                     |
     *   -------------------------------------->
     *                     |                    v
     *   v2 (0, 0)         |         v3 (1, 0)
     *                     |
     */
    const float2 quadPositions[4] = {
        float2(-1.0f, +1.0f),
        float2(+1.0f, +1.0f),
        float2(-1.0f, -1.0f),
        float2(+1.0f, -1.0f),
    };
    const float2 quadTexcoords[4] = {
        float2(0.0f, 1.0f),
        float2(1.0f, 1.0f),
        float2(0.0f, 0.0f),
        float2(1.0f, 0.0f),
    };
    position = float4(quadPositions[vertexID], 0.0f, 1.0f);
    texcoord = quadTexcoords[vertexID];
}

float4 psMain(in float4 position : SV_POSITION,
            in float2 texcoord : TEXCOORD0) : SV_TARGET {
    return src.Sample(smpler, texcoord);
}
    )_";

			ComPtr<ID3DBlob> errors;

			ComPtr<ID3DBlob> vsBytes;
			HRESULT hr = D3DCompile(flipShadersSource, sizeof(flipShadersSource), nullptr, nullptr, nullptr, "vsMain", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, vsBytes.GetAddressOf(), errors.GetAddressOf());
			if (FAILED(hr)) {
				Com_Printf("VR_OpenXR: VS compile failed: %*s\n", errors->GetBufferSize(), errors->GetBufferPointer());
				ThrowIfHFailed(hr);
			}
			ThrowIfHFailed(m_d3dDevice->CreateVertexShader(vsBytes->GetBufferPointer(), vsBytes->GetBufferSize(), nullptr, m_d3dFlipVertexShader.GetAddressOf()));

			ComPtr<ID3DBlob> psBytes;
			hr = D3DCompile(flipShadersSource, sizeof(flipShadersSource), nullptr, nullptr, nullptr, "psMain", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, psBytes.GetAddressOf(), errors.ReleaseAndGetAddressOf());
			if (FAILED(hr)) {
				Com_Printf("VR_OpenXR: PS compile failed: %*s\n", errors->GetBufferSize(), errors->GetBufferPointer());
				ThrowIfHFailed(hr);
			}
			ThrowIfHFailed(m_d3dDevice->CreatePixelShader(psBytes->GetBufferPointer(), psBytes->GetBufferSize(), nullptr, m_d3dFlipPixelShader.GetAddressOf()));

			// Create the resources needed to invoke the shaders.
			// Note that the RTV and SRV for each swapchain images are created in InitD3DSwapchainImages().
			D3D11_SAMPLER_DESC sampDesc;
			sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampDesc.MipLODBias = 0;
			sampDesc.MaxAnisotropy = 1;
			sampDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
			sampDesc.MinLOD = 0;
			sampDesc.MaxLOD = 0;
			ThrowIfHFailed(m_d3dDevice->CreateSamplerState(&sampDesc, m_d3dSampler.GetAddressOf()));

			D3D11_RASTERIZER_DESC rsDesc;
			rsDesc.FillMode = D3D11_FILL_SOLID;
			rsDesc.CullMode = D3D11_CULL_NONE;
			rsDesc.FrontCounterClockwise = TRUE;
			rsDesc.DepthBias = 0;
			rsDesc.DepthBiasClamp = 0;
			rsDesc.SlopeScaledDepthBias = 0;
			rsDesc.DepthClipEnable = FALSE;
			rsDesc.ScissorEnable = FALSE;
			rsDesc.MultisampleEnable = FALSE;
			rsDesc.AntialiasedLineEnable = FALSE;
			ThrowIfHFailed(m_d3dDevice->CreateRasterizerState(&rsDesc, m_d3dRasterizer.GetAddressOf()));
		}

		// Initialize the OpenGL and D3D resources needed to write to the OpenXR swapchains.
		void InitD3DSwapchainImages(unsigned int eye, std::vector<XrSwapchainImageD3D11KHR>& swapchainImages) {
			for (auto swapchainImage : swapchainImages) {
				// Create the interop textures.
				D3D11_TEXTURE2D_DESC textureDesc = {};
				textureDesc.Width = m_xrSwapchainSize.extent.width;
				textureDesc.Height = m_xrSwapchainSize.extent.height;
				textureDesc.MipLevels = 1;
				textureDesc.ArraySize = 1;
				textureDesc.Format = (DXGI_FORMAT)m_xrSwapchainFormat;
				textureDesc.SampleDesc.Count = 1;
				textureDesc.Usage = D3D11_USAGE_DEFAULT;
				textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
				ComPtr<ID3D11Texture2D> d3dBuffer;
				ThrowIfHFailed(m_d3dDevice->CreateTexture2D(&textureDesc, NULL, d3dBuffer.GetAddressOf()));

				GLuint glBuffer;
				glGenTextures(1, &glBuffer);

				glBindTexture(GL_TEXTURE_2D, glBuffer);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_xrSwapchainSize.extent.width, m_xrSwapchainSize.extent.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

				HANDLE interopBuffer = wglDXRegisterObjectNV(m_dxglInterop, d3dBuffer.Get(), glBuffer, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
				ThrowIfNull(interopBuffer);

				// Create the RTV and SRV for flipping of the rendered image vertically.
				D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
				rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				rtvDesc.Texture2D.MipSlice = 0;
				ComPtr<ID3D11RenderTargetView> swapchainImageRtv;
				ThrowIfHFailed(m_d3dDevice->CreateRenderTargetView(swapchainImage.texture, &rtvDesc, swapchainImageRtv.GetAddressOf()));

				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
				srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MipLevels = 1;
				srvDesc.Texture2D.MostDetailedMip = 0;
				ComPtr<ID3D11ShaderResourceView> d3dBufferSrv;
				ThrowIfHFailed(m_d3dDevice->CreateShaderResourceView(d3dBuffer.Get(), &srvDesc, d3dBufferSrv.GetAddressOf()));

				m_d3dRenderBuffer[eye].push_back(swapchainImage.texture);

				m_d3dIntermediateBuffer[eye].push_back(d3dBuffer);
				m_dxglInteropRenderBuffer[eye].push_back(interopBuffer);
				m_glRenderBuffer[eye].push_back(glBuffer);

				m_d3dRenderBufferRtv[eye].push_back(swapchainImageRtv);
				m_d3dIntermediateBufferSrv[eye].push_back(d3dBufferSrv);
			}
		}

		// Initialize the OpenGL resources, including the ones needed for interop with D3D.
		void InitGLResources() {
			if (m_useD3DInterop) {
				m_dxglInterop = wglDXOpenDeviceNV(m_d3dDevice.Get());
				ThrowIfNull(m_dxglInterop);
			}

			// TODO: Setup framebuffer objects for GetViewState().
		}

		// Singleton.
		static VrOpenXr* getInstance() {
			if (!instance) {
				instance = new VrOpenXr();
			}
			return instance;
		}

		// OpenXR resources.
		XrInstance m_xrInstance{ XR_NULL_HANDLE };
		XrSession m_xrSession{ XR_NULL_HANDLE };
		XrSwapchain m_xrSwapchain[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
		int64_t m_xrSwapchainFormat;
		XrRect2Di m_xrSwapchainSize;
		XrSpace m_xrSpace;
		XrFrameState m_xrFrameState = { XR_TYPE_FRAME_STATE };
		XrView m_xrViews[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };

		// D3D resources.
		ComPtr<ID3D11Device> m_d3dDevice;
		ComPtr<ID3D11DeviceContext> m_d3dDeviceContext;
		std::vector<ID3D11Texture2D*> m_d3dRenderBuffer[2];
		std::vector<ComPtr<ID3D11Texture2D>> m_d3dIntermediateBuffer[2];
		std::vector<ComPtr<ID3D11RenderTargetView>> m_d3dRenderBufferRtv[2];
		std::vector<ComPtr<ID3D11ShaderResourceView>> m_d3dIntermediateBufferSrv[2];
		ComPtr<ID3D11VertexShader> m_d3dFlipVertexShader;
		ComPtr<ID3D11PixelShader> m_d3dFlipPixelShader;
		ComPtr<ID3D11SamplerState> m_d3dSampler;
		ComPtr<ID3D11RasterizerState> m_d3dRasterizer;

		// NV_DX_interop2 resources.
		HANDLE m_dxglInterop;
		std::vector<HANDLE> m_dxglInteropRenderBuffer[2];

		// OpenGL resources.
		std::vector<GLuint> m_glRenderBuffer[2];

		// General state.
		unsigned int m_currentSwapchainImageIndex[2];
		bool m_useD3DInterop{ false };

		static VrOpenXr* instance;

	public:
		// Wrappers to interact with the vr_main/r_vr abstractions.

		static int32_t wrapper_init() {
			try {
				return getInstance()->Init();
			} catch (std::exception exc) {
				Com_Printf("VR_OpenXR: Error initializing OpenXR support\n");
				return 0;
			}
		}
		
		static void wrapper_shutdown() {
			getInstance()->Shutdown();
		}

		static int32_t wrapper_enable() {
			try {
				return getInstance()->Enable();
			}
			catch (std::exception exc) {
				Com_Printf("VR_OpenXR: Error enabling HMD\n");
				return 0;
			}
		}
		
		static void wrapper_disable() {
			getInstance()->Disable();
		}

		static void wrapper_frameStart() {
			getInstance()->FrameStart();
		}

		static void wrapper_frameEnd() {
			getInstance()->FrameEnd();
		}

		static int32_t wrapper_setPredictionTime(float timeInMs) {
			return getInstance()->SetPredictionTime(timeInMs);
		}

		static int32_t wrapper_getOrientation(float euler[3]) {
			return getInstance()->GetOrientation(euler);
		}

		static int32_t wrapper_getHeadOffset(float offset[3]) {
			return getInstance()->GetHeadOffset(offset);
		}

		static void wrapper_getPosition(int32_t* xpos, int32_t* ypos) {
			getInstance()->GetPosition(xpos, ypos);
		}

		static void wrapper_getResolution(int32_t* width, int32_t* height) {
			getInstance()->GetResolution(width, height);
		}

		static void wrapper_getViewState(vr_param_t* state) {
			getInstance()->GetViewState(state);
		}
	};

	VrOpenXr* VrOpenXr::instance = nullptr;

}

extern "C" hmd_interface_t hmd_openxr = {
	HMD_OPENXR,
	VrOpenXr::wrapper_init,
	VrOpenXr::wrapper_shutdown,
	VrOpenXr::wrapper_enable,
	VrOpenXr::wrapper_disable,
	VrOpenXr::wrapper_frameStart,
	VrOpenXr::wrapper_frameEnd,
	nullptr,
	VrOpenXr::wrapper_getOrientation,
	VrOpenXr::wrapper_getHeadOffset,
	VrOpenXr::wrapper_setPredictionTime,
	VrOpenXr::wrapper_getPosition,
	VrOpenXr::wrapper_getResolution,
};

// We don't duplicate the calls to Enable/Disable, FrameStart and Present, because rendering is tightly-coupled to frames in OpenXR.
extern "C" hmd_render_t vr_render_openxr =
{
	HMD_OPENXR,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	VrOpenXr::wrapper_getViewState,
	nullptr,
	nullptr,
};
