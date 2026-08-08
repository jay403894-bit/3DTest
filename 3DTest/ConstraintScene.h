#pragma once
// ConstraintScene -- the joints test bed. Like CharacterScene it is a COURSE rather than a game: each
// piece isolates one constraint behaviour so a regression reads as "the door stopped swinging" instead
// of a vague feel change.
//
//   swing door      -> hinge with limits; push it with the character, it opens one way and stops
//   double doors    -> two hinges, mirrored limits, meeting in the middle
//   powered hatch   -> hinge with a MOTOR; press E to drive it open/closed
//   rope            -> a chain of PointConstraints with a heavy weight on the end; walk into it
//
// CharacterScene (Sponza) is deliberately left intact and still selectable from main.cpp -- this scene
// is additive, not a replacement, because Sponza is still the only real-geometry test we have.
#include <vector>
#include <memory>
#include <DirectXMath.h>
#include <Renderer2D.h>
#include <Renderer3D.h>
#include <Primitives3D.h>
#include <InputManager.h>
#include <Font.h>
#include "Physics3D.h"
#include "Scene.h"

class ConstraintScene : public Scene
{
public:
	ConstraintScene(JLib::Font* font, JLib::Renderer2D& r2d, JLib::ResourceManager& rm, JLib::Renderer3D& r3d,
	                std::shared_ptr<JLib::InputManager> input, unsigned int width, unsigned int height);

	void Update(bool& isRunning, float dt) override;
	void Draw() override;
	void HandleInput(float dt) override;

private:
	void BuildCourse();

	JLib::Font* font;
	JLib::Renderer2D& r2d;
	JLib::ResourceManager& rm;
	JLib::Renderer3D& r3d;
	std::shared_ptr<JLib::InputManager> input;
	float screenW, screenH;

	JLib::Mesh boxMesh, groundMesh, frameMesh, doorMesh, hatchMesh, ropeMesh, weightMesh, capsuleMesh;
	// Emissive lamps. Bloom has nothing to find in a scene made entirely of diffuse surfaces lit by
	// ambient -- the brightest thing is a lit wall, which is exactly what should NOT glow. A real
	// emitter is the missing ingredient, and emissiveFactor is added AFTER lighting and unbounded,
	// so it lands in the FP16 target well above 1.0 where the bloom threshold can see it.
	JLib::Mesh lampMesh;
	JLib::TextureHandle woodTex;

	JLib::Physics3D physics3d;
	JLib::Physics3D::CharacterHandle player = JLib::Physics3D::kInvalidCharacter;

	// Anything drawn from a body's live transform. Constrained bodies MOVE, so unlike CharacterScene's
	// static course these can't be baked into a fixed matrix at build time -- each is re-read from
	// physics every frame, which is also the point of the scene.
	struct Piece {
		JLib::Physics3D::BodyHandle handle;
		DirectX::XMFLOAT3 halfExtents;
		const JLib::Mesh* mesh = nullptr;
	};
	std::vector<Piece> dynamicPieces;   // doors, hatch, rope links, weight
	std::vector<Piece> staticPieces;    // ground, frames, anchors

	// The motorised hatch. Held so E can toggle it; the angle readout comes back through
	// GetHingeAngle, which is also the sanity check that the motor is actually driving it.
	JLib::Physics3D::ConstraintHandle hatchHinge = JLib::Physics3D::kInvalidConstraint;
	bool hatchOpen = false;
	bool ePrev = false;
	// Diagnostics: how many constraints actually got created vs. how many we asked for. A silent
	// kInvalidConstraint return and a constraint that exists but doesn't move look identical on screen,
	// and that ambiguity is what makes this class of bug slow to find.
	int hingesAsked = 0, hingesMade = 0, pointsAsked = 0, pointsMade = 0;

	float camYaw = 0.0f;
	DirectX::XMFLOAT3 camLookAt = { 0.0f, 1.0f, 0.0f };
	static constexpr float kCamDistance = 8.0f;
	static constexpr float kCamHeight   = 3.5f;
	static constexpr float kCamEase     = 8.0f;

	static constexpr float kMoveSpeed  = 5.0f;
	static constexpr float kJumpSpeed  = 6.5f;
	static constexpr float kAirControl = 0.35f;
	static constexpr float kMouseSens  = 0.0035f;

	static constexpr float kCharRadius  = 0.3f;
	static constexpr float kCharCylHalf = 0.6f;
	static constexpr float kMeshRadius  = 0.3f;    // MakeCapsuleMesh's own dimensions
	static constexpr float kMeshCylHalf = 0.35f;
	static constexpr DirectX::XMFLOAT3 kSpawnPos = { 0.0f, 2.0f, -6.0f };

	// Rope tuning. Segment count trades stretch against cost: fewer, heavier links are stiffer but
	// read as a chain of bars; more links drape better and stretch more under load. 12 is the point
	// where it reads as rope without needing extra solver iterations.
	static constexpr int   kRopeLinks     = 12;
	static constexpr float kRopeLinkHalf  = 0.16f;   // capsule cylinder half-height
	static constexpr float kRopeRadius    = 0.05f;
	// Emissive lamps (the bloom source). Deliberately SMALL: a large emitter produces a big soft
	// wash that is hard to tell from over-exposure, whereas a small intense one gives the tight core
	// plus wide halo that makes bloom legible as an effect. Spread so one is usually in frame.
	static constexpr float kLampRadius = 0.18f;
	static constexpr DirectX::XMFLOAT3 kLampPos[3] = {
		{ -3.0f, 1.8f,  1.5f },
		{  3.2f, 2.1f, -1.0f },
		{  0.0f, 1.6f,  6.0f },
	};
	bool quitRequested = false;
	// Smoothed frame + solver times for the HUD (see Update). EMA'd because a raw per-frame reading
	// at ~1ms jitters far too much to read off the screen.
	double frameMs   = 0.0;
	double physicsMs = 0.0;
};
