#include "pch.h"
#include "ConstraintScene.h"
#include <Helpers.h>
#include <Colors.h>
#include <cmath>
#include <cstdio>

using namespace DirectX;

ConstraintScene::ConstraintScene(JLib::Font* font_, JLib::Renderer2D& r2d_, JLib::ResourceManager& rm_,
                                 JLib::Renderer3D& r3d_, std::shared_ptr<JLib::InputManager> input_,
                                 unsigned int width, unsigned int height)
	: font(font_), r2d(r2d_), rm(rm_), r3d(r3d_), input(std::move(input_)),
	  screenW((float)width), screenH((float)height)
{
	woodTex = rm.LoadTextureAsync(JLib::ExeRelative(L"textures\\wood.png"));
	boxMesh = JLib::MakeCubeMesh(rm);
	boxMesh.material.albedo = woodTex; boxMesh.material.roughness = 0.85f;

	auto tinted = [&](XMFLOAT4 c) { JLib::Mesh m = boxMesh; m.material.baseColorFactor = c; return m; };
	groundMesh = tinted({ 0.55f, 0.55f, 0.58f, 1.0f });
	frameMesh  = tinted({ 0.45f, 0.42f, 0.40f, 1.0f });   // grey  -- static, doesn't move
	doorMesh   = tinted({ 0.85f, 0.55f, 0.25f, 1.0f });   // wood  -- hinged, swings
	hatchMesh  = tinted({ 0.35f, 0.75f, 0.95f, 1.0f });   // blue  -- motorised
	weightMesh = tinted({ 0.90f, 0.30f, 0.30f, 1.0f });   // red   -- the rope's load

	ropeMesh = JLib::MakeCapsuleMesh(rm);
	ropeMesh.material.albedo = woodTex; ropeMesh.material.roughness = 0.9f;
	ropeMesh.material.baseColorFactor = { 0.75f, 0.70f, 0.50f, 1.0f };

	capsuleMesh = JLib::MakeCapsuleMesh(rm);
	capsuleMesh.material.albedo = woodTex; capsuleMesh.material.roughness = 0.5f;
	capsuleMesh.material.baseColorFactor = { 0.5f, 1.0f, 0.6f, 1.0f };

	// Emissive lamp. baseColorFactor is nearly BLACK on purpose -- an emitter should not also be a
	// bright diffuse surface, or it double-counts and reads as a white blob rather than something
	// glowing. All the brightness comes from emissiveFactor, which Basic3D_PS adds after lighting
	// with no clamp: values of 8-20 are ordinary for a light source and are precisely the "above
	// 1.0" radiance that only survives to the end of the frame because of the FP16 target.
	lampMesh = JLib::MakeCapsuleMesh(rm);
	lampMesh.material.albedo = woodTex;
	lampMesh.material.baseColorFactor = { 0.02f, 0.02f, 0.02f, 1.0f };
	lampMesh.material.emissiveFactor  = { 14.0f, 7.0f, 2.5f };   // warm, well past white

	r3d.EnableShadows(true);
	r3d.EnableSSAO(true);
	// Tighter radius than Sponza: this course is built from small props, so the gaps worth darkening
	// (a door against its frame, rope links touching) are centiMeters rather than the half-Meter
	// column bases that scene was tuned for.
	r3d.SetSSAOParams(0.3f, 1.3f);
	// Threshold 3.0, not 1.0. These scenes sit around 2-3 linear radiance BEFORE exposure, and the
	// bloom threshold is applied to raw radiance -- so 1.0 would catch ordinary lit walls and read
	// as a haze over the whole frame rather than as light around bright things. 3.0 puts the cut
	// just above the brightest ordinary surface, leaving only the lamps (emissive 14/7/2.5) above it.
	r3d.EnableBloom(true);
	// Intensity 0.25 rather than 0.10 for a FIRST look. A subtle effect you cannot see is
	// indistinguishable from one that is broken, so start obvious, confirm it works with B, then
	// dial it back to taste -- 0.05-0.10 is where it should end up for a shipping look.
	r3d.SetBloomParams(3.0f, 0.25f);
	// FXAA. Enabling it changes the pipeline shape -- the tonemap pass writes an LDR intermediate
	// and FXAA resolves that to the back buffer (see Renderer3D::EnableFXAA).
	r3d.EnableFXAA(true);
	r3d.SetFXAAParams(0.125f);
	r3d.LoadEnvironment(L"textures\\citrus_orchard_puresky_2k.hdr");
	// EXPOSURE COMPENSATES FOR THE LINE ABOVE, and is not a tonemapping workaround. That HDRI carries
	// a sky irradiance around 3.0; the SetHemisphereAmbient it replaces is around 0.45, and the 4.0
	// sun below was hand-picked against THAT. So enabling IBL raises total scene light ~2.6x with no
	// corresponding rescale, and 1/2.6 ~= 0.38 is simply undoing it. Measured by eye at 0.33-0.41.
	// Set in the CONSTRUCTOR, not the per-frame lighting block below -- writing it every frame would
	// fight the [ and ] keys and make the live tuning controls look broken.
	r3d.SetExposure(0.35f);   // matched to ACESFitted (the default curve); see Renderer3D::SetExposure

	physics3d.Init();
	BuildCourse();
	player = physics3d.AddCharacter(kSpawnPos, kCharRadius, kCharCylHalf, 45.0f, 0.3f);
	physics3d.Finalize();
}

void ConstraintScene::BuildCourse() {
	auto addStatic = [&](XMFLOAT3 c, XMFLOAT3 he, const JLib::Mesh* mesh) {
		staticPieces.push_back({ physics3d.AddStaticBox(c, he), he, mesh });
		return staticPieces.back().handle;
	};
	// massKg is passed explicitly for everything here. Jolt's default density is 1000 kg/m^3 (water),
	// which makes a door ~384kg and a hatch ~384kg -- unpushable by a character and needing thousands
	// of N.m to motor. Anything meant to be moved by gameplay needs a real mass, not a derived one.
	auto addDynamic = [&](XMFLOAT3 c, XMFLOAT3 he, const JLib::Mesh* mesh, float massKg) {
		dynamicPieces.push_back({ physics3d.AddDynamicBox(c, he, massKg), he, mesh });
		return dynamicPieces.back().handle;
	};
	// Counted wrappers so the HUD can distinguish "the constraint was never created" from "the
	// constraint exists but nothing moves" -- two very different bugs that look the same on screen.
	auto hinge = [&](JLib::Physics3D::BodyHandle a, JLib::Physics3D::BodyHandle b,
	                 XMFLOAT3 pivot, XMFLOAT3 axis, float lo, float hi) {
		++hingesAsked;
		auto h = physics3d.AddHingeConstraint(a, b, pivot, axis, lo, hi);
		if (h != JLib::Physics3D::kInvalidConstraint) ++hingesMade;
		return h;
	};
	auto point = [&](JLib::Physics3D::BodyHandle a, JLib::Physics3D::BodyHandle b, XMFLOAT3 pivot) {
		++pointsAsked;
		auto h = physics3d.AddPointConstraint(a, b, pivot);
		if (h != JLib::Physics3D::kInvalidConstraint) ++pointsMade;
		return h;
	};

	auto ground = addStatic({ 0.0f, -0.5f, 0.0f }, { 20.0f, 0.5f, 20.0f }, &groundMesh);   // floor

	// The doors below hinge to the GROUND, not to a post, and no post body is created.
	//
	// A hinge does not require the two bodies to be near each other -- only the ANCHOR matters. That
	// matters here because CONSTRAINED BODIES STILL COLLIDE in Jolt: hinging a door at its own edge
	// makes its thickness sweep an arc straight through whatever it is hinged to, so it rotates a few
	// degrees, hits the post, and is pushed back. That is exactly what "moves a tiny bit then pushes
	// back" was. Real engines solve it with a collision group filter that exempts constrained pairs;
	// the wrapper doesn't expose one yet, so this scene sidesteps it by hinging to a body the door can
	// never touch. Doing that also makes the test HONEST: with nothing to collide against, anything the
	// door does is the constraint's doing and nothing else's.
	// (TODO for the wrapper: a "these two bodies never collide" filter -- doors, ragdolls and rope links
	//  all want it, and rope links only look right today because they happen to be thin enough to miss.)

	// ---- 1. single swing door -------------------------------------------------------------------
	// Hinge on the LEFT post. The door's own centre sits half a width to the right of the pivot, so
	// the pivot is at the door's edge -- that offset is the whole difference between a door and a
	// spinning plank, and it comes purely from where the anchor is placed.
	{
		const float w = 1.2f, h = 2.0f, t = 0.08f;
		// A DOOR HANGS -- it does not stand on the floor. This is why the first version appeared frozen:
		// with the door's bottom edge at exactly y=0 it RESTED on the ground, and a ~380kg slab at
		// friction 0.4 needs ~1500N to slide, far past what the player (600N) or a nudge can supply.
		// The hinge was working the whole time; the floor was holding the door. Real doors have a gap
		// underneath for exactly this reason.
		const float gap = 0.06f;
		XMFLOAT3 postPos = { -5.0f, h * 0.5f, 0.0f };
		auto post = addStatic(postPos, { 0.12f, h * 0.5f, 0.12f }, &frameMesh);
		XMFLOAT3 doorPos = { postPos.x + w * 0.5f + 0.12f, h * 0.5f + gap, 0.0f };
		auto door = addDynamic(doorPos, { w * 0.5f, h * 0.5f, t }, &doorMesh, 35.0f);
		physics3d.SetFriction(door, 0.4f);
		// Hinged to its own POST again, which only works now that AddHingeConstraint exempts the pair
		// from collision by default -- previously the door swept its thickness through the post and
		// jammed after a few degrees. Pivot at the post's inner face = the door's edge; that offset
		// between pivot and centre of mass is the whole difference between a door and a spinning plank.
		hinge(post, door,
			{ postPos.x + 0.12f, doorPos.y, 0.0f }, { 0.0f, 1.0f, 0.0f }, -110.0f, 110.0f);
	}

	// ---- 2. double doors ------------------------------------------------------------------------
	// Two hinges with MIRRORED limits meeting in the middle. Push either leaf and only it moves --
	// which is the visible proof that limits are measured per-constraint from its own creation pose,
	// not from some global reference.
	{
		const float w = 1.0f, h = 2.0f, t = 0.08f, gap = 0.06f;   // gap: hangs clear of the floor
		// A visible gap between the two leaves. Without it they touch when shut and shove each other
		// apart on the first frame -- the same collision problem the posts caused, just between the
		// doors themselves. Once the wrapper can exempt pairs from collision this can close up.
		const float leafGap = 0.04f;   // small clearance so the two leaves don't shove each other
		JLib::Physics3D::BodyHandle leaves[2] = {};
		for (int side = 0; side < 2; ++side) {
			const float dir = (side == 0) ? -1.0f : 1.0f;
			XMFLOAT3 postPos = { dir * (w + leafGap * 0.5f + 0.12f), h * 0.5f, 5.0f };
			auto post = addStatic(postPos, { 0.12f, h * 0.5f, 0.12f }, &frameMesh);
			XMFLOAT3 doorPos = { postPos.x - dir * (w * 0.5f + 0.12f), h * 0.5f + gap, 5.0f };
			leaves[side] = addDynamic(doorPos, { w * 0.5f, h * 0.5f, t }, &doorMesh, 35.0f);
			hinge(post, leaves[side],
				{ postPos.x - dir * 0.12f, doorPos.y, 5.0f }, { 0.0f, 1.0f, 0.0f },
				-110.0f, 110.0f);   // symmetric while diagnosing -- see the single door above
		}
		// The two leaves aren't joined by a constraint, so nothing exempts THEM from each other. They
		// meet in the middle when shut, and touching bodies push apart. This is the case
		// SetBodiesCollide exists for independently of any joint.
		physics3d.SetBodiesCollide(leaves[0], leaves[1], false);
	}

	// ---- 3. motorised hatch (press E) -----------------------------------------------------------
	// Hinged along X so it swings UP like a cellar door. The motor is what makes it a mechanism
	// rather than a thing that flops: without it, gravity wins and it just lies shut.
	{
		const float w = 1.6f, d = 1.2f, t = 0.1f;
		XMFLOAT3 sillPos = { 5.0f, 0.15f, 0.0f };
		auto sill = addStatic(sillPos, { w * 0.5f, 0.15f, 0.1f }, &frameMesh);
		XMFLOAT3 hatchPos = { 5.0f, 0.30f + t, d * 0.5f };
		auto hatch = addDynamic(hatchPos, { w * 0.5f, t, d * 0.5f }, &hatchMesh, 45.0f);
		hatchHinge = hinge(sill, hatch,
			{ 5.0f, 0.30f, 0.0f }, { 1.0f, 0.0f, 0.0f }, -95.0f, 0.0f);
		physics3d.SetHingeMotor(hatchHinge, true, 0.0f, 6000.0f);   // hold shut until asked
	}

	// ---- 4. rope ---------------------------------------------------------------------------------
	// A chain of capsules, each PointConstrained to the previous at the seam between them, with the
	// first pinned to a static anchor and a heavy box on the end. The weight is deliberate: an
	// unloaded rope hides stretch, and stretch is the thing you actually want to see when judging
	// whether the segment count and mass ratio are sane.
	{
		const float linkLen = kRopeLinkHalf * 2.0f + kRopeRadius * 2.0f;
		XMFLOAT3 anchorPos = { 0.0f, 5.0f, -3.0f };
		auto anchor = addStatic(anchorPos, { 0.25f, 0.15f, 0.25f }, &frameMesh);

		JLib::Physics3D::BodyHandle prev = anchor;
		float prevBottom = anchorPos.y - 0.15f;
		for (int i = 0; i < kRopeLinks; ++i) {
			XMFLOAT3 c = { anchorPos.x, prevBottom - linkLen * 0.5f, anchorPos.z };
			auto link = physics3d.AddDynamicCapsule(c, kRopeLinkHalf, kRopeRadius);
			dynamicPieces.push_back({ link, { kRopeRadius, kRopeLinkHalf, kRopeRadius }, &ropeMesh });
			// Anchor the joint at the SEAM (top of this link), not either centre -- constrain centres
			// together and the links interpenetrate into a stack instead of hanging in a line.
			point(prev, link, { c.x, prevBottom, c.z });
			prev = link;
			prevBottom -= linkLen;
		}
		// The load. Mass matters more than size here: too heavy relative to the links and the solver
		// visibly stretches the chain, which is the honest limit of an iterative solver rather than a
		// bug to tune away.
		XMFLOAT3 wPos = { anchorPos.x, prevBottom - 0.25f, anchorPos.z };
		auto weight = physics3d.AddDynamicBox(wPos, { 0.25f, 0.25f, 0.25f }, 25.0f);
		dynamicPieces.push_back({ weight, { 0.25f, 0.25f, 0.25f }, &weightMesh });
		point(prev, weight, { wPos.x, prevBottom, wPos.z });
	}

	// ---- 4. emissive lamps: THE BLOOM SOURCE --------------------------------------------------
	// Three glowing orbs at eye height, spread across the course so one is always roughly in frame.
	// Added as STATIC bodies so they also read as physical objects you can walk into rather than
	// floating decals -- and so the existing submitPiece loop draws them with no special case.
	//
	// Each is paired with a real point light in the per-frame rig below. An emitter that does not
	// light its surroundings is the classic giveaway that a "light" is only a bright texture: the
	// glow says light, the unlit floor underneath says otherwise, and the eye catches the
	// contradiction immediately.
	for (int i = 0; i < 3; ++i) {
		const XMFLOAT3 p = kLampPos[i];
		addStatic(p, { kLampRadius, kLampRadius, kLampRadius }, &lampMesh);
	}
}

void ConstraintScene::HandleInput(float dt) {
	if (input->IsKeyPressed(VK_ESCAPE)) quitRequested = true;

	camYaw += input->GetMouseDeltaX() * kMouseSens;

	XMFLOAT3 v = physics3d.GetCharacterVelocity(player);
	const bool grounded = physics3d.GetCharacterGroundState(player) == JLib::Physics3D::GroundState::Grounded;

	float fwd = 0.0f, strafe = 0.0f;
	if (input->IsKeyDown('W')) fwd += 1.0f;
	if (input->IsKeyDown('S')) fwd -= 1.0f;
	if (input->IsKeyDown('D')) strafe += 1.0f;
	if (input->IsKeyDown('A')) strafe -= 1.0f;

	const float sy = sinf(camYaw), cy = cosf(camYaw);
	float wx = strafe * cy + fwd * sy;
	float wz = -strafe * sy + fwd * cy;
	const float len = sqrtf(wx * wx + wz * wz);
	if (len > 1e-4f) { wx /= len; wz /= len; }

	const float authority = grounded ? 1.0f : kAirControl;
	v.x += (wx * kMoveSpeed - v.x) * authority;
	v.z += (wz * kMoveSpeed - v.z) * authority;

	if (grounded && input->IsKeyPressed(VK_SPACE)) v.y = kJumpSpeed;
	physics3d.SetCharacterVelocity(player, v);

	if (input->IsKeyPressed('R')) physics3d.SetCharacterPosition(player, kSpawnPos);

	// --- tonemap A/B, live. Judged by EYE, so rebuild-to-compare is the wrong loop: T cycles the
	// curve, [ / ] sweep exposure, both read out in the HUD.
	//
	// CYCLING THE CURVE CARRIES ITS OWN EXPOSURE, and it has to. The five operators differ by up to
	// ~2.7x in GAIN, so comparing them at one fixed exposure compares BRIGHTNESS and not curve
	// SHAPE -- whichever curve is brighter just looks "more washed out" every single time, which is
	// exactly the wrong conclusion and tells you nothing about the operator. These values are matched
	// so a representative scene value lands at the same display level: ACESFitted 0.35 was tuned by
	// eye against the real scene and the other four are solved back from it. [ / ] still move
	// whichever curve is live, so a matched value is a starting point rather than a cage.
	if (input->IsKeyPressed('T')) {
		using TM = JLib::Renderer3D::Tonemapper;
		//                                        None  Reinhard ACES  Uchimura ACESFitted
		static const float kMatchedExposure[] = { 0.25f, 0.48f, 0.18f, 0.25f,  0.35f };
		const unsigned next = ((unsigned)r3d.GetTonemapper() + 1u)
		                    % (unsigned)_countof(kMatchedExposure);
		r3d.SetTonemapper((TM)next);
		r3d.SetExposure(kMatchedExposure[next]);
	}
	// Multiplicative steps, not additive: exposure is perceptually a stop, so +/-25% per press keeps
	// the control usable across the whole useful range instead of crawling at the bottom.
	// B toggles bloom. "I can't tell if it works" is not a question you can answer by staring at one
	// frame -- an A/B against the same frame is the only reliable way to see a subtle post effect,
	// and rebuilding to compare loses the exact camera angle that made you ask.
	if (input->IsKeyPressed('B')) r3d.EnableBloom(!r3d.IsBloomEnabled());
	if (input->IsKeyPressed('F')) r3d.EnableFXAA(!r3d.IsFXAAEnabled());

	if (input->IsKeyPressed(VK_OEM_4)) r3d.SetExposure(r3d.GetExposure() * 0.8f);   // '['
	if (input->IsKeyPressed(VK_OEM_6)) r3d.SetExposure(r3d.GetExposure() * 1.25f);  // ']'

	// E toggles the hatch motor's TARGET, not the hatch itself -- the motor then drives it there over
	// time against gravity, which is what makes it read as a mechanism instead of a teleport.
	const bool e = input->IsKeyDown('E');
	if (e && !ePrev) {
		hatchOpen = !hatchOpen;
		physics3d.SetHingeMotor(hatchHinge, true, hatchOpen ? -90.0f : 0.0f, 6000.0f);
	}
	ePrev = e;
}

void ConstraintScene::Update(bool& isRunning, float dt) {
	if (quitRequested) { isRunning = false; return; }

	// Solver time on its own -- see the HUD line in Draw() for why FPS alone cannot answer the
	// question this is measuring.
	LARGE_INTEGER t0, t1, f;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);
	physics3d.Update(dt);
	physics3d.UpdateCharacters(dt);
	QueryPerformanceCounter(&t1);
	const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
	physicsMs = (physicsMs == 0.0) ? ms : (physicsMs * 0.95 + ms * 0.05);
	frameMs   = (frameMs   == 0.0) ? (double)dt * 1000.0
	                               : (frameMs * 0.95 + (double)dt * 1000.0 * 0.05);

	XMFLOAT3 p = physics3d.GetCharacterPosition(player);
	if (p.y < -20.0f) physics3d.SetCharacterPosition(player, kSpawnPos);

	float t = dt * kCamEase; if (t > 1.0f) t = 1.0f;
	camLookAt.x += (p.x - camLookAt.x) * t;
	camLookAt.y += ((p.y + 1.0f) - camLookAt.y) * t;
	camLookAt.z += (p.z - camLookAt.z) * t;
}

void ConstraintScene::Draw() {
	auto screenSize = r2d.GetScreenSize();
	float aspect = (screenSize.y > 0.0f) ? screenSize.x / screenSize.y : 1.0f;

	XMVECTOR target = XMVectorSet(camLookAt.x, camLookAt.y, camLookAt.z, 1.0f);
	XMVECTOR eye = XMVectorSet(camLookAt.x - sinf(camYaw) * kCamDistance,
	                           camLookAt.y + kCamHeight,
	                           camLookAt.z - cosf(camYaw) * kCamDistance, 1.0f);
	XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0, 1, 0, 0));
	XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspect, 0.1f, 300.0f);
	XMFLOAT3 camPos; XMStoreFloat3(&camPos, eye);
	r3d.SetCamera(view * proj, camPos);

	// Outdoor course, so hemisphere ambient is doing what it's actually good at here (unlike Sponza,
	// where nearly every surface is vertical and lands on the midpoint). IBL supersedes this when the
	// HDRI loaded.
	r3d.SetHemisphereAmbient({ 0.42f, 0.47f, 0.58f }, { 0.24f, 0.22f, 0.18f });
	r3d.ClearLights();
	r3d.AddDirectionalLight({ -0.35f, -0.86f, 0.36f }, { 1.0f, 0.96f, 0.88f }, 4.0f);
	// One point light per emissive lamp, co-located with its geometry. The colour matches the
	// lamp's emissiveFactor hue so the glow and the light it casts agree; a mismatch there reads as
	// wrong before you can say why. These are added AFTER the directional so index 0 stays the
	// shadow caster -- SetShadowCaster's auto-pick takes the first directional, and reordering here
	// would silently hand shadow casting to a lamp.
	// INTENSITY 50, NOT 6, AND THE NUMBER IS THE ARGUMENT. Attenuation at 2m is
	// f^2/(d^2+1) ~= 0.197, so intensity 6 delivers radiance ~1.2, and the diffuse response is
	// albedo/PI * 1.2 * NdotL ~= 0.17. The IBL ambient in this scene is albedo * ~3 ~= 1.5. A lamp
	// worth 11% of the light already present cannot read as lighting anything -- it looks like a
	// glowing prop sitting on an evenly lit floor, which is exactly how it looked. 50 puts the pool
	// at roughly parity with ambient at 2m and clearly brighter close in.
	// The general lesson: local lights have to be sized against the AMBIENT, and an outdoor HDRI
	// makes that ambient far larger than hand-picked hemisphere values ever did.
	for (int i = 0; i < 3; ++i)
		r3d.AddPointLight(kLampPos[i], { 1.0f, 0.62f, 0.28f }, 50.0f, 7.0f);
	r3d.SetShadowBounds({ 0.0f, 2.0f, 0.0f }, 14.0f);

	auto submitPiece = [&](const Piece& pc) {
		if (!pc.mesh) return;
		XMFLOAT3 pos, half; XMFLOAT4 rot;
		physics3d.GetBody(pc.handle, pos, rot, half);
		XMMATRIX m = XMMatrixScaling(pc.halfExtents.x * 2.0f, pc.halfExtents.y * 2.0f, pc.halfExtents.z * 2.0f)
		           * XMMatrixRotationQuaternion(XMVectorSet(rot.x, rot.y, rot.z, rot.w))
		           * XMMatrixTranslation(pos.x, pos.y, pos.z);
		r3d.Submit(*pc.mesh, m);
	};
	for (const Piece& pc : staticPieces)  submitPiece(pc);
	// Rope links are capsules, not boxes -- scale from MakeCapsuleMesh's own dimensions the same way
	// the character does, or they render as stretched blobs that don't match what physics collides.
	for (const Piece& pc : dynamicPieces) {
		if (pc.mesh == &ropeMesh) {
			XMFLOAT3 pos, half; XMFLOAT4 rot;
			physics3d.GetBody(pc.handle, pos, rot, half);
			XMMATRIX m = XMMatrixScaling(kRopeRadius / kMeshRadius, kRopeLinkHalf / kMeshCylHalf, kRopeRadius / kMeshRadius)
			           * XMMatrixRotationQuaternion(XMVectorSet(rot.x, rot.y, rot.z, rot.w))
			           * XMMatrixTranslation(pos.x, pos.y, pos.z);
			r3d.Submit(ropeMesh, m);
		} else {
			submitPiece(pc);
		}
	}

	XMFLOAT3 p = physics3d.GetCharacterPosition(player);
	r3d.Submit(capsuleMesh, XMMatrixScaling(kCharRadius / kMeshRadius,
	                                        kCharCylHalf / kMeshCylHalf,
	                                        kCharRadius / kMeshRadius)
	                      * XMMatrixTranslation(p.x, p.y, p.z));

	char hud[256];
	sprintf_s(hud, "hinges %d/%d  points %d/%d   hatch %s (%.1f deg)   WASD SPACE E R ESC",
	          hingesMade, hingesAsked, pointsMade, pointsAsked,
	          hatchOpen ? "OPEN" : "SHUT", physics3d.GetHingeAngle(hatchHinge));
	r2d.SubmitText(*font, 10.0f, 10.0f, hud, 1.0f, JLib::Colors::OffWhite);
	r2d.SubmitText(*font, 10.0f, 34.0f, "push the doors  |  walk into the rope  |  E toggles the hatch",
	               1.0f, JLib::Colors::Gray);

	// Tonemap readout. Reinhard @ 1.00 == the pre-FP16 look, so it is the reference to compare against.
	static const char* kToneNames[] = { "None(clip)", "Reinhard", "ACES", "Uchimura", "ACES(fitted)" };
	char tone[160];
	sprintf_s(tone, "tonemap %s   exposure %.2f   bloom %s   fxaa %s   [T curve, [ / ] exp, B bloom, F fxaa]",
	          kToneNames[(unsigned)r3d.GetTonemapper() % (unsigned)_countof(kToneNames)],
	          r3d.GetExposure(), r3d.IsBloomEnabled() ? "ON" : "off", r3d.IsFXAAEnabled() ? "ON" : "off");
	r2d.SubmitText(*font, 10.0f, 58.0f, tone, 1.0f, JLib::Colors::Gray);

	// WHERE THE FRAME WENT, in milliseconds. FPS is nonlinear and flatters the top end -- 900 to 600
	// fps sounds catastrophic and is 0.55ms, while 60 to 30 is 16ms -- so ms against a 16.6ms budget
	// is the number that actually means something. `record` is CPU command-list recording
	// (Renderer3D's own timer); `rest` is everything else, largely GPU wait and present. When rest
	// dominates, the CPU-side work being optimised is not what is limiting the frame.
	char perf[224];
	const double record = r3d.GetLastRecordMs();
	const double rest   = frameMs - physicsMs - record;
	sprintf_s(perf, "frame %.2f ms (%.0f fps)   physics %.2f   record %.2f   rest %.2f   "
	                "draws %zu  instances %zu",
	          frameMs, frameMs > 0.0 ? 1000.0 / frameMs : 0.0,
	          physicsMs, record, rest > 0.0 ? rest : 0.0,
	          r3d.GetLastDrawCallCount(), r3d.GetLastInstanceCount());
	r2d.SubmitText(*font, 10.0f, 82.0f, perf, 0.85f, JLib::Colors::Yellow);
}



