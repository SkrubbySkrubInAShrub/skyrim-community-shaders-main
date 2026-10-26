// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Deferred.h"
#include "Features/Skylighting.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

// Snow on bodies, stage R0 (MD plans/BODY-SNOW-PLAN.md): read-only probes.
// Rationale in CODE-NOTES.md, "Snow on bodies: R0 probes".

namespace
{
	constexpr uint32_t kDumpLinesMax = 600;

	bool IsLit(const RE::BSShaderProperty* a_property)
	{
		return a_property && a_property->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get();
	}

	RE::BSShaderMaterial::Feature FeatureOf(const RE::BSShaderProperty* a_property)
	{
		auto* material = a_property ? static_cast<RE::BSLightingShaderMaterialBase*>(a_property->material) : nullptr;
		return material ? material->GetFeature() : RE::BSShaderMaterial::Feature::kDefault;
	}

	float RotationDeviation(const RE::NiMatrix3& a_m)
	{
		float worst = 0.0f;
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				worst = std::max(worst, std::abs(a_m.entry[r][c] - (r == c ? 1.0f : 0.0f)));
		return worst;
	}

	bool HiddenByGame(const RE::NiAVObject* a_geometry, const RE::NiAVObject* a_root, bool a_firstPersonPlayer)
	{
		for (const RE::NiAVObject* n = a_geometry; n; n = n->parent)
			if (n->GetAppCulled() && !(a_firstPersonPlayer && n == a_root))
				return true;
		return false;
	}

	const char* NameOf(const RE::NiAVObject* a_object)
	{
		return a_object && a_object->name.c_str() ? a_object->name.c_str() : "";
	}
}

void SnowDeformation::TickBodyCoatProbes()
{
	if (!debugBodyCoatDrawProbe && bodyCoatProbeBound) {
		ID3D11ShaderResourceView* none = nullptr;
		globals::d3d::context->PSSetShaderResources(kBodyCoatProbeSlot, 1, &none);
		bodyCoatProbeBound = false;
		bodyCoatDrawFrame = UINT32_MAX;
	}
	const bool dump = debugBodyCoatCensusDump;
	if (!debugBodyCoatCensus && !dump)
		return;
	const uint32_t frame = globals::state->frameCount;
	if (!dump && bodyCoatCensusRan && frame - bodyCoatCensusFrame < kBodyCoatCensusFrames)
		return;
	debugBodyCoatCensusDump = false;
	bodyCoatCensusFrame = frame;
	bodyCoatCensusRan = true;
	const auto start = std::chrono::steady_clock::now();
	RunBodyCoatCensus(dump);
	bodyCoatCensusMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// The coat's near tier, as the contact capture picks it: the player and the
// nearest living actors whose bound lies inside the contact window, then the
// nearest corpses, each list at the capture's own cap.
void SnowDeformation::RunBodyCoatCensus(bool a_dump)
{
	bodyCoatCensus.clear();
	auto* player = globals::game::player;
	if (!player)
		return;
	const RE::NiPoint3 eye = Util::GetEyePosition();
	struct Pick
	{
		float d2;
		RE::NiPointer<RE::Actor> actor;
	};
	std::vector<Pick> living;
	std::vector<Pick> dead;
	auto consider = [&](RE::NiPointer<RE::Actor> a_actor, float a_d2) {
		if (!a_actor || !a_actor->Is3DLoaded())
			return;
		auto* root = a_actor->Get3D(false);
		if (!root)
			return;
		const auto& b = root->worldBound;
		if (b.radius <= 0.0f || std::abs(b.center.x - eye.x) + b.radius >= kContactHalfExtent ||
			std::abs(b.center.y - eye.y) + b.radius >= kContactHalfExtent)
			return;
		(a_actor->IsDead() ? dead : living).push_back({ a_d2, a_actor });
	};
	consider(RE::NiPointer<RE::Actor>(player), -1.0f);
	if (const auto processLists = RE::ProcessLists::GetSingleton())
		for (auto& handle : processLists->highActorHandles)
			if (auto actor = handle.get(); actor)
				consider(actor, eye.GetSquaredDistance(actor->GetPosition()));
	auto byDistance = [](const Pick& a_a, const Pick& a_b) { return a_a.d2 < a_b.d2; };
	std::sort(living.begin(), living.end(), byDistance);
	std::sort(dead.begin(), dead.end(), byDistance);
	if (living.size() > kContactMaxActors)
		living.resize(kContactMaxActors);
	if (dead.size() > kContactMaxCorpses)
		dead.resize(kContactMaxCorpses);

	uint32_t dumpLines = 0;
	auto dumpLine = [&](const std::string& a_line) {
		if (a_dump && dumpLines++ < kDumpLinesMax)
			logger::info("[SNOW DEFORMATION] body coat census: {}", a_line);
	};
	const bool firstPerson = RE::PlayerCamera::GetSingleton() && RE::PlayerCamera::GetSingleton()->IsInFirstPerson();

	struct Skin
	{
		RE::BSGeometry* geometry;
		RE::NiSkinInstance* skin;
		RE::NiSkinData* data;
		uint32_t bones;
	};
	// Every bone a skin names, at its bind position in the canonical frame:
	// rootParentToSkin undone, so the frames of all an actor's meshes line up.
	auto forEachBone = [](const Skin& a_skin, auto&& a_visit) {
		const RE::NiTransform toCanonical = a_skin.data->rootParentToSkin.Invert();
		const uint32_t count = std::min(a_skin.data->GetBoneCount(), a_skin.bones);
		for (uint32_t b = 0; b < count; ++b) {
			const auto* node = a_skin.skin->bones[b];
			if (!node)
				continue;
			const RE::NiTransform boneToSkin = a_skin.data->GetBoneDataSkinToBone(b).Invert();
			const RE::NiPoint3 bind = toCanonical * boneToSkin.translate;
			const auto& bound = a_skin.data->GetBoneDataBound(b);
			const RE::NiPoint3 centre = toCanonical * (boneToSkin * bound.center);
			a_visit(NameOf(node), bind, centre, bound.radius * boneToSkin.scale * toCanonical.scale);
		}
	};

	auto censusOf = [&](RE::Actor* a_actor, float a_d2, bool a_corpse) {
		BodyCoatActorCensus c;
		const char* display = a_actor->GetDisplayFullName();
		c.name = display ? display : "";
		c.formID = a_actor->formID;
		c.distance = a_d2 > 0.0f ? std::sqrt(a_d2) : 0.0f;
		c.corpse = a_corpse;
		if (auto* state = a_actor->AsActorState())
			c.weaponDrawn = state->IsWeaponDrawn();
		auto* root = a_actor->Get3D(false);
		const bool firstPersonPlayer = firstPerson && a_actor == player;
		std::vector<Skin> skins;
		std::vector<std::string> shapeLines;
		auto visit = [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
			auto& runtime = a_geometry->GetGeometryRuntimeData();
			auto* property = runtime.shaderProperty.get();
			if (!IsLit(property) || HiddenByGame(a_geometry, root, firstPersonPlayer) || IsProxyOrOverlayGeometry(a_geometry, property))
				return RE::BSVisit::BSVisitControl::kContinue;
			if (a_geometry->GetUserData() == a_actor)
				c.owned++;
			else
				c.unowned++;
			auto* triShape = a_geometry->AsTriShape();
			const uint32_t triangles = triShape ? uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) : 0u;
			auto* skin = runtime.skinInstance.get();
			auto* data = skin ? skin->skinData.get() : nullptr;
			if (!skin || !data || !skin->bones) {
				if (skin)
					return RE::BSVisit::BSVisitControl::kContinue;
				c.gear++;
				c.gearTriangles += triangles;
				c.gearRadiusMax = std::max(c.gearRadiusMax, a_geometry->worldBound.radius);
				if (a_dump)
					shapeLines.push_back(std::format("  gear '{}' under '{}': radius {:.0f}, {} triangles, owner {}", NameOf(a_geometry),
						NameOf(a_geometry->parent), a_geometry->worldBound.radius, triangles, a_geometry->GetUserData() == a_actor ? "this actor" : "NOT this actor"));
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			skins.push_back({ a_geometry, skin, data, data->GetBoneCount() });
			c.skinned++;
			c.triangles += triangles;
			if (a_geometry->AsDynamicTriShape())
				c.dynamic++;
			const auto feature = FeatureOf(property);
			if (feature == RE::BSShaderMaterial::Feature::kFaceGen || feature == RE::BSShaderMaterial::Feature::kFaceGenRGBTint)
				c.skinShapes++;
			if (feature == RE::BSShaderMaterial::Feature::kEye)
				c.eyeShapes++;
			auto* partition = skin->skinPartition.get();
			const RE::BSGraphics::TriShape* buffer = partition && partition->numPartitions > 0 && partition->partitions.data() ? partition->partitions[0].buffData : nullptr;
			if (buffer && !buffer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
				c.noNormals++;
			return RE::BSVisit::BSVisitControl::kContinue;
		};
		RE::BSVisit::TraverseScenegraphGeometries(root, std::ref(visit));

		// The skin with the most bones (the body, as a rule) sets the frame;
		// each later skin is measured against the bones set before it.
		std::sort(skins.begin(), skins.end(), [](const Skin& a_a, const Skin& a_b) { return a_a.bones > a_b.bones; });
		std::unordered_map<std::string, RE::NiPoint3> frame;
		for (const auto& s : skins) {
			const auto& rps = s.data->rootParentToSkin;
			c.rawOffsetMax = std::max(c.rawOffsetMax, rps.translate.Length());
			const bool transformed = RotationDeviation(rps.rotate) > 1e-3f || std::abs(rps.scale - 1.0f) > 1e-3f;
			c.transformed += transformed ? 1u : 0u;
			float deviation = 0.0f;
			std::string worstBone;
			uint32_t shared = 0;
			std::vector<std::pair<std::string, RE::NiPoint3>> added;
			forEachBone(s, [&](const char* a_name, const RE::NiPoint3& a_bind, const RE::NiPoint3& a_centre, float a_radius) {
				c.boundsMin = { std::min(c.boundsMin.x, a_centre.x - a_radius), std::min(c.boundsMin.y, a_centre.y - a_radius), std::min(c.boundsMin.z, a_centre.z - a_radius) };
				c.boundsMax = { std::max(c.boundsMax.x, a_centre.x + a_radius), std::max(c.boundsMax.y, a_centre.y + a_radius), std::max(c.boundsMax.z, a_centre.z + a_radius) };
				if (auto it = frame.find(a_name); it != frame.end()) {
					shared++;
					const float d = it->second.GetDistance(a_bind);
					if (d > deviation) {
						deviation = d;
						worstBone = a_name;
					}
				} else {
					added.emplace_back(a_name, a_bind);
				}
			});
			for (auto& [name, bind] : added)
				frame.emplace(std::move(name), bind);
			if (&s != &skins.front() && shared == 0)
				c.unchecked++;
			if (deviation > kBodyCoatVoxel)
				c.offFrame++;
			if (deviation > c.worstDeviation) {
				c.worstDeviation = deviation;
				c.worstShape = NameOf(s.geometry);
			}
			if (a_dump) {
				const auto feature = FeatureOf(s.geometry->GetGeometryRuntimeData().shaderProperty.get());
				shapeLines.push_back(std::format("  skin '{}': {} bones, {} shared, off frame by {:.2f}{}; rootParentToSkin ({:.1f}, {:.1f}, {:.1f}) rotation off {:.3f} scale {:.3f}; feature {}{}",
					NameOf(s.geometry), s.bones, shared, deviation, worstBone.empty() ? "" : std::format(" at '{}'", worstBone),
					rps.translate.x, rps.translate.y, rps.translate.z, RotationDeviation(rps.rotate), rps.scale, int(feature),
					s.geometry->AsDynamicTriShape() ? ", dynamic" : ""));
			}
		}

		// First person: separate meshes on the first-person skeleton, matched
		// to the third-person frame by bone name.
		if (a_actor == player) {
			auto* first = player->Get3D(true);
			if (first && first != root) {
				auto visitFirst = [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
					auto& runtime = a_geometry->GetGeometryRuntimeData();
					auto* property = runtime.shaderProperty.get();
					if (!IsLit(property) || IsProxyOrOverlayGeometry(a_geometry, property))
						return RE::BSVisit::BSVisitControl::kContinue;
					if (a_geometry->GetUserData() == a_actor)
						c.firstOwned++;
					else
						c.firstUnowned++;
					auto* skin = runtime.skinInstance.get();
					auto* data = skin ? skin->skinData.get() : nullptr;
					if (!skin || !data || !skin->bones)
						return RE::BSVisit::BSVisitControl::kContinue;
					c.firstSkinned++;
					float deviation = -1.0f;
					forEachBone({ a_geometry, skin, data, data->GetBoneCount() }, [&](const char* a_name, const RE::NiPoint3& a_bind, const RE::NiPoint3&, float) {
						if (auto it = frame.find(a_name); it != frame.end())
							deviation = std::max(deviation, it->second.GetDistance(a_bind));
					});
					c.firstDeviation = std::max(c.firstDeviation, deviation);
					if (a_dump)
						shapeLines.push_back(std::format("  first-person skin '{}': off the third-person frame by {}, owner {}", NameOf(a_geometry),
							deviation < 0.0f ? std::string("(no bone shared)") : std::format("{:.2f}", deviation), a_geometry->GetUserData() == a_actor ? "the player" : "NOT the player"));
					return RE::BSVisit::BSVisitControl::kContinue;
				};
				RE::BSVisit::TraverseScenegraphGeometries(first, std::ref(visitFirst));
			}
		}
		if (a_dump) {
			dumpLine(std::format("{:08X} '{}' {:.1f} m{}{}: {} skins, worst {:.2f} off frame, {} gear, {} lit geometries owned / {} not", c.formID, c.name,
				c.distance / kUnitsPerMeter, c.corpse ? " corpse" : "", c.weaponDrawn ? " weapon drawn" : "", c.skinned, c.worstDeviation, c.gear, c.owned, c.unowned));
			for (const auto& line : shapeLines)
				dumpLine(line);
		}
		return c;
	};

	if (a_dump)
		logger::info("[SNOW DEFORMATION] body coat census: {} living, {} corpses inside the contact window; each actor's line is followed by its shapes", living.size(), dead.size());
	for (const auto& pick : living)
		bodyCoatCensus.push_back(censusOf(pick.actor.get(), pick.d2, false));
	for (const auto& pick : dead)
		bodyCoatCensus.push_back(censusOf(pick.actor.get(), pick.d2, true));
}

void SnowDeformation::ProbeBodyCoatDraw(RE::BSRenderPass* a_pass)
{
	if (!debugBodyCoatDrawProbe || !a_pass || !a_pass->geometry)
		return;
	auto* context = globals::d3d::context;
	if (!bodyCoatProbeSRV) {
		D3D11_TEXTURE3D_DESC desc{ 1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
		const uint32_t zero = 0;
		D3D11_SUBRESOURCE_DATA init{ &zero, 4, 4 };
		if (FAILED(globals::d3d::device->CreateTexture3D(&desc, &init, bodyCoatProbeTexture.put())) ||
			FAILED(globals::d3d::device->CreateShaderResourceView(bodyCoatProbeTexture.get(), nullptr, bodyCoatProbeSRV.put()))) {
			debugBodyCoatDrawProbe = false;
			logger::warn("[SNOW DEFORMATION] body coat probe: could not create the probe texture");
			return;
		}
		Util::SetResourceName(bodyCoatProbeTexture.get(), "SnowDeformation::BodyCoatProbe");
		Util::SetResourceName(bodyCoatProbeSRV.get(), "SnowDeformation::BodyCoatProbe SRV");
	}
	ID3D11ShaderResourceView* probe = bodyCoatProbeSRV.get();

	// Once a frame, at its first Lighting pass: the bind the coat would make.
	const uint32_t frame = globals::state->frameCountAtomic.load(std::memory_order_relaxed);
	auto& t = bodyCoatDrawBuilding;
	if (frame != bodyCoatDrawFrame) {
		if (bodyCoatDrawFrame != UINT32_MAX) {
			t.frames++;
			t.framesWithLoss += bodyCoatDrawLostThisFrame ? 1u : 0u;
			if (t.frames >= kBodyCoatDrawWindow) {
				bodyCoatDrawLast = t;
				logger::info("[SNOW DEFORMATION] body coat draw probe over {} frames: {} skinned draws ({} name an actor, {} no owner, {} another form), {} rigid draws name an actor; owned draws deferred {} / forward {} / reflections {}, probe bind lost before {} / {} / {}, in {} frames",
					t.frames, t.skinnedDraws, t.skinnedOwned, t.skinnedNoOwner, t.skinnedOtherOwner, t.rigidOwned, t.ownedDeferred, t.ownedForward, t.ownedReflection,
					t.lostDeferred, t.lostForward, t.lostReflection, t.framesWithLoss);
				t = {};
			}
		}
		bodyCoatDrawFrame = frame;
		bodyCoatDrawLostThisFrame = false;
		context->PSSetShaderResources(kBodyCoatProbeSlot, 1, &probe);
		bodyCoatProbeBound = true;
	}

	t.lightingDraws++;
	auto* geometry = a_pass->geometry;
	const bool skinned = geometry->GetGeometryRuntimeData().skinInstance != nullptr;
	auto* owner = geometry->GetUserData();
	const bool actorOwner = owner && owner->formType == RE::FormType::ActorCharacter;
	if (skinned) {
		t.skinnedDraws++;
		if (actorOwner)
			t.skinnedOwned++;
		else if (!owner)
			t.skinnedNoOwner++;
		else
			t.skinnedOtherOwner++;
	} else if (actorOwner) {
		t.rigidOwned++;
	}
	if (!actorOwner)
		return;

	const bool reflection = globals::state->activeReflections;
	const bool deferred = !reflection && globals::deferred && globals::deferred->deferredPass;
	(reflection ? t.ownedReflection : deferred ? t.ownedDeferred : t.ownedForward)++;
	ID3D11ShaderResourceView* bound = nullptr;
	context->PSGetShaderResources(kBodyCoatProbeSlot, 1, &bound);
	const bool held = bound == probe;
	if (bound)
		bound->Release();
	if (!held) {
		(reflection ? t.lostReflection : deferred ? t.lostDeferred : t.lostForward)++;
		bodyCoatDrawLostThisFrame = true;
		context->PSSetShaderResources(kBodyCoatProbeSlot, 1, &probe);
	}
}

std::vector<std::string> SnowDeformation::BodyCoatProbeLines() const
{
	std::vector<std::string> lines;
	if (!bodyCoatCensusRan) {
		lines.push_back("Census: not run yet.");
	} else {
		uint32_t skins = 0, offFrame = 0, transformed = 0, unchecked = 0, noNormals = 0, dynamic = 0, gear = 0, owned = 0, unowned = 0;
		uint64_t triangles = 0;
		float worst = 0.0f, rawOffset = 0.0f;
		for (const auto& c : bodyCoatCensus) {
			skins += c.skinned;
			offFrame += c.offFrame;
			transformed += c.transformed;
			unchecked += c.unchecked;
			noNormals += c.noNormals;
			dynamic += c.dynamic;
			gear += c.gear;
			owned += c.owned;
			unowned += c.unowned;
			triangles += c.triangles + c.gearTriangles;
			worst = std::max(worst, c.worstDeviation);
			rawOffset = std::max(rawOffset, c.rawOffsetMax);
		}
		lines.push_back(std::format("Census {:.0f} frames ago, {:.2f} ms: {} actors, {} skins, {} carried meshes, {} triangles in all.",
			double(globals::state->frameCount - bodyCoatCensusFrame), bodyCoatCensusMs, bodyCoatCensus.size(), skins, gear, triangles));
		lines.push_back(std::format("Frames: raw skin frames up to {:.1f} units apart; once undone, the worst skin lies {:.2f} from its actor's frame; {} skins past a voxel ({:.1f}), {} rotate or scale, {} share no bone (unchecked).",
			rawOffset, worst, offFrame, kBodyCoatVoxel, transformed, unchecked));
		lines.push_back(std::format("Skins without vertex normals {}, dynamic (morphed bodies, facegen) {}. Owners: {} lit geometries name their actor, {} do not.", noNormals, dynamic, owned, unowned));
		for (const auto& c : bodyCoatCensus) {
			const RE::NiPoint3 size = c.boundsMax - c.boundsMin;
			const bool sized = c.skinned > 0 && size.x > 0.0f;
			lines.push_back(std::format("{} ({:.0f} m{}{}): {} skins ({} skin, {} eyes, {} dynamic, {} no normals), worst {:.2f}{}; frame {} ; {} triangles; gear {} ({} tris, radius up to {:.0f}); owners {}/{}",
				c.name.empty() ? std::format("{:08X}", c.formID) : c.name, c.distance / kUnitsPerMeter, c.corpse ? ", corpse" : "", c.weaponDrawn ? ", weapon drawn" : "",
				c.skinned, c.skinShapes, c.eyeShapes, c.dynamic, c.noNormals, c.worstDeviation, c.worstShape.empty() ? "" : std::format(" ('{}')", c.worstShape),
				sized ? std::format("{:.0f} x {:.0f} x {:.0f} units, voxels {:.1f} / {:.1f} / {:.1f}", size.x, size.y, size.z, size.x / 32.0f, size.y / 16.0f, size.z / 48.0f) : std::string("(no skins)"),
				c.triangles, c.gear, c.gearTriangles, c.gearRadiusMax, c.owned, c.owned + c.unowned));
			if (c.firstSkinned > 0 || c.firstOwned + c.firstUnowned > 0)
				lines.push_back(std::format("  first person: {} skins, {}, owners {}/{}", c.firstSkinned,
					c.firstDeviation < 0.0f ? std::string("no bone shared with the third-person frame") : std::format("worst {:.2f} from the third-person frame", c.firstDeviation),
					c.firstOwned, c.firstOwned + c.firstUnowned));
		}
	}

	if (!debugBodyCoatDrawProbe) {
		lines.push_back("Draw probe: off.");
	} else if (bodyCoatDrawLast.frames == 0) {
		lines.push_back(std::format("Draw probe: measuring, {} of {} frames.", bodyCoatDrawBuilding.frames, kBodyCoatDrawWindow));
	} else {
		const auto& t = bodyCoatDrawLast;
		const double frames = double(t.frames);
		lines.push_back(std::format("Draw probe, last {} frames, per frame: {:.0f} Lighting draws, {:.1f} skinned ({:.1f} name an actor, {:.1f} no owner, {:.1f} another form), {:.1f} rigid draws name an actor.",
			t.frames, t.lightingDraws / frames, t.skinnedDraws / frames, t.skinnedOwned / frames, t.skinnedNoOwner / frames, t.skinnedOtherOwner / frames, t.rigidOwned / frames));
		lines.push_back(std::format("Probe bind (t{}, once a frame) at owned draws: deferred {} lost of {}, forward {} of {}, reflections {} of {}; frames with a loss {} of {}.",
			kBodyCoatProbeSlot, t.lostDeferred, t.ownedDeferred, t.lostForward, t.ownedForward, t.lostReflection, t.ownedReflection, t.framesWithLoss, t.frames));
	}

	const auto& sky = globals::features::skylighting;
	if (!sky.loaded) {
		lines.push_back("Skylighting: not loaded. Snowfall shelter would fall back to the precipitation occlusion map.");
	} else {
		const float halfAcross = sky.occlusionDistance * 0.5f;
		const float tall = sky.occlusionDistance * 0.5f;
		lines.push_back(std::format("Skylighting: loaded; probe grid {}x{}x{}, +-{:.0f} units across and {:.0f} tall around the camera; the near tier reaches +-{:.0f}, so it is {}.",
			sky.probeArrayDims[0], sky.probeArrayDims[1], sky.probeArrayDims[2], halfAcross, tall, kContactHalfExtent,
			halfAcross >= kContactHalfExtent ? "inside the grid" : "partly outside the grid"));
	}
	return lines;
}
