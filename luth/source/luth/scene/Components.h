#pragma once

// Convenience aggregator that pulls every Luth::Component header. Gameplay and scene
// serialization include this single path instead of six individual ones.

#include "luth/scene/components/Common.h"
#include "luth/scene/components/Transform.h"
#include "luth/scene/components/Camera.h"
#include "luth/scene/components/Rendering.h"
#include "luth/scene/components/Lights.h"
#include "luth/scene/components/Animation.h"
#include "luth/scene/components/Physics.h"
#include "luth/scene/components/FogVolume.h"
