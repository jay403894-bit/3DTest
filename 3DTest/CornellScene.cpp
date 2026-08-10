#include "pch.h"
#include "CornellScene.h"
#include <Helpers.h>
#include <Colors.h>
#include <cmath>
#include <cstdio>

using namespace DirectX;

CornellScene::CornellScene(JLib::Font* font_, JLib::Renderer2D& r2d_, JLib::ResourceManager& rm_,
                           JLib::Renderer3D& r3d_, std::shared_ptr<JLib::InputManager> input_,
                           unsigned int width, unsigned int height)
    : font(font_), r2d(r2d_), rm(rm_), r3d(r3d_), input(std::move(input_)),
      screenW((float)width), screenH((float)height)
{
    boxMesh = JLib::MakeCubeMesh(rm);
    // ROUGHNESS 1.0 AND NO TEXTURES on every surface, deliberately. The whole scene is a measurement
    // of DIFFUSE interreflection, and a specular highlight or a wood grain would compete with the
    // only signal that matters -- whether the white floor picks up colour from the coloured walls.
    boxMesh.material.roughness = 1.0f;
    boxMesh.material.metallic  = 0.0f;

    auto tinted = [&](XMFLOAT4 c) { JLib::Mesh m = boxMesh; m.material.baseColorFactor = c; return m; };
    // Canonical-ish albedos. Not the original's measured spectra -- those are wavelength data, and
    // this renderer is RGB -- but the same intent: bright neutral surfaces against saturated walls.
    whiteMesh = tinted({ 0.73f, 0.73f, 0.73f, 1.0f });
    redMesh   = tinted({ 0.65f, 0.05f, 0.05f, 1.0f });
    greenMesh = tinted({ 0.12f, 0.45f, 0.15f, 1.0f });
    tallMesh  = tinted({ 0.73f, 0.73f, 0.73f, 1.0f });
    shortMesh = tinted({ 0.73f, 0.73f, 0.73f, 1.0f });

    // The visible panel. Emissive, near-black base colour -- an emitter that is ALSO a bright diffuse
    // surface double-counts and reads as a white blob rather than something glowing. The area light
    // below does the actual lighting; this is what you SEE when you look up.
    lightMesh = tinted({ 0.02f, 0.02f, 0.02f, 1.0f });
    // ENERGY-MATCHED to the area light (90 x the same colour). A panel must appear to emit what it
    // actually emits -- picking the visible brightness and the cast brightness independently is a
    // quiet inconsistency that reads as wrong without anyone being able to say why. It clips to pure
    // white through the tonemapper, which is correct: that is exactly how a light source looks.
    lightMesh.material.emissiveFactor = { 90.0f, 79.2f, 64.8f };

    r3d.EnableShadows(true);
    // Radius 0.25 suits a 2-unit room: the contact darkening that matters is where the boxes meet the
    // floor, which is centimetres, not the half-metre scale Sponza was tuned for.
    r3d.EnableSSAO(true);
    r3d.SetSSAOParams(0.25f, 1.2f);

    // SSGI radius 2.0 == the full width of the room, so wall-to-wall bounce is inside the gather.
    // THIS IS THE MEASUREMENT. Toggle it with G and watch the white floor beside each coloured wall.
    r3d.EnableSSGI(true);
    // INTENSITY 1.5 IS NOT AN ARBITRARY FUDGE -- it stands in for the bounces this technique cannot
    // do. SSGI computes ONE bounce; a converged reference sums an infinite series, which for a room
    // of average albedo rho totals bounce1 / (1 - rho). At rho = 0.73 that factor is 3.7, so anything
    // up to about 3.7x is defensible as replacing the missing terms rather than inventing light.
    // 1.5 is deliberately conservative: scaling the single bounce also scales its noise and its
    // screen-space errors by exactly the same amount.
    //
    // maxLuma has to rise with the light. The panel is now 90, so the old 8.0 clamp would have
    // rejected the single brightest surface in the room from contributing any bounce at all.
    // maxLuma 4, NOT 40. I raised it to 40 reasoning "the panel is 90, don't reject the brightest
    // surface in the room" -- which is exactly backwards. The clamp is a VARIANCE control, and the
    // light panel is the single worst offender: a ray that happens to hit it returns 40 while its
    // neighbour returns 0, so with 6 rays the mean swings 0 <-> 6.7 between adjacent pixels. Expected
    // mean radiance here is about 1-3, so one sample was worth ~3x the entire answer. That is the
    // salt-and-pepper static. Clamping low says "gather BOUNCE, not direct light from visible
    // emitters" -- the direct lighting already handles the panel, and it does so without noise.
    r3d.SetSSGIParams(3.0f, 1.5f, 4.0f);
    // 12 steps x 12 rays = 144 depth taps per pixel. RAYS matter more than steps for noise: variance
    // falls as 1/rays, while extra steps only help rays find hits they were stepping past. Trading
    // 16x6 for 12x12 halves the noise at a similar cost. thickness 0.25 suits a 2m room -- large
    // enough that a ray does not tunnel through a 0.6m block, small enough that passing behind one is
    // not mistaken for hitting it.
    r3d.SetSSGIMarch(12, 12, 0.25f);

    // Threshold well above the now much brighter walls, intensity low: the reference has no visible
    // glow around the panel, and bloom is not what this scene is measuring.
    r3d.EnableBloom(true);
    r3d.SetBloomParams(6.0f, 0.05f);
    r3d.EnableFXAA(true);

    // NO LoadEnvironment, and that is the point. An HDRI would flood the box with sky irradiance from
    // every direction, which is precisely the assumption a closed room violates -- and it would swamp
    // the bounce light being measured. Ambient is near-black so that essentially ALL the light in
    // frame arrives from the panel, directly or by bouncing.
    r3d.SetExposure(0.35f);
    r3d.SetSkyEnabled(false);
}

void CornellScene::HandleInput(float dt) {
    if (input->IsKeyPressed(VK_ESCAPE)) quitRequested = true;

    // Orbit. The box is a static subject; the only thing worth moving is the viewpoint.
    camYaw += input->GetMouseDeltaX() * 0.005f;
    camPitch = std::fmax(-1.2f, std::fmin(1.2f, camPitch - input->GetMouseDeltaY() * 0.005f));
    if (input->IsKeyDown('W')) camDist = std::fmax(0.6f, camDist - dt * 2.0f);
    if (input->IsKeyDown('S')) camDist = std::fmin(8.0f, camDist + dt * 2.0f);

    // Same post controls as ConstraintScene, so a finding transfers between scenes without relearning
    // the keys. G is the one that matters here.
    if (input->IsKeyPressed('T')) {
        using TM = JLib::Renderer3D::Tonemapper;
        static const float kMatchedExposure[] = { 0.25f, 0.48f, 0.18f, 0.25f, 0.35f };
        const unsigned next = ((unsigned)r3d.GetTonemapper() + 1u)
                            % (unsigned)_countof(kMatchedExposure);
        r3d.SetTonemapper((TM)next);
        r3d.SetExposure(kMatchedExposure[next]);
    }
    if (input->IsKeyPressed('B')) r3d.EnableBloom(!r3d.IsBloomEnabled());
    if (input->IsKeyPressed('F')) r3d.EnableFXAA(!r3d.IsFXAAEnabled());
    // G cycles THREE states: off -> on -> debug. The debug view is the one that actually answers the
    // question this scene exists to ask, because the bounce is a few percent of a lit surface and is
    // far easier to judge isolated than composited.
    if (input->IsKeyPressed('G')) {
        if (!r3d.IsSSGIEnabled())      { r3d.EnableSSGI(true);  r3d.SetSSGIDebug(false); }
        else if (!r3d.IsSSGIDebug())   { r3d.SetSSGIDebug(true); }
        else                           { r3d.EnableSSGI(false); r3d.SetSSGIDebug(false); }
    }
    if (input->IsKeyPressed(VK_OEM_4)) r3d.SetExposure(r3d.GetExposure() * 0.8f);
    if (input->IsKeyPressed(VK_OEM_6)) r3d.SetExposure(r3d.GetExposure() * 1.25f);
}

void CornellScene::Update(bool& isRunning, float dt) {
    if (quitRequested) { isRunning = false; return; }
    frameMs = (frameMs == 0.0) ? (double)dt * 1000.0 : (frameMs * 0.95 + (double)dt * 1000.0 * 0.05);
}

void CornellScene::Draw() {
    // ---- camera: orbit the centre of the room ----
    const float cy = kHeight * 0.5f;
    XMVECTOR target = XMVectorSet(0.0f, cy, 0.0f, 1.0f);
    // NEGATIVE Z at yaw 0. The room is closed on five sides and open toward -Z, so the camera has to
    // start on that side or it is outside a sealed box looking at the unlit back of the rear wall --
    // which, with the sky disabled and ambient near zero, renders as a completely black frame.
    XMVECTOR eye = XMVectorSet(
        std::sin(camYaw) * std::cos(camPitch) * camDist,
        cy + std::sin(camPitch) * camDist,
        -std::cos(camYaw) * std::cos(camPitch) * camDist, 1.0f);
    XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(55.0f),
                                             screenW / screenH, 0.05f, 100.0f);
    XMFLOAT3 eyePos; XMStoreFloat3(&eyePos, eye);
    r3d.SetCamera(view * proj, eyePos);

    // ---- lighting ----
    // Ambient is almost nothing. In a closed room there IS no sky, so every photon in frame should
    // have come from the panel -- either straight down, or by bouncing off a wall. Leaving a real
    // ambient term in would light the shadowed faces for free and quietly hide whether GI works.
    r3d.SetAmbient({ 0.015f, 0.015f, 0.018f });
    r3d.ClearLights();

    const XMFLOAT3 lightPos = { 0.0f, kHeight - kWallT * 2.0f - 0.005f, 0.0f };
    // The area light: a downward-facing rectangle matching the emissive panel exactly. Diffuse from
    // this is the analytic polygon integral, which is what gives the soft, wide terminator on the
    // walls that a punctual light cannot produce.
    // INTENSITY 90, SOLVED not guessed. The panel is 0.64 x 0.52 at 1.895m, so it subtends about
    // 0.093 sr from the floor directly below. Diffuse out = albedo/PI * intensity * solidAngle, then
    // exposure 0.35 and ACESFitted. Working that through: intensity 25 puts the floor at sRGB 0.37
    // (which is what made the room read as a dark cave), while 90 lands around 0.72 -- matching the
    // published reference, where nothing in frame is near black.
    r3d.AddRectAreaLight(lightPos, { 0.0f, -1.0f, 0.0f }, { 1.0f, 0.88f, 0.72f },
                         90.0f, kLightHalfW, kLightHalfD, 12.0f);
    // A spot from the same place, purely to SUPPLY SHADOWS -- area lights do not cast in this
    // renderer. Low intensity so it contributes little light of its own; it is here for the shadow
    // map, not for illumination. Index 0 would have been the shadow caster anyway, and an area light
    // is deliberately never auto-picked.
    r3d.AddSpotLight(lightPos, { 0.0f, -1.0f, 0.0f }, { 1.0f, 0.88f, 0.72f },
                     3.0f, 12.0f, 45.0f, 70.0f);
    r3d.SetShadowCaster(1);
    r3d.SetShadowBounds({ 0.0f, kHeight * 0.5f, 0.0f }, 2.2f);

    // ---- the room. Each wall is a thin box rather than a plane so it has thickness to bounce off
    // and to occlude with, exactly like the original. ----
    auto slab = [&](const JLib::Mesh& m, XMFLOAT3 centre, XMFLOAT3 halfExtent) {
        r3d.Submit(m, XMMatrixScaling(halfExtent.x * 2.0f, halfExtent.y * 2.0f, halfExtent.z * 2.0f)
                      * XMMatrixTranslation(centre.x, centre.y, centre.z));
    };

    slab(whiteMesh, { 0.0f, -kWallT, 0.0f },              { kHalf, kWallT, kHalf });            // floor
    slab(whiteMesh, { 0.0f, kHeight + kWallT, 0.0f },     { kHalf, kWallT, kHalf });            // ceiling
    slab(whiteMesh, { 0.0f, kHeight * 0.5f, kHalf + kWallT }, { kHalf, kHeight * 0.5f, kWallT });// back
    slab(redMesh,   { -kHalf - kWallT, kHeight * 0.5f, 0.0f }, { kWallT, kHeight * 0.5f, kHalf });// LEFT = red
    slab(greenMesh, {  kHalf + kWallT, kHeight * 0.5f, 0.0f }, { kWallT, kHeight * 0.5f, kHalf });// RIGHT = green

    // The two blocks. Rotations are baked into the scale-free transform below; the canonical scene
    // rotates them slightly, which is what makes the colour bleed land on visibly angled faces.
    XMMATRIX tall = XMMatrixScaling(0.6f, 1.2f, 0.6f)
                  * XMMatrixRotationY(XMConvertToRadians(17.0f))
                  * XMMatrixTranslation(-0.35f, 0.6f, 0.3f);
    r3d.Submit(tallMesh, tall);
    XMMATRIX shortB = XMMatrixScaling(0.6f, 0.6f, 0.6f)
                    * XMMatrixRotationY(XMConvertToRadians(-18.0f))
                    * XMMatrixTranslation(0.4f, 0.3f, -0.3f);
    r3d.Submit(shortMesh, shortB);

    // The visible emissive panel, just below the ceiling and matching the area light's extents.
    slab(lightMesh, { lightPos.x, lightPos.y, lightPos.z }, { kLightHalfW, 0.01f, kLightHalfD });

    // ---- HUD ----
    char l1[192];
    sprintf_s(l1, "CORNELL BOX   ssgi %s   frame %.2f ms (%.0f fps)   MOUSE orbit  W/S zoom  ESC quit",
              r3d.IsSSGIEnabled() ? (r3d.IsSSGIDebug() ? "DEBUG (bounce only)" : "ON") : "off",
              frameMs, frameMs > 0.0 ? 1000.0 / frameMs : 0.0);
    r2d.SubmitText(*font, 10.0f, 10.0f, l1, 1.0f, JLib::Colors::OffWhite);
    r2d.SubmitText(*font, 10.0f, 34.0f,
                   "press G: the white floor beside each wall should pick up RED / GREEN. that tint",
                   0.85f, JLib::Colors::Gray);
    r2d.SubmitText(*font, 10.0f, 54.0f,
                   "can only be bounce light -- nothing else in the scene is that colour.",
                   0.85f, JLib::Colors::Gray);
    static const char* kToneNames[] = { "None(clip)", "Reinhard", "ACES", "Uchimura", "ACES(fitted)" };
    char l2[160];
    sprintf_s(l2, "tonemap %s   exposure %.2f   [T curve, [ / ] exp, B bloom, F fxaa, G ssgi]",
              kToneNames[(unsigned)r3d.GetTonemapper() % (unsigned)_countof(kToneNames)],
              r3d.GetExposure());
    r2d.SubmitText(*font, 10.0f, 78.0f, l2, 0.85f, JLib::Colors::Gray);
}
