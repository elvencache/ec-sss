/*
* Copyright 2021 elven cache. All rights reserved.
* License: https://github.com/bkaradzic/bgfx#license-bsd-2-clause
*/

/*
* Implement screen space shadows denoising as bgfx example. Goal is to explore various
* options and parameters.
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
#define GBUFFER_RT_VELOCITY		2
#define GBUFFER_RT_DEPTH		3
#define GBUFFER_RENDER_TARGETS	4

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
	enum { NumVec4 = 18 };

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
			/*  0    */ struct { float m_cameraJitterCurr[2]; float m_cameraJitterPrev[2]; };
			/*  1    */ struct { float m_feedbackMin; float m_feedbackMax; float m_unused1[2]; };
			/*  2    */ struct { float m_unused2; float m_applyMitchellFilter; float m_options[2]; };
			/*  3-6  */ struct { float m_worldToViewPrev[16]; };
			/*  7-10 */ struct { float m_viewToProjPrev[16]; };
			/* 11    */ struct { float m_depthUnpackConsts[2]; float m_unused11[2]; };
			/* 12    */ struct { float m_ndcToViewMul[2]; float m_ndcToViewAdd[2]; };
			/* 13    */ struct { float m_lightPosition[3]; float m_unused13; };
			/* 14-17 */ struct { float m_worldToView[16]; }; // built-in u_view will be transform for quad during screen passes
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
		s_velocity = bgfx::createUniform("s_velocity", bgfx::UniformType::Sampler); // Velocity gbuffer
		s_depth = bgfx::createUniform("s_depth", bgfx::UniformType::Sampler); // Depth gbuffer
		s_previousColor = bgfx::createUniform("s_previousColor", bgfx::UniformType::Sampler); // Previous frame's result
		s_shadows = bgfx::createUniform("s_shadows", bgfx::UniformType::Sampler);

		// Create program from shaders.
		m_gbufferProgram = loadProgram("vs_sss_gbuffer", "fs_sss_gbuffer"); // Fill gbuffer
		m_linearDepthProgram = loadProgram("vs_sss_screenquad", "fs_sss_linear_depth");
		m_shadowsProgram = loadProgram("vs_sss_screenquad", "fs_screen_space_shadows");
		m_combineProgram = loadProgram("vs_sss_screenquad", "fs_sss_deferred_combine"); // Compute lighting from gbuffer
		m_copyProgram = loadProgram("vs_sss_screenquad", "fs_sss_copy");
		m_txaaProgram = loadProgram("vs_sss_screenquad", "fs_sss_txaa");

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
		cameraSetPosition({ 0.0f, 1.5f, 0.0f });
		cameraSetVerticalAngle(-0.3f);
		m_fovY = 60.0f;

		// Init "prev" matrices, will be same for first frame
		cameraGetViewMtx(m_view);
		bx::mtxProj(m_proj, m_fovY, float(m_size[0]) / float(m_size[1]), 0.01f, 100.0f,  bgfx::getCaps()->homogeneousDepth);
		mat4Set(m_worldToViewPrev, m_view);
		mat4Set(m_viewToProjPrev, m_proj);

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
		bgfx::destroy(m_linearDepthProgram);
		bgfx::destroy(m_shadowsProgram);
		bgfx::destroy(m_combineProgram);
		bgfx::destroy(m_copyProgram);
		bgfx::destroy(m_txaaProgram);

		m_uniforms.destroy();

		bgfx::destroy(s_albedo);
		bgfx::destroy(s_color);
		bgfx::destroy(s_normal);
		bgfx::destroy(s_velocity);
		bgfx::destroy(s_depth);
		bgfx::destroy(s_previousColor);
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
			const float rotationSpeed = 1.0f;
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

			if (m_enableTxaa)
			{
				m_proj[2*4+0] += m_jitter[0] * (2.0f / m_size[0]);
				m_proj[2*4+1] -= m_jitter[1] * (2.0f / m_size[1]);
			}

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
				//bgfx::setTexture(1, s_normal, m_gbufferTex[GBUFFER_RT_NORMAL]);
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
				bgfx::setViewFrameBuffer(view, m_currentColor.m_buffer);
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

			// update last texture written, to chain passes together
			bgfx::TextureHandle lastTex = m_currentColor.m_texture;

			if (m_enableTxaa)
			{
				// Draw txaa to txaa buffer
				{
					bgfx::setViewName(view, "temporal aa");

					bgfx::setViewRect(view, 0, 0, uint16_t(m_width), uint16_t(m_height));
					bgfx::setViewTransform(view, NULL, orthoProj);
					bgfx::setViewFrameBuffer(view, m_txaaColor.m_buffer);
					bgfx::setState(0
						| BGFX_STATE_WRITE_RGB
						| BGFX_STATE_WRITE_A
						| BGFX_STATE_DEPTH_TEST_ALWAYS
						);
					bgfx::setTexture(0, s_color, lastTex);
					bgfx::setTexture(1, s_previousColor, m_previousColor.m_texture);
					bgfx::setTexture(2, s_velocity, m_gbufferTex[GBUFFER_RT_VELOCITY]);
					bgfx::setTexture(3, s_depth, m_gbufferTex[GBUFFER_RT_DEPTH]);
					m_uniforms.submit();
					screenSpaceQuad(float(m_width), float(m_height), m_texelHalf, caps->originBottomLeft);
					bgfx::submit(view, m_txaaProgram);
					++view;
				}
			
				// Copy txaa result to previous
				{
					bgfx::setViewName(view, "copy2previous");

					bgfx::setViewRect(view, 0, 0, uint16_t(m_width), uint16_t(m_height));
					bgfx::setViewTransform(view, NULL, orthoProj);
					bgfx::setViewFrameBuffer(view, m_previousColor.m_buffer);
					bgfx::setState(0
						| BGFX_STATE_WRITE_RGB
						| BGFX_STATE_WRITE_A
						| BGFX_STATE_DEPTH_TEST_ALWAYS
						);
					bgfx::setTexture(0, s_color, m_txaaColor.m_texture);
					screenSpaceQuad(float(m_width), float(m_height), m_texelHalf, caps->originBottomLeft);
					bgfx::submit(view, m_copyProgram);
					++view;
				}

				// Copy txaa result to swap chain
				{
					bgfx::setViewName(view, "display");

					bgfx::setViewRect(view, 0, 0, uint16_t(m_width), uint16_t(m_height));
					bgfx::setViewTransform(view, NULL, orthoProj);
					bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
					bgfx::setState(0
						| BGFX_STATE_WRITE_RGB
						| BGFX_STATE_WRITE_A
						| BGFX_STATE_DEPTH_TEST_ALWAYS
						);
					bgfx::setTexture(0, s_color, m_txaaColor.m_texture);
					screenSpaceQuad(float(m_width), float(m_height), m_texelHalf, caps->originBottomLeft);
					bgfx::submit(view, m_copyProgram);
					++view;
				}
			}
			else
			{
				// Copy color result to swap chain
				{
					bgfx::setViewName(view, "display");
					bgfx::setViewClear(view
						, BGFX_CLEAR_NONE
						, 0
						, 1.0f
						, 0
					);

					bgfx::setViewRect(view, 0, 0, uint16_t(m_width), uint16_t(m_height));
					bgfx::setViewTransform(view, NULL, orthoProj);
					bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
					bgfx::setState(0
						| BGFX_STATE_WRITE_RGB
						| BGFX_STATE_WRITE_A
						);
					bgfx::setTexture(0, s_color, m_shadows.m_texture);//lastTex);
					screenSpaceQuad(float(m_width), float(m_height), m_texelHalf, caps->originBottomLeft);
					bgfx::submit(view, m_copyProgram);
					++view;
				}
			}

			// Copy matrices for next time
			mat4Set(m_worldToViewPrev, m_view);
			mat4Set(m_viewToProjPrev, m_proj);

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
				ImVec2(m_width / 4.0f, m_height / 1.24f)
				, ImGuiCond_FirstUseEver
				);
			ImGui::Begin("Settings"
				, NULL
				, 0
				);

			ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.5f);

			{
				ImGui::TextWrapped("screen space shadows");
				ImGui::Separator();
			}

			if (ImGui::CollapsingHeader("TXAA options"))
			{
				ImGui::Checkbox("use TXAA", &m_enableTxaa);
				ImGui::Checkbox("apply extra blur to current color", &m_applyMitchellFilter);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("reduces flicker/crawl on thin features, maybe too much!");

				ImGui::SliderFloat("feedback min", &m_feedbackMin, 0.0f, 1.0f);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("minimum amount of previous frame to blend in");

				ImGui::SliderFloat("feedback max", &m_feedbackMax, 0.0f, 1.0f);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("maximum amount of previous frame to blend in");

				ImGui::Checkbox("debug TXAA with slow frame rate", &m_useTxaaSlow);
				if (ImGui::IsItemHovered())
				{
					ImGui::BeginTooltip();
					ImGui::Text("sleep 100ms per frame to highlight temporal artifacts");
					ImGui::Text("high framerate compensates for flickering, masking issues");
					ImGui::EndTooltip();
				}
				ImGui::Separator();
			}

			ImGui::End();

			imguiEndFrame();

			// Advance to next frame. Rendering thread will be kicked to
			// process submitted rendering primitives.
			m_currFrame = bgfx::frame();

			// add artificial wait to emphasize txaa behavior
			if (m_useTxaaSlow)
			{
				bx::sleep(100);
			}

			return true;
		}

		return false;
	}

	void drawAllModels(bgfx::ViewId _pass, bgfx::ProgramHandle _program, const Uniforms & _uniforms)
	{
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

			// Submit mesh to gbuffer
			bgfx::setTexture(0, s_albedo, m_groundTexture);
			bgfx::setTexture(1, s_normal, m_normalTexture);
			_uniforms.submit();

			meshSubmit(m_meshes[m_lightModel.mesh], _pass, _program, mtx);
		}

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

		const uint64_t bilinearFlags = 0
			| BGFX_TEXTURE_RT
			| BGFX_SAMPLER_U_CLAMP
			| BGFX_SAMPLER_V_CLAMP
			;

		const uint64_t pointSampleFlags = bilinearFlags
			| BGFX_SAMPLER_MIN_POINT
			| BGFX_SAMPLER_MAG_POINT
			| BGFX_SAMPLER_MIP_POINT
			;

		m_gbufferTex[GBUFFER_RT_COLOR]    = bgfx::createTexture2D(uint16_t(m_size[0]), uint16_t(m_size[1]), false, 1, bgfx::TextureFormat::BGRA8, pointSampleFlags);
		m_gbufferTex[GBUFFER_RT_NORMAL]   = bgfx::createTexture2D(uint16_t(m_size[0]), uint16_t(m_size[1]), false, 1, bgfx::TextureFormat::BGRA8, pointSampleFlags);
		m_gbufferTex[GBUFFER_RT_VELOCITY] = bgfx::createTexture2D(uint16_t(m_size[0]), uint16_t(m_size[1]), false, 1, bgfx::TextureFormat::RG16F, pointSampleFlags);
		m_gbufferTex[GBUFFER_RT_DEPTH]    = bgfx::createTexture2D(uint16_t(m_size[0]), uint16_t(m_size[1]), false, 1, bgfx::TextureFormat::D24, pointSampleFlags);
		m_gbuffer = bgfx::createFrameBuffer(BX_COUNTOF(m_gbufferTex), m_gbufferTex, true);

		m_currentColor.init(m_size[0], m_size[1], bgfx::TextureFormat::RG11B10F, bilinearFlags);
		m_previousColor.init(m_size[0], m_size[1], bgfx::TextureFormat::RG11B10F, bilinearFlags);
		m_linearDepth.init(m_size[0], m_size[1], bgfx::TextureFormat::R16F, bilinearFlags);
		m_shadows.init(m_size[0], m_size[1], bgfx::TextureFormat::RG11B10F /*R16F*/, bilinearFlags);
		m_txaaColor.init(m_size[0], m_size[1], bgfx::TextureFormat::RG11B10F, bilinearFlags);
	}

	// all buffers set to destroy their textures
	void destroyFramebuffers()
	{
		bgfx::destroy(m_gbuffer);

		m_currentColor.destroy();
		m_previousColor.destroy();
		m_linearDepth.destroy();
		m_shadows.destroy();
		m_txaaColor.destroy();
	}

	void updateUniforms()
	{
		{
			uint32_t idx = m_currFrame % 8;
			const float offsets[] = {
				(1.0f/2.0f),  (1.0f/3.0f),
				(1.0f/4.0f),  (2.0f/3.0f),
				(3.0f/4.0f),  (1.0f/9.0f),
				(1.0f/8.0f),  (4.0f/9.0f),
				(5.0f/8.0f),  (7.0f/9.0f),
				(3.0f/8.0f),  (2.0f/9.0f),
				(7.0f/8.0f),  (5.0f/9.0f),
				(1.0f/16.0f), (8.0f/9.0f)
			};

			// Strange constant for jitterX is because 8 values from halton2
			// sequence above do not average out to 0.5, 1/16 skews it to the
			// left. Subtracting a smaller value to center the range of jitter
			// around 0. Not necessary for jitterY. Not confident this makes sense...
			const float jitterX = 1.0f * (offsets[2*idx]   - (7.125f/16.0f));
			const float jitterY = 1.0f * (offsets[2*idx+1] - 0.5f);

			vec2Set(m_uniforms.m_cameraJitterCurr, jitterX, jitterY);
			vec2Set(m_uniforms.m_cameraJitterPrev, m_jitter[0], m_jitter[1]);

			m_jitter[0] = jitterX;
			m_jitter[1] = jitterY;
		}

		m_uniforms.m_feedbackMin = m_feedbackMin;
		m_uniforms.m_feedbackMax = m_feedbackMax;
		m_uniforms.m_applyMitchellFilter = m_applyMitchellFilter ? 1.0f : 0.0f;

		mat4Set(m_uniforms.m_worldToViewPrev, m_worldToViewPrev);
		mat4Set(m_uniforms.m_viewToProjPrev, m_viewToProjPrev);
		mat4Set(m_uniforms.m_worldToView, m_view);

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

			float tanHalfFOVY = 1.0f / m_proj2[1*4+1];    // = tanf( drawContext.Camera.GetYFOV( ) * 0.5f );
			float tanHalfFOVX = 1.0F / m_proj2[0];    // = tanHalfFOVY * drawContext.Camera.GetAspect( );

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
	bgfx::ProgramHandle m_linearDepthProgram;
	bgfx::ProgramHandle m_shadowsProgram;
	bgfx::ProgramHandle m_combineProgram;
	bgfx::ProgramHandle m_copyProgram;
	bgfx::ProgramHandle m_txaaProgram;

	// Shader uniforms
	Uniforms m_uniforms;

	// Uniforms to indentify texture samplers
	bgfx::UniformHandle s_albedo;
	bgfx::UniformHandle s_color;
	bgfx::UniformHandle s_normal;
	bgfx::UniformHandle s_velocity;
	bgfx::UniformHandle s_depth;
	bgfx::UniformHandle s_previousColor;
	bgfx::UniformHandle s_shadows;

	bgfx::FrameBufferHandle m_gbuffer;
	bgfx::TextureHandle m_gbufferTex[GBUFFER_RENDER_TARGETS];

	RenderTarget m_currentColor;
	RenderTarget m_previousColor;
	RenderTarget m_linearDepth;
	RenderTarget m_shadows;
	RenderTarget m_txaaColor;

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
	float m_viewToProjPrev[16];
	float m_worldToViewPrev[16];
	float m_jitter[2];
	int32_t m_size[2];

	// UI parameters
	bool m_enableTxaa = false;
	float m_feedbackMin = 0.8f;
	float m_feedbackMax = 0.95f;
	bool m_applyMitchellFilter = true;
	bool m_useTxaaSlow = false;
};

} // namespace

ENTRY_IMPLEMENT_MAIN(ExampleScreenSpaceShadows, "xx-sss", "Screen Space Shadows.");
