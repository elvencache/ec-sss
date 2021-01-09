/*
* Copyright 2021 elven cache. All rights reserved.
* License: https://github.com/bkaradzic/bgfx#license-bsd-2-clause
*/

/*
* Implement screen space shadows as bgfx example. Goal is to explore various
* options and parameters. Includes toggle for "soft contact shadows", inspired
* by screen space ambient occlusion. These could fit better with the look of
* your scene if you already have some kind of soft shadows.
*/


#include <common.h>
#include <camera.h>
#include <bgfx_utils.h>
#include <imgui/imgui.h>
#include <bx/rng.h>
#include <bx/os.h>


namespace {

// Gbuffer has multiple render targets
#define GBUFFER_RT_COLOR		0
#define GBUFFER_RT_NORMAL		1
#define GBUFFER_RT_DEPTH		2
#define GBUFFER_RENDER_TARGETS	3

#define MODEL_COUNT				100

static const char * s_meshPaths[] =
{
	"meshes/unit_sphere.bin",
	"meshes/column.bin",
	"meshes/tree.bin",
	"meshes/hollowcube.bin",
	"meshes/bunny.bin"
};

static const float s_meshScale[] =
{
	0.25f,
	0.05f,
	0.15f,
	0.25f,
	0.25f
};

// Vertex decl for our screen space quad (used in deferred rendering)
struct PosTexCoord0Vertex
{
	float m_x;
	float m_y;
	float m_z;
	float m_u;
	float m_v;

	static void init()
	{
		ms_layout
			.begin()
			.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
			.end();
	}

	static bgfx::VertexLayout ms_layout;
};

bgfx::VertexLayout PosTexCoord0Vertex::ms_layout;

struct Uniforms
{
	enum { NumVec4 = 12 };

	void init() {
		u_params = bgfx::createUniform("u_params", bgfx::UniformType::Vec4, NumVec4);
	};

	void submit() const {
		bgfx::setUniform(u_params, m_params, NumVec4);
	}

	void destroy() {
		bgfx::destroy(u_params);
	}

	union
	{
		struct
		{
			/* 0    */ struct { float m_frameIdx; float m_shadowRadius; float m_shadowSteps; float m_useNoiseOffset; };
			/* 1    */ struct { float m_depthUnpackConsts[2]; float m_contactShadowsMode; float m_useScreenSpaceRadius; };
			/* 2    */ struct { float m_ndcToViewMul[2]; float m_ndcToViewAdd[2]; };
			/* 3    */ struct { float m_lightPosition[3]; float m_displayShadows; };
			/* 4-7  */ struct { float m_worldToView[16]; }; // built-in u_view will be transform for quad during screen passes
			/* 8-11 */ struct { float m_viewToProj[16]; };	 // built-in u_proj will be transform for quad during screen passes
		};

		float m_params[NumVec4 * 4];
	};

	bgfx::UniformHandle u_params;
};

struct RenderTarget
{
	void init(uint32_t _width, uint32_t _height, bgfx::TextureFormat::Enum _format, uint64_t _flags)
	{
		m_texture = bgfx::createTexture2D(uint16_t(_width), uint16_t(_height), false, 1, _format, _flags);
		const bool destroyTextures = true;
		m_buffer = bgfx::createFrameBuffer(1, &m_texture, destroyTextures);
	}

	void destroy()
	{
		// also responsible for destroying texture
		bgfx::destroy(m_buffer);
	}

	bgfx::TextureHandle m_texture;
	bgfx::FrameBufferHandle m_buffer;
};

void screenSpaceQuad(float _textureWidth, float _textureHeight, float _texelHalf, bool _originBottomLeft, float _width = 1.0f, float _height = 1.0f)
{
	if (3 == bgfx::getAvailTransientVertexBuffer(3, PosTexCoord0Vertex::ms_layout))
	{
		bgfx::TransientVertexBuffer vb;
		bgfx::allocTransientVertexBuffer(&vb, 3, PosTexCoord0Vertex::ms_layout);
		PosTexCoord0Vertex* vertex = (PosTexCoord0Vertex*)vb.data;

		const float minx = -_width;
		const float maxx =  _width;
		const float miny = 0.0f;
		const float maxy =  _height * 2.0f;

		const float texelHalfW = _texelHalf / _textureWidth;
		const float texelHalfH = _texelHalf / _textureHeight;
		const float minu = -1.0f + texelHalfW;
		const float maxu =  1.0f + texelHalfW;

		const float zz = 0.0f;

		float minv = texelHalfH;
		float maxv = 2.0f + texelHalfH;

		if (_originBottomLeft)
		{
			float temp = minv;
			minv = maxv;
			maxv = temp;

			minv -= 1.0f;
			maxv -= 1.0f;
		}

		vertex[0].m_x = minx;
		vertex[0].m_y = miny;
		vertex[0].m_z = zz;
		vertex[0].m_u = minu;
		vertex[0].m_v = minv;

		vertex[1].m_x = maxx;
		vertex[1].m_y = miny;
		vertex[1].m_z = zz;
		vertex[1].m_u = maxu;
		vertex[1].m_v = minv;

		vertex[2].m_x = maxx;
		vertex[2].m_y = maxy;
		vertex[2].m_z = zz;
		vertex[2].m_u = maxu;
		vertex[2].m_v = maxv;

		bgfx::setVertexBuffer(0, &vb);
	}
}

void vec2Set(float* _v, float _x, float _y)
{
	_v[0] = _x;
	_v[1] = _y;
}

void vec4Set(float* _v, float _x, float _y, float _z, float _w)
{
	_v[0] = _x;
	_v[1] = _y;
	_v[2] = _z;
	_v[3] = _w;
}

void mat4Set(float * _m, const float * _src)
{
	const uint32_t MAT4_FLOATS = 16;
	for (uint32_t ii = 0; ii < MAT4_FLOATS; ++ii) {
		_m[ii] = _src[ii];
	}
}

class ExampleScreenSpaceShadows : public entry::AppI
{
public:
	ExampleScreenSpaceShadows(const char* _name, const char* _description)
		: entry::AppI(_name, _description)
		, m_currFrame(UINT32_MAX)
		, m_texelHalf(0.0f)
	{
	}

	void init(int32_t _argc, const char* const* _argv, uint32_t _width, uint32_t _height) override
	{
		Args args(_argc, _argv);

		m_width = _width;
		m_height = _height;
		m_debug = BGFX_DEBUG_NONE;
		m_reset = BGFX_RESET_VSYNC;

		bgfx::Init init;
		init.type = args.m_type;

		init.vendorId = args.m_pciId;
		init.resolution.width = m_width;
		init.resolution.height = m_height;
		init.resolution.reset = m_reset;
		bgfx::init(init);

		// Enable debug text.
		bgfx::setDebug(m_debug);

		// Create uniforms
		m_uniforms.init();

		// Create texture sampler uniforms (used when we bind textures)
		s_albedo = bgfx::createUniform("s_albedo", bgfx::UniformType::Sampler); // Model's source albedo
		s_color = bgfx::createUniform("s_color", bgfx::UniformType::Sampler); // Color (albedo) gbuffer, default color input
		s_normal = bgfx::createUniform("s_normal", bgfx::UniformType::Sampler); // Normal gbuffer, Model's source normal
		s_depth = bgfx::createUniform("s_depth", bgfx::UniformType::Sampler); // Depth gbuffer
		s_shadows = bgfx::createUniform("s_shadows", bgfx::UniformType::Sampler);

		// Create program from shaders.
		m_gbufferProgram = loadProgram("vs_sss_gbuffer", "fs_sss_gbuffer"); // Fill gbuffer
		m_sphereProgram = loadProgram("vs_sss_gbuffer", "fs_sss_unlit");
		m_linearDepthProgram = loadProgram("vs_sss_screenquad", "fs_sss_linear_depth");
		m_shadowsProgram = loadProgram("vs_sss_screenquad", "fs_screen_space_shadows");
		m_combineProgram = loadProgram("vs_sss_screenquad", "fs_sss_deferred_combine"); // Compute lighting from gbuffer

		// Load some meshes
		for (uint32_t ii = 0; ii < BX_COUNTOF(s_meshPaths); ++ii)
		{
			m_meshes[ii] = meshLoad(s_meshPaths[ii]);
		}

		// sphere is first mesh
		m_lightModel.mesh = 0;

		// Randomly create some models
		bx::RngMwc mwc;
		for (uint32_t ii = 0; ii < BX_COUNTOF(m_models); ++ii)
		{
			Model& model = m_models[ii];

			model.mesh = mwc.gen() % BX_COUNTOF(s_meshPaths);
			model.position[0] = (((mwc.gen() % 256)) - 128.0f) / 20.0f;
			model.position[1] = 0;
			model.position[2] = (((mwc.gen() % 256)) - 128.0f) / 20.0f;
		}

		// Load ground, just use the cube
		m_ground = meshLoad("meshes/cube.bin");

		m_groundTexture = loadTexture("textures/fieldstone-rgba.dds");
		m_normalTexture = loadTexture("textures/fieldstone-n.dds");

		m_recreateFrameBuffers = false;
		createFramebuffers();
	
		// Vertex decl
		PosTexCoord0Vertex::init();

		// Init camera
		cameraCreate();
		cameraSetPosition({ 0.0f, 1.5f, -4.0f });
		cameraSetVerticalAngle(-0.3f);
		m_fovY = 60.0f;

		cameraGetViewMtx(m_view);
		bx::mtxProj(m_proj, m_fovY, float(m_size[0]) / float(m_size[1]), 0.01f, 100.0f,  bgfx::getCaps()->homogeneousDepth);

		// Track whether previous results are valid
		m_havePrevious = false;

		// Get renderer capabilities info.
		const bgfx::RendererType::Enum renderer = bgfx::getRendererType();
		m_texelHalf = bgfx::RendererType::Direct3D9 == renderer ? 0.5f : 0.0f;

		imguiCreate();
	}

	int32_t shutdown() override
	{
		for (uint32_t ii = 0; ii < BX_COUNTOF(s_meshPaths); ++ii)
		{
			meshUnload(m_meshes[ii]);
		}
		meshUnload(m_ground);

		bgfx::destroy(m_normalTexture);
		bgfx::destroy(m_groundTexture);

		bgfx::destroy(m_gbufferProgram);
		bgfx::destroy(m_sphereProgram);
		bgfx::destroy(m_linearDepthProgram);
		bgfx::destroy(m_shadowsProgram);
		bgfx::destroy(m_combineProgram);

		m_uniforms.destroy();

		bgfx::destroy(s_albedo);
		bgfx::destroy(s_color);
		bgfx::destroy(s_normal);
		bgfx::destroy(s_depth);
		bgfx::destroy(s_shadows);

		destroyFramebuffers();

		cameraDestroy();

		imguiDestroy();

		bgfx::shutdown();

		return 0;
	}

	bool update() override
	{
		if (!entry::processEvents(m_width, m_height, m_debug, m_reset, &m_mouseState))
		{
			// skip processing when minimized, otherwise crashing
			if (0 == m_width || 0 == m_height)
			{
				return true;
			}

			// Update frame timer
			int64_t now = bx::getHPCounter();
			static int64_t last = now;
			const int64_t frameTime = now - last;
			last = now;
			const double freq = double(bx::getHPFrequency());
			const float deltaTime = float(frameTime / freq);
			const bgfx::Caps* caps = bgfx::getCaps();

			if (m_size[0] != (int32_t)m_width
			||  m_size[1] != (int32_t)m_height
			||  m_recreateFrameBuffers)
			{
				destroyFramebuffers();
				createFramebuffers();
				m_recreateFrameBuffers = false;
			}

			// rotate light
			const float rotationSpeed = m_moveLight ? 0.75f : 0.0f;
			m_lightRotation += deltaTime * rotationSpeed;
			if (bx::kPi2 < m_lightRotation)
			{
				m_lightRotation -= bx::kPi2;
			}
			m_lightModel.position[0] = bx::cos(m_lightRotation) * 3.0f;
			m_lightModel.position[1] = 1.5f;
			m_lightModel.position[2] = bx::sin(m_lightRotation) * 3.0f;

			// Update camera
			cameraUpdate(deltaTime*0.15f, m_mouseState);

			// Set up matrices for gbuffer
			cameraGetViewMtx(m_view);

			updateUniforms();

			bx::mtxProj(m_proj, m_fovY, float(m_size[0]) / float(m_size[1]), 0.01f, 100.0f, caps->homogeneousDepth);
			bx::mtxProj(m_proj2, m_fovY, float(m_size[0]) / float(m_size[1]), 0.01f, 100.0f, false);

			bgfx::ViewId view = 0;

			// Draw everything into gbuffer
			{
				bgfx::setViewName(view, "gbuffer");
				bgfx::setViewClear(view
					, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH
					, 0
					, 1.0f
					, 0
				);

				bgfx::setViewRect(view, 0, 0, uint16_t(m_size[0]), uint16_t(m_size[1]));
				bgfx::setViewTransform(view, m_view, m_proj);
				// Make sure when we draw it goes into gbuffer and not backbuffer
				bgfx::setViewFrameBuffer(view, m_gbuffer);

				bgfx::setState(0
					| BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_WRITE_Z
					| BGFX_STATE_DEPTH_TEST_LESS
					);

				drawAllModels(view, m_gbufferProgram, m_uniforms);

				// draw sphere to visualize light
				{
					const float scale = s_meshScale[m_lightModel.mesh];
					float mtx[16];
					bx::mtxSRT(mtx
						, scale
						, scale
						, scale
						, 0.0f
						, 0.0f
						, 0.0f
						, m_lightModel.position[0]
						, m_lightModel.position[1]
						, m_lightModel.position[2]
						);

					m_uniforms.submit();
					meshSubmit(m_meshes[m_lightModel.mesh], view, m_sphereProgram, mtx);
				}


				++view;
			}

			float orthoProj[16];
			bx::mtxOrtho(orthoProj, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, caps->homogeneousDepth);
			{
				// clear out transform stack
				float identity[16];
				bx::mtxIdentity(identity);
				bgfx::setTransform(identity);
			}

			// Convert depth to linear depth for shadow depth compare
			{
				bgfx::setViewName(view, "linear depth");

				bgfx::setViewRect(view, 0, 0, uint16_t(m_width), uint16_t(m_height));
				bgfx::setViewTransform(view, NULL, orthoProj);
				bgfx::setViewFrameBuffer(view, m_linearDepth.m_buffer);
				bgfx::setState(0
					| BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_DEPTH_TEST_ALWAYS
					);
				bgfx::setTexture(0, s_depth, m_gbufferTex[GBUFFER_RT_DEPTH]);
				m_uniforms.submit();
				screenSpaceQuad(float(m_width), float(m_height), m_texelHalf, caps->originBottomLeft);
				bgfx::submit(view, m_linearDepthProgram);
				++view;
			}

			// Do screen space shadows
			{
				bgfx::setViewName(view, "screen space shadows");

				bgfx::setViewRect(view, 0, 0, uint16_t(m_width), uint16_t(m_height));
				bgfx::setViewTransform(view, NULL, orthoProj);
				bgfx::setViewFrameBuffer(view, m_shadows.m_buffer);
				bgfx::setState(0
					| BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_DEPTH_TEST_ALWAYS
					);
				bgfx::setTexture(0, s_depth, m_linearDepth.m_texture);
				m_uniforms.submit();
				screenSpaceQuad(float(m_width), float(m_height), m_texelHalf, caps->originBottomLeft);
				bgfx::submit(view, m_shadowsProgram);
				++view;
			}

			// Shade gbuffer
			{
				bgfx::setViewName(view, "combine");

				bgfx::setViewRect(view, 0, 0, uint16_t(m_width), uint16_t(m_height));
				bgfx::setViewTransform(view, NULL, orthoProj);
				bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
				bgfx::setState(0
					| BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_DEPTH_TEST_ALWAYS
					);
				bgfx::setTexture(0, s_color, m_gbufferTex[GBUFFER_RT_COLOR]);
				bgfx::setTexture(1, s_normal, m_gbufferTex[GBUFFER_RT_NORMAL]);
				bgfx::setTexture(2, s_depth, m_linearDepth.m_texture);
				bgfx::setTexture(3, s_shadows, m_shadows.m_texture);
				m_uniforms.submit();
				screenSpaceQuad(float(m_width), float(m_height), m_texelHalf, caps->originBottomLeft);
				bgfx::submit(view, m_combineProgram);
				++view;
			}

			// Draw UI
			imguiBeginFrame(m_mouseState.m_mx
				, m_mouseState.m_my
				, (m_mouseState.m_buttons[entry::MouseButton::Left] ? IMGUI_MBUT_LEFT : 0)
				| (m_mouseState.m_buttons[entry::MouseButton::Right] ? IMGUI_MBUT_RIGHT : 0)
				| (m_mouseState.m_buttons[entry::MouseButton::Middle] ? IMGUI_MBUT_MIDDLE : 0)
				, m_mouseState.m_mz
				, uint16_t(m_width)
				, uint16_t(m_height)
				);

			showExampleDialog(this);

			ImGui::SetNextWindowPos(
				ImVec2(m_width - m_width / 4.0f - 10.0f, 10.0f)
				, ImGuiCond_FirstUseEver
				);
			ImGui::SetNextWindowSize(
				ImVec2(m_width / 4.0f, m_height / 2.3f)
				, ImGuiCond_FirstUseEver
				);
			ImGui::Begin("Settings"
				, NULL
				, 0
				);

			ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.5f);

			{
				ImGui::Text("shadow controls:");
				ImGui::Checkbox("screen space radius", &m_useScreenSpaceRadius);
				if (m_useScreenSpaceRadius)
				{
					ImGui::SliderFloat("radius in pixels", &m_shadowRadiusPixels, 1.0f, 100.0f);
				}
				else
				{
					ImGui::SliderFloat("radius in world units", &m_shadowRadius, 1e-3f, 1.0f);
				}
				ImGui::SliderInt("shadow steps", &m_shadowSteps, 1, 64);

				ImGui::Combo("contact shadows mode", &m_contactShadowsMode, "hard\0soft\0very soft\0\0");

				ImGui::Checkbox("add random offset to initial position", &m_useNoiseOffset);
				ImGui::Checkbox("use different offset each frame", &m_dynamicNoise);
				ImGui::Separator();

				ImGui::Text("scene controls:");
				ImGui::Checkbox("display shadows only", &m_displayShadows);
				ImGui::Checkbox("move light", &m_moveLight);
			}

			ImGui::End();

			imguiEndFrame();

			// Advance to next frame. Rendering thread will be kicked to
			// process submitted rendering primitives.
			m_currFrame = bgfx::frame();

			return true;
		}

		return false;
	}

	void drawAllModels(bgfx::ViewId _pass, bgfx::ProgramHandle _program, const Uniforms & _uniforms)
	{
		for (uint32_t ii = 0; ii < BX_COUNTOF(m_models); ++ii)
		{
			const Model& model = m_models[ii];

			// Set up transform matrix for each model
			const float scale = s_meshScale[model.mesh];
			float mtx[16];
			bx::mtxSRT(mtx
				, scale
				, scale
				, scale
				, 0.0f
				, 0.0f
				, 0.0f
				, model.position[0]
				, model.position[1]
				, model.position[2]
				);

			// Submit mesh to gbuffer
			bgfx::setTexture(0, s_albedo, m_groundTexture);
			bgfx::setTexture(1, s_normal, m_normalTexture);
			_uniforms.submit();

			meshSubmit(m_meshes[model.mesh], _pass, _program, mtx);
		}

		// Draw ground
		float mtxScale[16];
		const float scale = 10.0f;
		bx::mtxScale(mtxScale, scale, scale, scale);

		float mtxTranslate[16];
		bx::mtxTranslate(mtxTranslate
			, 0.0f
			, -10.0f
			, 0.0f
			);

		float mtx[16];
		bx::mtxMul(mtx, mtxScale, mtxTranslate);
		bgfx::setTexture(0, s_albedo, m_groundTexture);
		bgfx::setTexture(1, s_normal, m_normalTexture);
		_uniforms.submit();

		meshSubmit(m_ground, _pass, _program, mtx);
	}

	void createFramebuffers()
	{
		m_size[0] = m_width;
		m_size[1] = m_height;

		const uint64_t pointSampleFlags = 0
			| BGFX_TEXTURE_RT
			| BGFX_SAMPLER_U_CLAMP
			| BGFX_SAMPLER_V_CLAMP
			| BGFX_SAMPLER_MIN_POINT
			| BGFX_SAMPLER_MAG_POINT
			| BGFX_SAMPLER_MIP_POINT
			;

		m_gbufferTex[GBUFFER_RT_COLOR]    = bgfx::createTexture2D(uint16_t(m_size[0]), uint16_t(m_size[1]), false, 1, bgfx::TextureFormat::BGRA8, pointSampleFlags);
		m_gbufferTex[GBUFFER_RT_NORMAL]   = bgfx::createTexture2D(uint16_t(m_size[0]), uint16_t(m_size[1]), false, 1, bgfx::TextureFormat::BGRA8, pointSampleFlags);
		m_gbufferTex[GBUFFER_RT_DEPTH]    = bgfx::createTexture2D(uint16_t(m_size[0]), uint16_t(m_size[1]), false, 1, bgfx::TextureFormat::D24, pointSampleFlags);
		m_gbuffer = bgfx::createFrameBuffer(BX_COUNTOF(m_gbufferTex), m_gbufferTex, true);

		m_linearDepth.init(m_size[0], m_size[1], bgfx::TextureFormat::R16F, pointSampleFlags);
		m_shadows.init(m_size[0], m_size[1], bgfx::TextureFormat::R16F, pointSampleFlags);
	}

	// all buffers set to destroy their textures
	void destroyFramebuffers()
	{
		bgfx::destroy(m_gbuffer);

		m_linearDepth.destroy();
		m_shadows.destroy();
	}

	void updateUniforms()
	{
		m_uniforms.m_displayShadows = m_displayShadows ? 1.0f : 0.0f;
		m_uniforms.m_frameIdx = m_dynamicNoise
			? float(m_currFrame % 8)
			: 0.0f;
		m_uniforms.m_shadowRadius = m_useScreenSpaceRadius ? m_shadowRadiusPixels : m_shadowRadius;
		m_uniforms.m_shadowSteps = float(m_shadowSteps);
		m_uniforms.m_useNoiseOffset = m_useNoiseOffset ? 1.0f : 0.0f;
		m_uniforms.m_contactShadowsMode = float(m_contactShadowsMode);
		m_uniforms.m_useScreenSpaceRadius = m_useScreenSpaceRadius ? 1.0f : 0.0f;

		mat4Set(m_uniforms.m_worldToView, m_view);
		mat4Set(m_uniforms.m_viewToProj, m_proj);

		// from assao sample, cs_assao_prepare_depths.sc
		{
			// float depthLinearizeMul = ( clipFar * clipNear ) / ( clipFar - clipNear );
			// float depthLinearizeAdd = clipFar / ( clipFar - clipNear );
			// correct the handedness issue. need to make sure this below is correct, but I think it is.

			float depthLinearizeMul = -m_proj2[3*4+2];
			float depthLinearizeAdd =  m_proj2[2*4+2];

			if (depthLinearizeMul * depthLinearizeAdd < 0)
			{
				depthLinearizeAdd = -depthLinearizeAdd;
			}

			vec2Set(m_uniforms.m_depthUnpackConsts, depthLinearizeMul, depthLinearizeAdd);

			float tanHalfFOVY = 1.0f / m_proj2[1*4+1];	// = tanf( drawContext.Camera.GetYFOV( ) * 0.5f );
			float tanHalfFOVX = 1.0F / m_proj2[0];		// = tanHalfFOVY * drawContext.Camera.GetAspect( );

			if (bgfx::getRendererType() == bgfx::RendererType::OpenGL)
			{
				vec2Set(m_uniforms.m_ndcToViewMul, tanHalfFOVX * 2.0f, tanHalfFOVY * 2.0f);
				vec2Set(m_uniforms.m_ndcToViewAdd, tanHalfFOVX * -1.0f, tanHalfFOVY * -1.0f);
			}
			else
			{
				vec2Set(m_uniforms.m_ndcToViewMul, tanHalfFOVX * 2.0f, tanHalfFOVY * -2.0f);
				vec2Set(m_uniforms.m_ndcToViewAdd, tanHalfFOVX * -1.0f, tanHalfFOVY * 1.0f);
			}
		}

		{
			float lightPosition[4];
			bx::memCopy(lightPosition, m_lightModel.position, 3*sizeof(float));
			lightPosition[3] = 1.0f;
			float viewSpaceLightPosition[4];
			bx::vec4MulMtx(viewSpaceLightPosition, lightPosition, m_view);
			bx::memCopy(m_uniforms.m_lightPosition, viewSpaceLightPosition, 3*sizeof(float));
		}
	}


	uint32_t m_width;
	uint32_t m_height;
	uint32_t m_debug;
	uint32_t m_reset;

	entry::MouseState m_mouseState;

	// Resource handles
	bgfx::ProgramHandle m_gbufferProgram;
	bgfx::ProgramHandle m_sphereProgram;
	bgfx::ProgramHandle m_linearDepthProgram;
	bgfx::ProgramHandle m_shadowsProgram;
	bgfx::ProgramHandle m_combineProgram;

	// Shader uniforms
	Uniforms m_uniforms;

	// Uniforms to indentify texture samplers
	bgfx::UniformHandle s_albedo;
	bgfx::UniformHandle s_color;
	bgfx::UniformHandle s_normal;
	bgfx::UniformHandle s_depth;
	bgfx::UniformHandle s_shadows;

	bgfx::FrameBufferHandle m_gbuffer;
	bgfx::TextureHandle m_gbufferTex[GBUFFER_RENDER_TARGETS];

	RenderTarget m_linearDepth;
	RenderTarget m_shadows;

	struct Model
	{
		uint32_t mesh; // Index of mesh in m_meshes
		float position[3];
	};

	Model m_lightModel;
	Model m_models[MODEL_COUNT];
	Mesh* m_meshes[BX_COUNTOF(s_meshPaths)];
	Mesh* m_ground;
	bgfx::TextureHandle m_groundTexture;
	bgfx::TextureHandle m_normalTexture;

	uint32_t m_currFrame;
	float m_lightRotation = 0.0f;
	float m_texelHalf = 0.0f;
	float m_fovY = 60.0f;
	bool m_recreateFrameBuffers = false;
	bool m_havePrevious = false;

	float m_view[16];
	float m_proj[16];
	float m_proj2[16];
	int32_t m_size[2];

	// UI parameters
	bool m_displayShadows = false;
	bool m_useNoiseOffset = true;
	bool m_dynamicNoise = true;
	float m_shadowRadius = 0.25f;
	float m_shadowRadiusPixels = 25.0f;
	int32_t m_shadowSteps = 8;
	bool m_moveLight = true;
	int32_t m_contactShadowsMode = 0;
	bool m_useScreenSpaceRadius = false;
};

} // namespace

ENTRY_IMPLEMENT_MAIN(ExampleScreenSpaceShadows, "xx-sss", "Screen Space Shadows.");
