#pragma once
#include <vector>
#include <memory>
#include <DirectXMath.h>
#include <Renderer2D.h>
#include <Renderer3D.h>
#include <Primitives3D.h>
#include <InputManager.h>
#include <Font.h>
#include "Scene.h"

// CORNELL BOX -- the reference scene for validating global illumination.
//
// WHY THIS EXISTS AND WHY IT LOOKS LIKE THIS. Every other scene here is wood under warm lights, so
// bounce light and lamp light are the same colour and cannot be told apart by eye. That is exactly
// the ambiguity a Cornell box removes: neutral WHITE floor, ceiling and back wall, one RED wall, one
// GREEN wall. Any pink on the white floor beside the red wall can only have arrived by bouncing off
// it. There is no other explanation available, which is what makes it a test rather than a picture.
//
// It doubles as a permanent regression scene: any future change to tonemapping, probes, shadows or
// the GI gather can be checked against it in seconds, and against reference images that a great many
// renderers have matched.
//
// TWO DELIBERATE DEVIATIONS from the canonical setup, both documented so neither reads as a bug:
//   1. The ceiling light is a RECT AREA LIGHT plus a co-located emissive quad. The original uses an
//      area source; ours is analytic for diffuse but casts NO SHADOW (the shadow rig handles
//      directional/spot/point only), so a spot is added from the same spot to supply the shadows.
//   2. Surfaces are flat colours with roughness 1.0 -- no textures. Texture detail would compete
//      with the thing being measured.
class CornellScene : public Scene
{
public:
    CornellScene(JLib::Font* font, JLib::Renderer2D& r2d, JLib::ResourceManager& rm,
                 JLib::Renderer3D& r3d, std::shared_ptr<JLib::InputManager> input,
                 unsigned int width, unsigned int height);

    void HandleInput(float dt) override;
    void Update(bool& isRunning, float dt) override;
    void Draw() override;

private:
    JLib::Font*            font;
    JLib::Renderer2D&      r2d;
    JLib::ResourceManager& rm;
    JLib::Renderer3D&      r3d;
    std::shared_ptr<JLib::InputManager> input;
    float screenW, screenH;
    bool  quitRequested = false;

    JLib::Mesh boxMesh;
    // One tinted copy per surface. Copying a Mesh copies buffer VIEWS, not buffers, so all of these
    // draw the same cube and differ only in baseColorFactor.
    JLib::Mesh whiteMesh, redMesh, greenMesh, lightMesh, tallMesh, shortMesh;

    // The box is 2 units on a side, centred on the origin at floor level -- close to the canonical
    // proportions, and small enough that the SSGI gather radius covers wall-to-wall bounce.
    static constexpr float kHalf      = 1.0f;   // half the room's width/depth
    static constexpr float kHeight    = 2.0f;   // floor to ceiling
    static constexpr float kWallT     = 0.05f;  // wall thickness (a thin box, not a plane)
    static constexpr float kLightHalfW = 0.32f; // the ceiling panel's half-extents
    static constexpr float kLightHalfD = 0.26f;

    // Orbiting camera: the box is a static subject, so there is nothing to drive but the view.
    float camYaw   = 0.0f;
    float camPitch = 0.18f;
    float camDist  = 3.6f;

    double frameMs = 0.0;
};
