#include "HiZCull.h"

#include "Globals.h"
#include "HiZReadback.h"

#include <RE/B/BSMultiBound.h>
#include <RE/B/BSMultiBoundNode.h>
#include <RE/B/BSMultiStreamInstanceTriShape.h>
#include <RE/N/NiAVObject.h>
#include <RE/N/NiCamera.h>
#include <RE/S/ShadowSceneNode.h>

#include <atomic>
#include <cfloat>
#include <cmath>

namespace HiZCull
{
	bool  EnableOcclusionTesting = false;
	float HiZOcclusionBias = 0.0005f;
	float HiZMaxCameraMotion = 1024.0f;
	float ObjectTestMinRadius = 0.0f;

	bool  CullSmallObjects = false;
	float ObjectCullNearRadius = 8.0f;
	float ObjectCullDistSlope = 0.004f;

	bool  CullSmallShadows = true;
	float ShadowCullNearRadius = 32.0f;
	float ShadowCullDistSlope = 0.012f;

	bool          CullTreeLODGroups = false;
	std::uint32_t HideFrames = 2;

	namespace
	{
		using namespace DirectX;

		std::atomic<std::uint32_t> g_frameIndex{ 0 };

		// World origin the current cull pass is relative to. Written by BeginCull on the cull
		// thread, read by the tests; a torn read would at worst mis-scale one object's motion
		// allowance for one frame, so it does not need a lock.
		RE::NiPoint3 g_posAdjust{ 0.0f, 0.0f, 0.0f };

		std::atomic<const RE::NiCamera*> g_sunGatherCam{ nullptr };

		// Per-object state for the anti-flicker hysteresis (see Stabilize). Open addressed, never
		// resized, and deliberately racy: concurrent cull threads and hash collisions both reset
		// a streak, which keeps the object drawn.
		constexpr std::uint32_t kStateBits = 14;
		constexpr std::uint32_t kStateSize = 1u << kStateBits;
		constexpr std::uint32_t kStateMask = kStateSize - 1u;

		struct ObjectState
		{
			std::atomic<std::uintptr_t> key{ 0 };
			std::atomic<std::uint32_t>  frame{ 0 };
			std::atomic<std::uint32_t>  hiddenStreak{ 0 };
			std::atomic<std::uint32_t>  lastHidden{ 0 };
		};
		ObjectState g_state[kStateSize];

		inline std::uint32_t StateSlot(const void* a_object)
		{
			const auto h = reinterpret_cast<std::uintptr_t>(a_object);
			return static_cast<std::uint32_t>(((h >> 4) * 2654435761u) >> (32 - kStateBits)) & kStateMask;
		}

		std::atomic<std::uint32_t> g_tested{ 0 };
		std::atomic<std::uint32_t> g_culled{ 0 };
		std::atomic<std::uint32_t> g_flips{ 0 };    ///< verdict changed from the previous frame
		std::atomic<std::uint32_t> g_deferred{ 0 };  ///< hidden, but not yet for long enough to cull
		std::atomic<std::uint32_t> g_culledSphere{ 0 };
		std::atomic<std::uint32_t> g_culledAABB{ 0 };
		std::atomic<std::uint32_t> g_smallObjects{ 0 };
		std::atomic<std::uint32_t> g_smallShadows{ 0 };

		RE::NiCamera* GetMainCamera()
		{
			// The world root node is a BSSceneGraph whose runtime camera is the exact pointer
			// DrawWorld_PreRender loads for CacheCameraData / SetCameraData and for every
			// main-scene cull. NOT a scene-graph child walk, which can find a different (stale)
			// camera under CameraRoot.
			RE::NiNode* root = RE::Main::WorldRootNode();
			if (!root)
				return nullptr;
			return static_cast<RE::BSSceneGraph*>(root)->GetRuntimeData().camera.get();
		}

		/**
		 * @brief Faithful port of NiCamera::CalculateViewProjection.
		 *
		 * Camera-relative, row-vector layout. The odd-looking near/far handling is the engine's;
		 * it is reproduced rather than tidied because every consumer of this matrix has to agree
		 * with the depth buffer the GPU actually produced.
		 */
		void CalculateViewProjection(RE::NiCamera* a_camera, XMMATRIX& a_view, XMMATRIX& a_proj, XMMATRIX& a_viewProj)
		{
			const RE::NiMatrix3& R = a_camera->world.rotate;
			const RE::NiPoint3   dir{ R.entry[0][0], R.entry[1][0], R.entry[2][0] };    // col 0
			const RE::NiPoint3   up{ R.entry[0][1], R.entry[1][1], R.entry[2][1] };     // col 1
			const RE::NiPoint3   right{ R.entry[0][2], R.entry[1][2], R.entry[2][2] };  // col 2

			a_view.r[0] = XMVectorSet(right.x, up.x, dir.x, 0.0f);
			a_view.r[1] = XMVectorSet(right.y, up.y, dir.y, 0.0f);
			a_view.r[2] = XMVectorSet(right.z, up.z, dir.z, 0.0f);
			a_view.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);

			const RE::NiFrustum& fr = a_camera->GetRuntimeData2().viewFrustum;
			const float          rightLeftDiff = fr.fRight - fr.fLeft;
			const float          rightLeftRatio = -((1.0f / rightLeftDiff) * (fr.fRight + fr.fLeft));
			const float          topBottomDiff = fr.fTop - fr.fBottom;
			const float          topBottomRatio = -((1.0f / topBottomDiff) * (fr.fTop + fr.fBottom));
			const float          invNearFarDiff = 1.0f / (fr.fFar - fr.fNear);

			a_proj.r[0] = _mm_setzero_ps();
			a_proj.r[1] = _mm_setzero_ps();
			a_proj.r[2] = _mm_setzero_ps();
			a_proj.r[3] = _mm_setzero_ps();
			a_proj.r[0].m128_f32[0] = (1.0f / rightLeftDiff) * 2.0f;
			a_proj.r[1].m128_f32[1] = (1.0f / topBottomDiff) * 2.0f;
			if (!fr.bOrtho) {
				a_proj.r[2].m128_f32[0] = rightLeftRatio;
				a_proj.r[2].m128_f32[1] = topBottomRatio;
				a_proj.r[2].m128_f32[2] = invNearFarDiff * fr.fFar;
				a_proj.r[2].m128_f32[3] = 1.0f;
				a_proj.r[3].m128_f32[2] = -((fr.fNear * fr.fFar) * invNearFarDiff);
			} else {
				a_proj.r[2].m128_f32[2] = invNearFarDiff;
				a_proj.r[3].m128_f32[0] = rightLeftRatio;
				a_proj.r[3].m128_f32[1] = topBottomRatio;
				a_proj.r[3].m128_f32[2] = -(invNearFarDiff * fr.fNear);
				a_proj.r[3].m128_f32[3] = 1.0f;
			}

			a_viewProj = XMMatrixMultiply(a_view, a_proj);
		}

		/**
		 * @brief Sphere occlusion test. Mirrors the grass feature's GrassCullingCS.
		 *
		 * The pyramid holds the farthest depth beneath each texel, so a bound whose nearest point
		 * is behind that across its whole footprint is hidden. Sky reads as the far plane, which
		 * nothing can be behind.
		 *
		 * The snapshot is a few frames old, so: bounds are projected with the camera that
		 * captured it, a footprint not wholly inside the captured image is kept, and bounds are
		 * inflated by the camera's travel since. Only translation can reveal occluded geometry,
		 * so rotation needs no allowance.
		 */
		bool TestSphereHiZ(const RE::NiPoint3& a_center, float a_radius, const HiZReadback::Snapshot& a_snap)
		{
			const float mdx = g_posAdjust.x - a_snap.posAdjust[0];
			const float mdy = g_posAdjust.y - a_snap.posAdjust[1];
			const float mdz = g_posAdjust.z - a_snap.posAdjust[2];
			const float motion = std::sqrt(mdx * mdx + mdy * mdy + mdz * mdz);
			if (motion > HiZMaxCameraMotion)
				return true;  // travelled too far since the capture to trust it about anything

			const float radius = a_radius + motion;

			XMVECTOR rel = _mm_setr_ps(
				a_center.x - a_snap.posAdjust[0],
				a_center.y - a_snap.posAdjust[1],
				a_center.z - a_snap.posAdjust[2], 1.0f);

			const float dist = XMVector3Length(rel).m128_f32[0];
			if (dist <= radius)
				return true;  // captured camera inside the bound

			const XMVECTOR clip = XMVector4Transform(rel, a_snap.viewProj);
			const float    w = clip.m128_f32[3];
			if (w <= 1e-4f)
				return true;  // behind the captured camera; the engine's frustum cull owns this

			const float invW = 1.0f / w;
			const float tcx = (clip.m128_f32[0] * invW * 0.5f + 0.5f) * a_snap.validWidth;
			const float tcy = (clip.m128_f32[1] * invW * -0.5f + 0.5f) * a_snap.validHeight;

			// Level where the bound spans about two texels, so the 3x3 block below covers it. A
			// finer level would take the max over only part of the footprint and cull something
			// visible.
			const float projPx = (radius / dist) * a_snap.projScale;
			const float rT = projPx / a_snap.texelPixels;
			const float wantLevel = std::ceil(std::log2(std::max(2.0f * rT, 1.0f)));
			if (!(wantLevel >= 0.0f) || wantLevel > float(a_snap.mipCount) - 1.0f)
				return true;  // too big for the chain to cover (or a NaN bound)

			const int   level = int(wantLevel);
			const float scale = std::exp2(float(level));
			const float rTL = rT / scale;
			const int   x0 = int(std::floor(tcx / scale - rTL));
			const int   y0 = int(std::floor(tcy / scale - rTL));
			const int   validW = std::max(1, int(std::ceil(a_snap.validWidth / scale)));
			const int   validH = std::max(1, int(std::ceil(a_snap.validHeight / scale)));

			// Wholly inside, 3x3 included; clamping would test an off-screen footprint against
			// edge texels.
			if (x0 < 0 || y0 < 0 || x0 + 2 > validW - 1 || y0 + 2 > validH - 1)
				return true;

			const XMVECTOR nearPoint = XMVectorSetW(XMVectorScale(rel, (dist - radius) / dist), 1.0f);
			const XMVECTOR clipN = XMVector4Transform(nearPoint, a_snap.viewProj);
			const float    nearZ = clipN.m128_f32[2] / std::max(clipN.m128_f32[3], 1e-4f);

			const float tileMax = a_snap.TileMax(level, x0, y0, validW, validH);
			return !(nearZ > tileMax + HiZOcclusionBias);
		}

		/**
		 * @brief Turns a per-frame hidden/visible reading into a stable decision.
		 * @return true if the object should be culled this frame.
		 *
		 * A single Hi-Z reading is not reliable: the snapshot's age varies, which moves the motion
		 * allowance, which can move the chosen mip level across a ceil() step and flip the answer.
		 * Distant LOD sits nearest those boundaries. Requiring HideFrames consecutive hidden
		 * readings costs an extra frame of drawing and removes the flicker.
		 *
		 * Also counts disagreements with the previous frame, which is the only direct measure of
		 * flicker: an object drawn every other frame looks identical in any single screenshot.
		 */
		bool Stabilize(const void* a_object, bool a_hidden)
		{
			const std::uint32_t frame = g_frameIndex.load(std::memory_order_relaxed);
			auto&               slot = g_state[StateSlot(a_object)];
			const auto          key = reinterpret_cast<std::uintptr_t>(a_object);

			if (slot.key.load(std::memory_order_relaxed) != key) {
				// First sighting, or a collision evicting someone else.
				slot.key.store(key, std::memory_order_relaxed);
				slot.frame.store(frame, std::memory_order_relaxed);
				slot.hiddenStreak.store(a_hidden ? 1u : 0u, std::memory_order_relaxed);
				slot.lastHidden.store(a_hidden ? 1u : 0u, std::memory_order_relaxed);
				return a_hidden && HideFrames <= 1;
			}

			const std::uint32_t prevFrame = slot.frame.load(std::memory_order_relaxed);
			const bool          prevHidden = slot.lastHidden.load(std::memory_order_relaxed) != 0u;

			// Only consecutive frames say anything about stability; a gap means it was not walked.
			if (prevFrame + 1u == frame && prevHidden != a_hidden)
				g_flips.fetch_add(1, std::memory_order_relaxed);

			std::uint32_t streak = 0u;
			if (a_hidden)
				streak = (prevFrame + 1u == frame) ? slot.hiddenStreak.load(std::memory_order_relaxed) + 1u : 1u;

			slot.frame.store(frame, std::memory_order_relaxed);
			slot.hiddenStreak.store(streak, std::memory_order_relaxed);
			slot.lastHidden.store(a_hidden ? 1u : 0u, std::memory_order_relaxed);

			if (!a_hidden)
				return false;
			if (streak < HideFrames) {
				g_deferred.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			return true;
		}

		/** @brief Distance-scaled size gate shared by the two size culls. Returns true to keep. */
		bool KeepBySize(const RE::NiPoint3& a_center, float a_radius, float a_near, float a_slope)
		{
			const float dx = a_center.x - g_posAdjust.x;
			const float dy = a_center.y - g_posAdjust.y;
			const float dz = a_center.z - g_posAdjust.z;
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			return a_radius >= a_near + a_slope * dist;
		}

		/**
		 * @brief Box occlusion test, projecting the eight corners for a tight screen rect.
		 *
		 * Not reduced to the box's bounding sphere: a sphere around a long flat room is far
		 * larger than the room, covers much more of the screen, and so almost never tests as
		 * hidden. These are rooms and building shells, where one cull prunes all the contents.
		 */
		bool TestAABBHiZ(const RE::NiPoint3& a_center, const RE::NiPoint3& a_extents, const HiZReadback::Snapshot& a_snap)
		{
			const float mdx = g_posAdjust.x - a_snap.posAdjust[0];
			const float mdy = g_posAdjust.y - a_snap.posAdjust[1];
			const float mdz = g_posAdjust.z - a_snap.posAdjust[2];
			const float motion = std::sqrt(mdx * mdx + mdy * mdy + mdz * mdz);
			if (motion > HiZMaxCameraMotion)
				return true;

			// Grown by the camera's travel since the capture, for the same reason the sphere is.
			const float ex = a_extents.x + motion;
			const float ey = a_extents.y + motion;
			const float ez = a_extents.z + motion;

			const float cx = a_center.x - a_snap.posAdjust[0];
			const float cy = a_center.y - a_snap.posAdjust[1];
			const float cz = a_center.z - a_snap.posAdjust[2];

			// Camera inside the box: it cannot be behind anything.
			if (std::abs(cx) <= ex && std::abs(cy) <= ey && std::abs(cz) <= ez)
				return true;

			float ndcMinX = FLT_MAX, ndcMinY = FLT_MAX;
			float ndcMaxX = -FLT_MAX, ndcMaxY = -FLT_MAX;
			float nearZ = FLT_MAX;
			float nearW = FLT_MAX;

			for (int i = 0; i < 8; ++i) {
				const XMVECTOR corner = _mm_setr_ps(
					cx + ((i & 1) ? ex : -ex),
					cy + ((i & 2) ? ey : -ey),
					cz + ((i & 4) ? ez : -ez), 1.0f);

				const XMVECTOR clip = XMVector4Transform(corner, a_snap.viewProj);
				const float    w = clip.m128_f32[3];
				if (w <= 1e-4f)
					return true;  // straddles the captured near plane; no usable rect

				const float invW = 1.0f / w;
				const float x = clip.m128_f32[0] * invW;
				const float y = clip.m128_f32[1] * invW;
				ndcMinX = std::min(ndcMinX, x);
				ndcMaxX = std::max(ndcMaxX, x);
				ndcMinY = std::min(ndcMinY, y);
				ndcMaxY = std::max(ndcMaxY, y);

				// The nearest corner decides: if even that is behind, every part of the box is.
				const float z = clip.m128_f32[2] * invW;
				if (w < nearW) {
					nearW = w;
					nearZ = z;
				}
			}

			// NDC -> level-0 texels. y is flipped, as in the sphere path.
			const float tx0 = (ndcMinX * 0.5f + 0.5f) * a_snap.validWidth;
			const float tx1 = (ndcMaxX * 0.5f + 0.5f) * a_snap.validWidth;
			const float ty0 = (ndcMaxY * -0.5f + 0.5f) * a_snap.validHeight;
			const float ty1 = (ndcMinY * -0.5f + 0.5f) * a_snap.validHeight;

			// Level where the rect spans about two texels, so a 3x3 block covers it.
			const float spanT = std::max(tx1 - tx0, ty1 - ty0);
			const float wantLevel = std::ceil(std::log2(std::max(spanT, 1.0f)));
			if (!(wantLevel >= 0.0f) || wantLevel > float(a_snap.mipCount) - 1.0f)
				return true;

			const int   level = int(wantLevel);
			const float scale = std::exp2(float(level));
			const int   x0 = int(std::floor(tx0 / scale));
			const int   y0 = int(std::floor(ty0 / scale));
			const int   validW = std::max(1, int(std::ceil(a_snap.validWidth / scale)));
			const int   validH = std::max(1, int(std::ceil(a_snap.validHeight / scale)));

			// Wholly inside the captured image; anything reaching an edge may have been
			// off-screen when the depth was taken.
			if (x0 < 0 || y0 < 0 || x0 + 2 > validW - 1 || y0 + 2 > validH - 1)
				return true;

			const float tileMax = a_snap.TileMax(level, x0, y0, validW, validH);
			return !(nearZ > tileMax + HiZOcclusionBias);
		}
	}

	void BeginFrame()
	{
		// DIAG (CS_OCCLUSION_STATS=1): the previous frame's counters, before they are reset.
		static const bool s_logStats = [] {
			char buf[8] = {};
			return GetEnvironmentVariableA("CS_OCCLUSION_STATS", buf, sizeof(buf)) && buf[0] == '1';
		}();
		if (s_logStats && (g_frameIndex.load(std::memory_order_relaxed) % 300u) == 0u) {
			logger::info("[OcclusionCulling] tested={} culled={} (sphere={} aabb={}) flips={} deferred={} smallObj={} smallShadow={} age={}",
				g_tested.load(), g_culled.load(), g_culledSphere.load(), g_culledAABB.load(),
				g_flips.load(), g_deferred.load(),
				g_smallObjects.load(), g_smallShadows.load(), HiZReadback::GetStats().age);
		}

		g_frameIndex.fetch_add(1, std::memory_order_relaxed);
		g_tested.store(0, std::memory_order_relaxed);
		g_culled.store(0, std::memory_order_relaxed);
		g_flips.store(0, std::memory_order_relaxed);
		g_deferred.store(0, std::memory_order_relaxed);
		g_culledSphere.store(0, std::memory_order_relaxed);
		g_culledAABB.store(0, std::memory_order_relaxed);
		g_smallObjects.store(0, std::memory_order_relaxed);
		g_smallShadows.store(0, std::memory_order_relaxed);
	}

	std::uint32_t CurrentFrame()
	{
		return g_frameIndex.load(std::memory_order_relaxed);
	}

	void CameraViewProj(RE::NiCamera* a_camera, XMMATRIX& a_outViewProj, RE::NiPoint3& a_outPosAdjust)
	{
		if (!a_camera) {
			a_outViewProj = XMMatrixIdentity();
			a_outPosAdjust = RE::NiPoint3{ 0.0f, 0.0f, 0.0f };
			return;
		}
		XMMATRIX view, proj;
		CalculateViewProjection(a_camera, view, proj, a_outViewProj);
		a_outPosAdjust = a_camera->world.translate;
	}

	bool BeginCull(const RE::NiCamera* a_camera)
	{
		if (!EnableOcclusionTesting || !a_camera)
			return false;

		// Always the MAIN camera, never this pass's. Shadow and reflection culls run interleaved
		// and their cameras sit far away; latching one of those makes the motion check see a huge
		// delta and refuse every snapshot.
		if (auto* mainCam = GetMainCamera())
			g_posAdjust = mainCam->world.translate;

		// The sun's caster-gather camera, so the small-shadow cull knows which walk this is.
		const RE::NiCamera* sunCam = nullptr;
		if (CullSmallShadows) {
			if (auto* smState = globals::game::smState) {
				if (auto* ssn = smState->shadowSceneNode[0]) {
					if (auto* dirLight = ssn->GetRuntimeData().sunShadowDirLight)
						sunCam = dirLight->GetShadowDirectionalLightRuntimeData().fullFrustumCamera.get();
				}
			}
		}
		g_sunGatherCam.store(sunCam, std::memory_order_release);

		return true;
	}

	bool TestObject(RE::NiAVObject* a_object)
	{
		if (!EnableOcclusionTesting || !a_object)
			return true;

		// CHEAP-FIRST gate order: this runs thousands of times per frame, so plain member reads
		// come before anything virtual and all RTTI stays at the end, paid only by survivors.
		const auto& bound = a_object->worldBound;

		if (a_object->GetAppCulled())
			return true;

		// kAlwaysDraw and kPreProcessedNode are the engine's own shortcuts. Effect nodes carry
		// them, and their bounds describe an emitter rather than the particles it has thrown.
		if (const auto fl = a_object->GetFlags().underlying(); fl & 0x800 || fl & 0x1000)
			return true;

		// Never cull actors: skinned world bounds lag animation. formType is a plain member read.
		auto* ref = a_object->GetUserData();
		if (ref && ref->formType == RE::FormType::ActorCharacter)
			return true;

		if (CullSmallObjects && bound.radius > 0.0f &&
			!KeepBySize(bound.center, bound.radius, ObjectCullNearRadius, ObjectCullDistSlope)) {
			g_smallObjects.fetch_add(1, std::memory_order_relaxed);
			g_culled.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		if (bound.radius < ObjectTestMinRadius)
			return true;

		// Distant-tree LOD is tested per instance group from the OnVisible hook. Judging the node
		// here too would condemn a whole group on one sphere spanning the entire LOD block.
		if (netimmerse_cast<RE::BSMultiStreamInstanceTriShape*>(a_object))
			return true;

		const auto* snap = HiZReadback::Current();
		if (!snap)
			return true;  // nothing collected yet

		const bool hidden = !TestSphereHiZ(bound.center, bound.radius, *snap);
		g_tested.fetch_add(1, std::memory_order_relaxed);

		if (!Stabilize(a_object, hidden))
			return true;

		g_culled.fetch_add(1, std::memory_order_relaxed);
		g_culledSphere.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	bool TestMultiBound(void* a_multiBound)
	{
		if (!EnableOcclusionTesting || !a_multiBound)
			return true;

		auto* mb = static_cast<RE::BSMultiBound*>(a_multiBound);
		auto* aabb = netimmerse_cast<RE::BSMultiBoundAABB*>(mb->data.get());
		if (!aabb || aabb->size.z <= 1.0f)
			return true;  // no AABB shape (spheres etc.) or degenerate bounds

		const auto* snap = HiZReadback::Current();
		if (!snap)
			return true;

		const bool hidden = !TestAABBHiZ(aabb->center, aabb->size, *snap);
		g_tested.fetch_add(1, std::memory_order_relaxed);

		// Same hysteresis as objects: a container that flips takes its whole contents with it.
		if (!Stabilize(aabb, hidden))
			return true;

		g_culled.fetch_add(1, std::memory_order_relaxed);
		g_culledAABB.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	bool TestInstanceGroup(RE::BSMultiBoundAABB* a_aabb)
	{
		if (!EnableOcclusionTesting || !CullTreeLODGroups || !a_aabb)
			return true;

		const auto* snap = HiZReadback::Current();
		if (!snap)
			return true;

		const bool hidden = !TestAABBHiZ(a_aabb->center, a_aabb->size, *snap);
		g_tested.fetch_add(1, std::memory_order_relaxed);

		// Groups sit far away, where the chosen mip level is closest to a boundary, and each
		// covers many trees.
		if (!Stabilize(a_aabb, hidden))
			return true;

		g_culled.fetch_add(1, std::memory_order_relaxed);
		g_culledAABB.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	bool TestShadowCasterSmall(RE::NiAVObject* a_object)
	{
		// Behind the master switch as well as its own, so toggling the feature is a true A/B.
		if (!EnableOcclusionTesting || !CullSmallShadows || !a_object)
			return true;
		const auto& b = a_object->worldBound;
		if (b.radius <= 0.0f)
			return true;
		const bool keep = KeepBySize(b.center, b.radius, ShadowCullNearRadius, ShadowCullDistSlope);
		if (!keep)
			g_smallShadows.fetch_add(1, std::memory_order_relaxed);
		return keep;
	}

	bool IsSceneListCamera(const RE::NiCamera* a_camera)
	{
		return a_camera && a_camera == GetMainCamera();
	}

	bool IsSunGatherCamera(const RE::NiCamera* a_camera)
	{
		return a_camera && a_camera == g_sunGatherCam.load(std::memory_order_acquire);
	}

	Stats GetStats()
	{
		return Stats{
			g_tested.load(std::memory_order_relaxed),
			g_culled.load(std::memory_order_relaxed),
			g_smallObjects.load(std::memory_order_relaxed),
			g_smallShadows.load(std::memory_order_relaxed),
			HiZReadback::GetStats().age
		};
	}
}
