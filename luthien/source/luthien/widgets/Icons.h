#pragma once

// Single remap point for every editor icon. Call sites use the semantic ICON_*
// names below; this file binds each to a Phosphor glyph. To change an icon
// engine-wide, edit one line here. To swap icon fonts entirely, repoint these
// macros at a different generated header.
//
// Macros (not constexpr) so string-literal concatenation keeps working:
//   ImGui::MenuItem(ICON_FILE "  Open");
//
// Two weights are bundled: Regular (outline, the default) and Fill (accent for
// filled/active states). Phosphor's weights are independent fonts with their own
// codepoints, so a Fill glyph is referenced via its own ICON_PHF_* define and
// rendered by pushing Editor::GetIconFill(). See arch/editor.md.

#include "luthien/widgets/IconsPhosphor.h"
#include "luthien/widgets/IconsPhosphorFill.h"

// ── Generic UI ──
#define ICON_SEARCH            ICON_PH_MAGNIFYING_GLASS
#define ICON_PLUS              ICON_PH_PLUS
#define ICON_MINUS             ICON_PH_MINUS
#define ICON_CLOSE             ICON_PH_X
#define ICON_CHECK             ICON_PH_CHECK
#define ICON_TRASH             ICON_PH_TRASH
#define ICON_DUPLICATE         ICON_PH_COPY
#define ICON_FILTER            ICON_PH_FUNNEL
#define ICON_LIST              ICON_PH_LIST
#define ICON_GRID              ICON_PH_GRID_FOUR
#define ICON_SETTINGS          ICON_PH_GEAR
#define ICON_CARET_DOWN        ICON_PH_CARET_DOWN
#define ICON_CARET_RIGHT       ICON_PH_CARET_RIGHT
#define ICON_CARET_DOWN_FILL   ICON_PHF_CARET_DOWN
#define ICON_CARET_RIGHT_FILL  ICON_PHF_CARET_RIGHT
#define ICON_EXPAND            ICON_PH_ARROWS_OUT
#define ICON_LOCK              ICON_PH_LOCK
#define ICON_UNLOCK            ICON_PH_LOCK_OPEN
#define ICON_EYE               ICON_PH_EYE
#define ICON_EYE_SLASH         ICON_PH_EYE_SLASH
#define ICON_KEYBOARD          ICON_PH_KEYBOARD
#define ICON_TERMINAL          ICON_PH_TERMINAL_WINDOW
#define ICON_ARROW_RIGHT       ICON_PH_ARROW_RIGHT
#define ICON_ARROWS_VERTICAL   ICON_PH_ARROWS_VERTICAL

// ── Status / logs ──
#define ICON_WARNING           ICON_PH_WARNING
#define ICON_WARNING_CIRCLE    ICON_PH_WARNING_CIRCLE
#define ICON_INFO              ICON_PH_INFO
#define ICON_ERROR             ICON_PH_X_CIRCLE
#define ICON_BUG               ICON_PH_BUG
#define ICON_BUG_FILL          ICON_PHF_BUG
#define ICON_BUG_BEETLE_FILL   ICON_PHF_BUG_BEETLE
#define ICON_CIRCLE            ICON_PH_CIRCLE

// ── Edit / history / transport ──
#define ICON_UNDO              ICON_PH_ARROW_COUNTER_CLOCKWISE
#define ICON_REDO              ICON_PH_ARROW_CLOCKWISE
#define ICON_REFRESH           ICON_PH_ARROWS_CLOCKWISE
#define ICON_ROTATE            ICON_PH_ARROWS_CLOCKWISE
#define ICON_HISTORY           ICON_PH_CLOCK_COUNTER_CLOCKWISE
#define ICON_RENAME            ICON_PH_PENCIL_SIMPLE
#define ICON_PLAY              ICON_PH_PLAY
#define ICON_PAUSE             ICON_PH_PAUSE
#define ICON_STOP              ICON_PH_STOP
#define ICON_STEP_FORWARD      ICON_PH_SKIP_FORWARD
#define ICON_PLAY_FILL         ICON_PHF_PLAY
#define ICON_PAUSE_FILL        ICON_PHF_PAUSE
#define ICON_STOP_FILL         ICON_PHF_STOP
#define ICON_STEP_FORWARD_FILL ICON_PHF_SKIP_FORWARD

// ── Viewport / gizmo ──
#define ICON_MOVE              ICON_PH_ARROWS_OUT_CARDINAL
#define ICON_RESIZE            ICON_PH_RESIZE
#define ICON_SELECT            ICON_PH_CURSOR
#define ICON_SELECT_FILL       ICON_PHF_CURSOR
#define ICON_WIREFRAME         ICON_PH_SPHERE
#define ICON_WIREFRAME_SHADED  ICON_PHF_SPHERE
#define ICON_ATOM              ICON_PH_ATOM

// ── Scene / components ──
#define ICON_ENTITY            ICON_PH_DOT_OUTLINE
#define ICON_MESH              ICON_PH_GRID_NINE
#define ICON_BONE              ICON_PH_BONE
#define ICON_BONE_FILL         ICON_PHF_BONE
#define ICON_VIDEO_CAMERA      ICON_PH_VIDEO_CAMERA
#define ICON_VIDEO_CAMERA_FILL ICON_PHF_VIDEO_CAMERA
#define ICON_CAMERA            ICON_PH_CAMERA
#define ICON_CAMERA_FILL       ICON_PHF_CAMERA
#define ICON_LIGHT_DIRECTIONAL      ICON_PH_SUN
#define ICON_LIGHT_DIRECTIONAL_FILL ICON_PHF_SUN
#define ICON_LIGHT_POINT       ICON_PH_LIGHTBULB
#define ICON_LIGHT_POINT_FILL  ICON_PHF_LIGHTBULB
#define ICON_LIGHT_SPOT        ICON_PH_HEADLIGHTS
#define ICON_LIGHT_SPOT_FILL   ICON_PHF_HEADLIGHTS
#define ICON_ANIMATION         ICON_PH_PERSON_SIMPLE_RUN
#define ICON_PHYSICS           ICON_PH_BOUNDING_BOX
#define ICON_FOG               ICON_PH_CLOUD_FOG
#define ICON_FOG_FILL          ICON_PHF_CLOUD_FOG
#define ICON_WIND              ICON_PH_WIND
#define ICON_WIND_FILL         ICON_PHF_WIND
#define ICON_CUBE              ICON_PH_CUBE
#define ICON_SHAPES            ICON_PH_SHAPES

// ── Assets / content ──
#define ICON_MODEL             ICON_PH_CUBE
#define ICON_IMAGE             ICON_PH_IMAGE
#define ICON_MATERIAL          ICON_PH_CIRCLE_HALF
#define ICON_MATERIAL_FILL     ICON_PHF_CIRCLE_HALF
#define ICON_PHYSICS_MATERIAL  ICON_PH_SOCCER_BALL
#define ICON_SHADER            ICON_PH_FILE_CODE
#define ICON_FONT              ICON_PH_TEXT_AA
#define ICON_FILM              ICON_PH_FILM_STRIP
#define ICON_FILE              ICON_PH_FILE
#define ICON_FILE_UNKNOWN      ICON_PH_FILE_DASHED
#define ICON_FOLDER            ICON_PH_FOLDER       // outline (push Editor::GetIconRegular)
#define ICON_FOLDER_FILL       ICON_PHF_FOLDER      // filled  (push Editor::GetIconFill)
#define ICON_FOLDER_OPEN       ICON_PH_FOLDER_OPEN

// ── Panels / misc ──
#define ICON_VIEWPORT          ICON_PH_CUBE_FOCUS
#define ICON_NODE_GRAPH        ICON_PH_GRAPH
#define ICON_DATABASE          ICON_PH_DATABASE
#define ICON_FRAME_GRAPH       ICON_PH_TREE_STRUCTURE
#define ICON_LAYERS            ICON_PH_STACK
#define ICON_IMPORT            ICON_PH_SIGN_IN
#define ICON_GAMEPAD           ICON_PH_GAME_CONTROLLER
#define ICON_CHART             ICON_PH_CHART_LINE
