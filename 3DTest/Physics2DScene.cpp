#include "pch.h"
#include "Physics2DScene.h"
#include <Helpers.h>
#include <Colors.h>
#include <cmath>
#include <cstdio>

using namespace DirectX;

// Screen pixels (Y DOWN, origin top-left) -> physics meters (Y UP). Everything in BuildScene is
// authored in pixels and goes through here, so the scene reads in the same units the renderer uses.
XMFLOAT3 Physics2DScene::ToMeters(XMFLOAT2 px) const {
	return { px.x / kPixelsPerMeter, -px.y / kPixelsPerMeter, 0.0f };
}

Physics2DScene::Physics2DScene(JLib::Font* font_, JLib::Renderer2D& r2d_,
                               std::shared_ptr<JLib::InputManager> input_,
                               unsigned int width, unsigned int height)
	: font(font_), r2d(r2d_), input(std::move(input_)),
	  screenW((float)width), screenH((float)height)
{
	physics.Init();
	physics.SetPixelsPerMeter(kPixelsPerMeter);
	physics.SetGravity2D(kGravityPx);   // px/s^2, positive = down the screen
	BuildScene();
	physics.Finalize();
}

void Physics2DScene::BuildScene() {
	auto addStaticPx = [&](XMFLOAT2 centrePx, XMFLOAT2 halfPx, XMFLOAT4 col) {
		auto h = physics.AddStaticBox(ToMeters(centrePx), { ToMeters(halfPx.x), ToMeters(halfPx.y), 0.25f });
		props.push_back({ h, Shape::Box, halfPx, col });
		return h;
	};
	// Plane2D is applied to every dynamic body: without it they are full 3D bodies that will happily
	// tip out of the screen plane the first time they land on a corner, which looks like the sim
	// "losing" objects. The Z half-extent above/below is arbitrary (0.25m) -- it only affects the
	// collision volume in a direction nothing can move along.
	auto addDynamicPx = [&](XMFLOAT2 centrePx, XMFLOAT2 halfPx, float massKg, XMFLOAT4 col) {
		auto h = physics.AddDynamicBox(ToMeters(centrePx),
		                               { ToMeters(halfPx.x), ToMeters(halfPx.y), 0.25f }, massKg);
		physics.SetBodyPlane2D(h);
		props.push_back({ h, Shape::Box, halfPx, col });
		return h;
	};
	auto addCirclePx = [&](XMFLOAT2 centrePx, float radiusPx, float massKg, XMFLOAT4 col) {
		auto h = physics.AddDynamicSphere(ToMeters(centrePx), ToMeters(radiusPx), massKg);
		physics.SetBodyPlane2D(h);
		props.push_back({ h, Shape::Circle, { radiusPx, radiusPx }, col });
		return h;
	};

	const float floorY = screenH - 60.0f;
	addStaticPx({ screenW * 0.5f, floorY }, { screenW * 0.5f, 20.0f }, { 0.35f, 0.37f, 0.40f, 1.0f });
	addStaticPx({ 20.0f, screenH * 0.5f }, { 20.0f, screenH * 0.5f }, { 0.35f, 0.37f, 0.40f, 1.0f });
	addStaticPx({ screenW - 20.0f, screenH * 0.5f }, { 20.0f, screenH * 0.5f }, { 0.35f, 0.37f, 0.40f, 1.0f });

	// ---- a stack, to show resting contact and friction behave in 2D ----
	for (int i = 0; i < 5; ++i)
		addDynamicPx({ 260.0f, floorY - 40.0f - i * 62.0f }, { 30.0f, 30.0f }, 8.0f,
		             { 0.85f, 0.55f, 0.25f, 1.0f });

	// ---- see-saw: a plank HINGED to a static pivot. Identical code path to the 3D doors; only the
	//      DOF lock and the units differ. Drop a heavy circle on one end and it tips. ----
	{
		XMFLOAT2 pivotPx = { screenW * 0.5f + 120.0f, floorY - 90.0f };
		auto post  = addStaticPx(pivotPx, { 12.0f, 90.0f }, { 0.30f, 0.32f, 0.35f, 1.0f });
		auto plank = addDynamicPx({ pivotPx.x, pivotPx.y - 96.0f }, { 170.0f, 10.0f }, 12.0f,
		                          { 0.35f, 0.75f, 0.95f, 1.0f });
		// Hinge axis is the SCREEN's Z -- out of the plane. That is the only axis a Plane2D body can
		// rotate about, so a 2D hinge is always {0,0,1}.
		// Back to the default (exempt the pair) now that we know that flag is what breaks it -- the
		// logging in PairFilter will show WHICH pairs are actually being rejected.
		physics.AddHingeConstraint(post, plank,
			ToMeters({ pivotPx.x, pivotPx.y - 96.0f }), { 0.0f, 0.0f, 1.0f }, -35.0f, 35.0f);
		addCirclePx({ pivotPx.x - 150.0f, 120.0f }, 26.0f, 30.0f, { 0.90f, 0.30f, 0.30f, 1.0f });
	}

	// ---- rope: a chain of point constraints, same as the 3D scene ----
	{
		XMFLOAT2 anchorPx = { 200.0f, 90.0f };
		auto anchor = addStaticPx(anchorPx, { 16.0f, 10.0f }, { 0.30f, 0.32f, 0.35f, 1.0f });
		JLib::Physics3D::BodyHandle prev = anchor;
		float y = anchorPx.y + 10.0f;
		const float linkHalf = 14.0f;
		for (int i = 0; i < 10; ++i) {
			auto link = addDynamicPx({ anchorPx.x, y + linkHalf }, { 5.0f, linkHalf }, 1.0f,
			                         { 0.75f, 0.70f, 0.50f, 1.0f });
			physics.AddPointConstraint(prev, link, ToMeters({ anchorPx.x, y }));
			prev = link;
			y += linkHalf * 2.0f;
		}
		auto weight = addDynamicPx({ anchorPx.x, y + 24.0f }, { 24.0f, 24.0f }, 14.0f,
		                           { 0.90f, 0.30f, 0.30f, 1.0f });
		physics.AddPointConstraint(prev, weight, ToMeters({ anchorPx.x, y }));
	}
}

void Physics2DScene::SpawnBox(XMFLOAT2 posPx) {
	const float s = 16.0f + (float)(spawnCount % 4) * 6.0f;
	auto h = physics.AddDynamicBox(ToMeters(posPx), { ToMeters(s), ToMeters(s), 0.25f }, 4.0f);
	physics.SetBodyPlane2D(h);
	props.push_back({ h, Shape::Box, { s, s },
	                  { 0.45f + 0.1f * (spawnCount % 3), 0.75f, 0.55f, 1.0f } });
	++spawnCount;
}

void Physics2DScene::HandleInput(float dt) {
	if (input->IsKeyPressed(VK_ESCAPE)) quitRequested = true;
	if (input->IsKeyPressed(VK_SPACE))  SpawnBox({ screenW * 0.5f, 80.0f });
	if (input->IsMouseButtonPressed(JLib::InputManager::MouseLeftButton))
		SpawnBox(input->GetMousePos());
}

void Physics2DScene::Update(bool& isRunning, float dt) {
	if (quitRequested) { isRunning = false; return; }

	// Time the solver step on its own. FPS alone cannot answer "is the physics expensive?" -- at
	// 900 FPS the whole frame is ~1.1ms, and the interesting question is whether the solver is 0.1ms
	// of that or 0.9ms. Those are completely different situations and the frame rate looks identical.
	LARGE_INTEGER t0, t1, f;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);
	physics.Update(dt);
	QueryPerformanceCounter(&t1);
	// Exponential moving average, not the raw sample: a per-frame number at this timescale jitters
	// far too much to read off the screen. 0.05 settles in about a second and still tracks a change.
	const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
	physicsMs = (physicsMs == 0.0) ? ms : (physicsMs * 0.95 + ms * 0.05);
	frameMs   = (frameMs   == 0.0) ? (double)dt * 1000.0
	                               : (frameMs * 0.95 + (double)dt * 1000.0 * 0.05);
}

void Physics2DScene::Draw() {
	// Every prop is drawn from its LIVE 2D pose. GetBody2D hands back pixels and radians with the Y
	// flip and rotation sign already handled, so nothing here converts anything.
	for (const Prop& p : props) {
		XMFLOAT2 pos; float rot;
		physics.GetBody2D(p.handle, pos, rot);
		if (p.shape == Shape::Box) {
			// DrawRectangle takes the TOP-LEFT (raylib convention) but rotates about the CENTRE, which
			// is what a rigid body needs -- so the only adjustment is the corner offset.
			r2d.DrawRectangle({ pos.x - p.halfPx.x, pos.y - p.halfPx.y },
			                  { p.halfPx.x * 2.0f, p.halfPx.y * 2.0f },
			                  p.color, XMConvertToDegrees(rot));
		} else {
			r2d.DrawCircle(pos, p.halfPx.x, p.color);
			// A circle with no marking looks identical however it spins, so draw a spoke -- otherwise
			// there is no way to see whether rotation is actually being simulated.
			r2d.DrawLine(pos, { pos.x + std::cos(rot) * p.halfPx.x, pos.y + std::sin(rot) * p.halfPx.x },
			             JLib::Colors::Black, 2.0f);
		}
	}

	char hud[192];
	sprintf_s(hud, "JOLT IN 2D (Plane2D)  bodies %zu   SPACE / CLICK spawn   ESC quit", props.size());
	r2d.SubmitText(*font, 10.0f, 10.0f, hud, 1.0f, JLib::Colors::OffWhite);
	r2d.SubmitText(*font, 10.0f, 34.0f,
	               "same solver, constraints and shapes as the 3D scenes -- only the DOFs are locked",
	               1.0f, JLib::Colors::Gray);

	// MILLISECONDS, with the physics share broken out -- the two numbers that FPS cannot give you.
	// `rest` is everything that is not the solver: rendering, input, submission. When rest dominates,
	// adding bodies is nearly free and the body count is not what you are measuring.
	// `awake` is the count Jolt is actually SOLVING. Watching it fall as a pile settles -- and the
	// physics time fall with it -- is what separates "more contacts cost more" (correct) from
	// "bodies never sleep" (a bug that looks identical on a frame-time readout alone).
	char perf[224];
	const double rest = frameMs - physicsMs;
	sprintf_s(perf, "frame %.2f ms (%.0f fps)   physics %.2f ms (%.0f%%)   rest %.2f ms   awake %zu/%zu",
	          frameMs, frameMs > 0.0 ? 1000.0 / frameMs : 0.0,
	          physicsMs, frameMs > 0.0 ? (physicsMs / frameMs) * 100.0 : 0.0,
	          rest > 0.0 ? rest : 0.0,
	          physics.ActiveBodyCount(), physics.BodyCount());
	r2d.SubmitText(*font, 10.0f, 58.0f, perf, 1.0f, JLib::Colors::Yellow);
}

