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
	constexpr uint32_t kDumpLinesMax = 900;

	bool IsLit(const RE::BSShaderProperty* a_property)
	{
		return a_property && a_property->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get();
	}

	RE::BSShaderMaterial::Feature FeatureOf(const RE::BSShaderProperty* a_property)
	{
		auto* material = a_property ? static_cast<RE::BSLightingShaderMaterialBase*>(a_property->material) : nullptr;
		return material ? material->GetFeature() : RE::BSShaderMaterial::Feature::kDefault;
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

	struct SkinBone
	{
		std::string name;
		const RE::NiAVObject* node;
		RE::NiTransform skinToBone;
		/** @brief The bone's bind pose in the skin's own space; its translate is the bone's origin there. */
		RE::NiTransform boneToSkin;
		RE::NiBound bound;
	};

	std::vector<SkinBone> BonesOf(const RE::NiSkinInstance* a_skin, const RE::NiSkinData* a_data)
	{
		std::vector<SkinBone> bones;
		const uint32_t count = a_data->GetBoneCount();
		bones.reserve(count);
		for (uint32_t b = 0; b < count; ++b) {
			const RE::NiAVObject* node = a_skin->bones[b];
			if (!node)
				continue;
			const RE::NiTransform& skinToBone = a_data->GetBoneDataSkinToBone(b);
			bones.push_back({ NameOf(node), node, skinToBone, skinToBone.Invert(), a_data->GetBoneDataBound(b) });
		}
		return bones;
	}

	/** @brief Each bone's bind pose in the anchor bone's space, by name. */
	using BindMap = std::unordered_map<std::string, RE::NiTransform>;

	struct SkinMap
	{
		bool chained = false;
		/** @brief The skin's space into the anchor's: bind(via) x skinToBone(via). */
		RE::NiTransform toAnchor;
		std::string via;
		uint32_t shared = 0;
		uint32_t agree = 0;
		float residual = 0.0f;
		/** @brief Shared bones the chosen map puts more than a voxel off: name, distance, bone-sphere radius. */
		std::vector<std::tuple<std::string, float, float>> outliers;
	};

	// The transform skinning itself applies: a vertex bound to bone b lands at
	// bone world x skinToBone(b) x v, so bind(b) x skinToBone(b) maps the skin
	// into the anchor's space whatever its rootParentToSkin claims. Every
	// shared bone proposes one; the proposal most of them agree with wins, so
	// a bone a skin lists but never weights cannot pull it off.
	SkinMap MapSkin(const std::vector<SkinBone>& a_bones, const BindMap& a_bind, float a_voxel)
	{
		SkinMap best;
		std::vector<std::pair<const SkinBone*, const RE::NiTransform*>> known;
		for (const auto& bone : a_bones)
			if (auto it = a_bind.find(bone.name); it != a_bind.end())
				known.emplace_back(&bone, &it->second);
		best.shared = uint32_t(known.size());
		if (known.empty())
			return best;
		int bestAgree = -1;
		float bestResidual = 0.0f;
		for (const auto& [candidate, candidateBind] : known) {
			const RE::NiTransform map = *candidateBind * candidate->skinToBone;
			int agree = 0;
			float residual = 0.0f;
			for (const auto& [bone, bind] : known) {
				const float r = (map * bone->boneToSkin.translate).GetDistance(bind->translate);
				if (r <= a_voxel) {
					agree++;
					residual = std::max(residual, r);
				}
			}
			const bool better = agree > bestAgree || (agree == bestAgree && (residual < bestResidual || (residual == bestResidual && candidate->name < best.via)));
			if (better) {
				bestAgree = agree;
				bestResidual = residual;
				best.toAnchor = map;
				best.via = candidate->name;
			}
		}
		best.chained = true;
		best.agree = uint32_t(bestAgree);
		best.residual = bestResidual;
		for (const auto& [bone, bind] : known) {
			const float r = (best.toAnchor * bone->boneToSkin.translate).GetDistance(bind->translate);
			if (r > a_voxel)
				best.outliers.emplace_back(bone->name, r, bone->bound.radius);
		}
		return best;
	}

	std::string OutlierText(const SkinMap& a_map)
	{
		std::string text;
		for (const auto& [name, distance, radius] : a_map.outliers)
			text += std::format("{}'{}' {:.1f} off (bone sphere {:.1f})", text.empty() ? "" : ", ", name, distance, radius);
		return text;
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

		// What the actor has equipped, by biped slot: each item's attached 3D.
		// Gear is what hangs below a weapon, shield or quiver slot; a rigid mesh
		// below no equipped slot at all is something stuck on the body.
		std::vector<std::pair<const RE::NiAVObject*, uint32_t>> parts;
		auto addBiped = [&](const RE::BSTSmartPointer<RE::BipedAnim>& a_biped) {
			if (!a_biped)
				return;
			for (uint32_t i = 0; i < RE::BIPED_OBJECTS::kTotal; ++i)
				if (const RE::NiAVObject* part = a_biped->objects[i].partClone.get())
					parts.emplace_back(part, i);
		};
		addBiped(a_actor->GetActorRuntimeData().biped);
		if (a_actor == player)
			addBiped(player->GetInfoRuntimeData().largeBiped);
		auto slotOf = [&](const RE::NiAVObject* a_object) -> int {
			for (const RE::NiAVObject* n = a_object; n; n = n->parent)
				for (const auto& [part, slot] : parts)
					if (part == n)
						return int(slot);
			return -1;
		};
		auto carried = [](int a_slot) { return a_slot >= int(RE::BIPED_OBJECTS::kEditorTotal) || a_slot == int(RE::BIPED_OBJECTS::kShield); };

		struct Skin
		{
			RE::BSGeometry* geometry;
			std::vector<SkinBone> bones;
			RE::NiTransform rootParentToSkin;
			bool dynamic;
			int feature;
		};
		std::vector<Skin> skins;
		std::vector<std::string> gearLines;
		auto visit = [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
			auto& runtime = a_geometry->GetGeometryRuntimeData();
			auto* property = runtime.shaderProperty.get();
			if (!IsLit(property) || HiddenByGame(a_geometry, root, firstPersonPlayer) || IsProxyOrOverlayGeometry(a_geometry, property))
				return RE::BSVisit::BSVisitControl::kContinue;
			if (a_geometry->GetUserData() == a_actor)
				c.owned++;
			else
				c.unowned++;
			if (IsEngineDecalName(NameOf(a_geometry))) {
				c.decals++;
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			auto* triShape = a_geometry->AsTriShape();
			const uint32_t triangles = triShape ? uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) : 0u;
			auto* skin = runtime.skinInstance.get();
			auto* data = skin ? skin->skinData.get() : nullptr;
			if (skin && (!data || !skin->bones))
				return RE::BSVisit::BSVisitControl::kContinue;
			const int slot = slotOf(a_geometry);
			if (!skin || carried(slot)) {
				const char* kind = "attached, not equipped";
				if (carried(slot)) {
					(skin ? c.gearSkinned : c.gear)++;
					c.gearTriangles += triangles;
					c.gearRadiusMax = std::max(c.gearRadiusMax, a_geometry->worldBound.radius);
					kind = skin ? "skinned gear" : "gear";
				} else if (slot >= 0) {
					c.rigidWorn++;
					kind = "rigid worn";
				} else {
					c.attached++;
				}
				if (a_dump)
					gearLines.push_back(std::format("  {} '{}' under '{}', slot {}: radius {:.0f}, {} triangles{}, owner {}", kind, NameOf(a_geometry), NameOf(a_geometry->parent), slot,
						a_geometry->worldBound.radius, triangles, skin ? std::format(", {} bones of its own", data->GetBoneCount()) : std::string(),
						a_geometry->GetUserData() == a_actor ? "this actor" : "NOT this actor"));
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			const auto feature = FeatureOf(property);
			skins.push_back({ a_geometry, BonesOf(skin, data), data->rootParentToSkin, a_geometry->AsDynamicTriShape() != nullptr, int(feature) });
			c.skinned++;
			c.triangles += triangles;
			c.dynamic += skins.back().dynamic ? 1u : 0u;
			if (feature == RE::BSShaderMaterial::Feature::kFaceGen || feature == RE::BSShaderMaterial::Feature::kFaceGenRGBTint)
				c.skinShapes++;
			if (feature == RE::BSShaderMaterial::Feature::kEye)
				c.eyeShapes++;
			auto* partition = skin->skinPartition.get();
			const RE::BSGraphics::TriShape* buffer = partition && partition->numPartitions > 0 && partition->partitions.data() ? partition->partitions[0].buffData : nullptr;
			if (buffer && !buffer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
				c.noNormals++;
			c.rawOffsetMax = std::max(c.rawOffsetMax, data->rootParentToSkin.translate.Length());
			return RE::BSVisit::BSVisitControl::kContinue;
		};
		RE::BSVisit::TraverseScenegraphGeometries(root, std::ref(visit));

		// The anchor: the root-most bone any body skin names, so the frame is
		// the skeleton's own and does not hang on which mesh came first.
		auto depthOf = [&](const RE::NiAVObject* a_node) {
			uint32_t depth = 0;
			for (const RE::NiAVObject* n = a_node; n && n != root; n = n->parent)
				depth++;
			return depth;
		};
		c.anchorDepth = UINT32_MAX;
		for (const auto& s : skins)
			for (const auto& bone : s.bones) {
				const uint32_t depth = depthOf(bone.node);
				if (depth < c.anchorDepth || (depth == c.anchorDepth && bone.name < c.anchor)) {
					c.anchorDepth = depth;
					c.anchor = bone.name;
				}
			}
		if (skins.empty())
			c.anchorDepth = 0;

		// Bind poses chained outward from the anchor, the biggest skins first:
		// each skin that reaches the chain adds the bones it alone names.
		BindMap bind;
		if (!c.anchor.empty())
			bind.emplace(c.anchor, RE::NiTransform());
		std::sort(skins.begin(), skins.end(), [](const Skin& a_a, const Skin& a_b) { return a_a.bones.size() > a_b.bones.size(); });
		std::vector<SkinMap> maps(skins.size());
		for (bool progress = true; progress;) {
			progress = false;
			for (size_t i = 0; i < skins.size(); ++i) {
				if (maps[i].chained)
					continue;
				SkinMap map = MapSkin(skins[i].bones, bind, kBodyCoatVoxel);
				if (!map.chained)
					continue;
				progress = true;
				for (const auto& bone : skins[i].bones) {
					if (!bind.contains(bone.name))
						bind.emplace(bone.name, map.toAnchor * bone.boneToSkin);
					const RE::NiPoint3 centre = map.toAnchor * (bone.boneToSkin * bone.bound.center);
					const float radius = bone.bound.radius * bone.boneToSkin.scale * map.toAnchor.scale;
					c.boundsMin = { std::min(c.boundsMin.x, centre.x - radius), std::min(c.boundsMin.y, centre.y - radius), std::min(c.boundsMin.z, centre.z - radius) };
					c.boundsMax = { std::max(c.boundsMax.x, centre.x + radius), std::max(c.boundsMax.y, centre.y + radius), std::max(c.boundsMax.z, centre.z + radius) };
				}
				maps[i] = std::move(map);
			}
		}
		c.bindBones = uint32_t(bind.size());
		std::vector<std::string> skinLines;
		for (size_t i = 0; i < skins.size(); ++i) {
			const auto& s = skins[i];
			const auto& map = maps[i];
			const auto& rps = s.rootParentToSkin.translate;
			if (!map.chained) {
				c.unchained++;
				if (a_dump)
					skinLines.push_back(std::format("  skin '{}': {} bones, NOT CHAINED (no bone reaches the bind); rootParentToSkin ({:.1f}, {:.1f}, {:.1f}); feature {}{}",
						NameOf(s.geometry), s.bones.size(), rps.x, rps.y, rps.z, s.feature, s.dynamic ? ", dynamic" : ""));
				continue;
			}
			c.chained++;
			c.outlierBones += uint32_t(map.outliers.size());
			if (map.residual > c.worstResidual) {
				c.worstResidual = map.residual;
				c.worstShape = NameOf(s.geometry);
			}
			if (a_dump)
				skinLines.push_back(std::format("  skin '{}': {} bones, via '{}' ({} of {} shared agree), residual {:.2f}{}; rootParentToSkin ({:.1f}, {:.1f}, {:.1f}); feature {}{}",
					NameOf(s.geometry), s.bones.size(), map.via, map.agree, map.shared, map.residual,
					map.outliers.empty() ? "" : std::format(", outliers {}", OutlierText(map)), rps.x, rps.y, rps.z, s.feature, s.dynamic ? ", dynamic" : ""));
		}

		// First person: the same bones on the first-person skeleton, mapped by
		// name through the third-person bind.
		std::vector<std::string> firstLines;
		if (a_actor == player) {
			auto* first = player->Get3D(true);
			if (first && first != root) {
				auto visitFirst = [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
					auto& runtime = a_geometry->GetGeometryRuntimeData();
					auto* property = runtime.shaderProperty.get();
					if (!IsLit(property) || IsProxyOrOverlayGeometry(a_geometry, property) || IsEngineDecalName(NameOf(a_geometry)))
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
					const SkinMap map = MapSkin(BonesOf(skin, data), bind, kBodyCoatVoxel);
					if (map.chained) {
						c.firstChained++;
						c.firstResidual = std::max(c.firstResidual, map.residual);
					}
					if (a_dump)
						firstLines.push_back(std::format("  first-person skin '{}': {}, owner {}", NameOf(a_geometry),
							map.chained ? std::format("via '{}' ({} of {} shared agree), residual {:.2f}{}", map.via, map.agree, map.shared, map.residual,
											  map.outliers.empty() ? "" : std::format(", outliers {}", OutlierText(map))) :
										  std::string("NOT CHAINED"),
							a_geometry->GetUserData() == a_actor ? "the player" : "NOT the player"));
					return RE::BSVisit::BSVisitControl::kContinue;
				};
				RE::BSVisit::TraverseScenegraphGeometries(first, std::ref(visitFirst));
			}
		}

		if (a_dump) {
			dumpLine(std::format("{:08X} '{}' {:.1f} m{}{}: anchor '{}' (depth {}, {} bind bones); {} skins, {} chained, {} not, worst residual {:.2f}{}, {} outlier bones; {} decals left out; gear {} (+{} skinned), rigid worn {}, attached {}; {} lit geometries owned / {} not",
				c.formID, c.name, c.distance / kUnitsPerMeter, c.corpse ? " corpse" : "", c.weaponDrawn ? " weapon drawn" : "", c.anchor, c.anchorDepth, c.bindBones,
				c.skinned, c.chained, c.unchained, c.worstResidual, c.worstShape.empty() ? "" : std::format(" ('{}')", c.worstShape), c.outlierBones,
				c.decals, c.gear, c.gearSkinned, c.rigidWorn, c.attached, c.owned, c.unowned));
			for (const auto& line : skinLines)
				dumpLine(line);
			for (const auto& line : firstLines)
				dumpLine(line);
			for (const auto& line : gearLines)
				dumpLine(line);
		}
		return c;
	};

	if (a_dump)
		logger::info("[SNOW DEFORMATION] body coat census (frames through the bones): {} living, {} corpses inside the contact window; each actor's line is followed by its shapes", living.size(), dead.size());
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
		uint32_t skins = 0, chained = 0, unchained = 0, outliers = 0, decals = 0, noNormals = 0, dynamic = 0, gear = 0, gearSkinned = 0, rigidWorn = 0, attached = 0, owned = 0, unowned = 0;
		uint64_t triangles = 0;
		float worst = 0.0f, rawOffset = 0.0f;
		for (const auto& c : bodyCoatCensus) {
			skins += c.skinned;
			chained += c.chained;
			unchained += c.unchained;
			outliers += c.outlierBones;
			decals += c.decals;
			noNormals += c.noNormals;
			dynamic += c.dynamic;
			gear += c.gear;
			gearSkinned += c.gearSkinned;
			rigidWorn += c.rigidWorn;
			attached += c.attached;
			owned += c.owned;
			unowned += c.unowned;
			triangles += c.triangles + c.gearTriangles;
			worst = std::max(worst, c.worstResidual);
			rawOffset = std::max(rawOffset, c.rawOffsetMax);
		}
		lines.push_back(std::format("Census {:.0f} frames ago, {:.2f} ms: {} actors, {} skins, {} triangles in all; {} engine decals left out.",
			double(globals::state->frameCount - bodyCoatCensusFrame), bodyCoatCensusMs, bodyCoatCensus.size(), skins, triangles, decals));
		lines.push_back(std::format("Frames through the bones: {} skins chained to their actor's anchor, {} not; worst residual {:.2f} (a voxel is {:.1f}); {} outlier bones dropped by the vote. Raw skin frames sit up to {:.1f} units apart.",
			chained, unchained, worst, kBodyCoatVoxel, outliers, rawOffset));
		lines.push_back(std::format("Gear: {} rigid and {} skinned under weapon, shield or quiver slots; {} rigid worn pieces; {} attached to no equipped slot (stuck arrows). Skins without vertex normals {}, dynamic {}. Owners: {} lit geometries name their actor, {} do not.",
			gear, gearSkinned, rigidWorn, attached, noNormals, dynamic, owned, unowned));
		for (const auto& c : bodyCoatCensus) {
			const RE::NiPoint3 size = c.boundsMax - c.boundsMin;
			const bool sized = c.chained > 0 && size.x > 0.0f;
			lines.push_back(std::format("{} ({:.0f} m{}{}): anchor '{}' (depth {}), {} skins, {} chained / {} not, worst residual {:.2f}{}, {} outliers; frame {}; {} triangles; gear {} (+{} skinned, radius up to {:.0f}), worn {}, attached {}, decals {}; owners {}/{}",
				c.name.empty() ? std::format("{:08X}", c.formID) : c.name, c.distance / kUnitsPerMeter, c.corpse ? ", corpse" : "", c.weaponDrawn ? ", weapon drawn" : "",
				c.anchor, c.anchorDepth, c.skinned, c.chained, c.unchained, c.worstResidual, c.worstShape.empty() ? "" : std::format(" ('{}')", c.worstShape), c.outlierBones,
				sized ? std::format("{:.0f} x {:.0f} x {:.0f} in the anchor's axes", size.x, size.y, size.z) : std::string("(none chained)"),
				c.triangles, c.gear, c.gearSkinned, c.gearRadiusMax, c.rigidWorn, c.attached, c.decals, c.owned, c.owned + c.unowned));
			if (c.firstSkinned > 0 || c.firstOwned + c.firstUnowned > 0)
				lines.push_back(std::format("  first person: {} skins, {} chained to the third-person bind, worst residual {:.2f}; owners {}/{}", c.firstSkinned, c.firstChained,
					c.firstResidual, c.firstOwned, c.firstOwned + c.firstUnowned));
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
