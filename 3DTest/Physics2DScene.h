#pragma once
// Physics2DScene -- Jolt running as a 2D engine, drawn entirely through Renderer2D.
//
// The point of this scene is to prove one claim: that JLib does not need a second physics library for
// 2D. Every body here is a normal Jolt rigid body with Plane2D degrees of freedom (translate X/Y,
// rotate Z), using the same solver, the same constraints and the same queries as the 3D scenes. What
// makes it "2D" is the DOF lock plus the units bridge in Physics3D (pixels <-> meters, Y-down <-> Y-up),
// not a different engine.
//
// Contents, each isolating one thing:
//   falling boxes / circles -> basic dynamics + shapes under a 2D gravity in px/s^2
//   see-saw plank           -> a HINGE constraint in 2D (the 3D door code path, unchanged)
//   rope                    -> POINT constraints in 2D (the 3D rope code path, unchanged)
//   SPACE                   -> spawn more bodies, to watch it under load
#include <vector>
#include <DirectXMath.h>
#include <Renderer2D.h>
#include <InputManager.h>
#include <Font.h>
#include "Physics3D.h"
#include "Scene.h"

class Physics2DScene : public Scene
{
public:
	Physics2DScene(JLib::Font* font, JLib::Renderer2D& r2d, std::shared_ptr<JLib::InputManager> input,
	               unsigned int width, unsigned int height);

	void Update(bool& isRunning, float dt) override;
	void Draw() override;
	void HandleInput(float dt) override;

private:
	// Everything below authors in SCREEN PIXELS (Y down) and converts on the way in, because that is
	// what the renderer and the rest of JLib's 2D code already speak. Physics3D owns the conversion.
	DirectX::XMFLOAT3 ToMeters(DirectX::XMFLOAT2 px) const;
	float             ToMeters(float px) const { return px / kPixelsPerMeter; }
	void BuildScene();
	void SpawnBox(DirectX::XMFLOAT2 posPx);

	JLib::Font* font;
	JLib::Renderer2D& r2d;
	std::shared_ptr<JLib::InputManager> input;
	float screenW, screenH;

	JLib::Physics3D physics;

	// Bodies are drawn from their live 2D pose, so each needs its render size and shape kind. Rects
	// carry a half-size in pixels; circles carry a radius.
	enum class Shape : uint8_t { Box, Circle };
	struct Prop {
		JLib::Physics3D::BodyHandle handle;
		Shape shape;
		DirectX::XMFLOAT2 halfPx;      // Box: half width/height. Circle: x = radius.
		DirectX::XMFLOAT4 color;
	};
	std::vector<Prop> props;

	static constexpr float kPixelsPerMeter = 100.0f;   // a 1.8m character would be 180px
	static constexpr float kGravityPx      = 900.0f;   // matches PlatformerPhysics2D's convention
	bool quitRequested = false;
	// Smoothed frame + solver times for the HUD (see Update). EMA'd because a raw per-frame reading
	// at ~1ms jitters too much to read.
	double frameMs   = 0.0;
	double physicsMs = 0.0;
	int  spawnCount = 0;
};

