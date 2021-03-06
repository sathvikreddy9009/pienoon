// Copyright 2014 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "precompiled.h"
#include "SDL_events.h"
#include "analytics_tracking.h"
#include "audio_config_generated.h"
#include "character_state_machine.h"
#include "character_state_machine_def_generated.h"
#include "config_generated.h"
#include "motive/init.h"
#include "motive/io/flatbuffers.h"
#include "motive/math/angle.h"
#include "multiplayer_generated.h"
#include "pie_noon_common_generated.h"
#include "pie_noon_game.h"
#include "pindrop/pindrop.h"
#include "timeline_generated.h"
#include "touchscreen_controller.h"

#include "SDL.h"

#ifdef ANDROID_HMD
#include "fplbase/glplatform.h"
#include "fplbase/renderer_hmd.h"
#endif  // ANDROID_HMD

using motive::Angle;
using mathfu::mat3;

#ifdef _WIN32
#define snprintf(buffer, count, format, ...) \
  _snprintf_s(buffer, count, count, format, __VA_ARGS__)
#endif  // _WIN32

namespace fpl {
namespace pie_noon {

static const int kQuadNumVertices = 4;
static const int kQuadNumIndices = 6;

static const char* kCategoryUi = "Ui";
static const char* kActionClickedButton = "Clicked button";
static const char* kActionViewedTutorialSlide = "Viewed tutorial slide";
static const char* kActionViewedMSTutorialSlide = "MSX-Viewed tutorial slide";
static const char* kLabelSlideDurationFmt = "Slide #%i duration";
static const char* kLabelMSSlideDurationFmt = "MSX-Slide #%i duration";
static const char* kLabelSignInOutButton = "Sign In/Out";
static const char* kLabelLicenseButton = "License";
static const char* kLabelAboutButton = "About";
static const char* kLabelStartButton = "Start";
static const char* kLabelPauseButton = "Pause";
static const char* kLabelUnpauseButton = "Unpause";
static const char* kLabelAchievementsButton = "Achievements";
static const char* kLabelExtrasButton = "Extras";
static const char* kLabelExtrasBackButton = "Extras back button";
static const char* kLabelHowToPlayButton = "How to play";
static const char* kLabelLeaderboardButton = "Leaderboard";
static const char* kLabelMultiscreenButton = "Multiscreen";
static const char* kLabelCardboardButton = "Cardboard";
static const char* kLabelGameModesButton = "Game Modes";

#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
static const char* kCategoryMultiscreen = "Multiscreen";
static const char* kActionStart = "Start";
static const char* kActionFinish = "Finish";
static const char* kActionError = "Error";
static const char* kLabelAdvertising = "Advertising";
static const char* kLabelDiscovery = "Discovery";
static const char* kLabelGameHost = "GameHost";
static const char* kLabelGameClient = "GameClient";
static const char* kLabelReconnection = "Reconnection";
static const char* kLabelHostDisconnected = "HostDisconnect";
static const char* kLabelClientsDisconnected = "ClientDisconnect";
static const char* kLabelConnectionLost = "ConnectionLost";
#endif  // PIE_NOON_USES_GOOGLE_PLAY_GAMES

static const unsigned short kQuadIndices[] = {0, 1, 2, 2, 1, 3};

static const fplbase::Attribute kQuadMeshFormat[] = {
  fplbase::kPosition3f, fplbase::kTexCoord2f, fplbase::kNormal3f,
  fplbase::kTangent4f, fplbase::kEND
};

static const char kAssetsDir[] = "assets";

static const char kConfigFileName[] = "config.pieconfig";

static const char kDefaultOverlayFile[] = "default_overlay.txt";

#ifdef ANDROID_HMD
static const char kCardboardConfigFileName[] = "cardboard_config.pieconfig";
#endif

#ifdef __ANDROID__
static const int kAndroidMaxScreenWidth = 1920;
static const int kAndroidMaxScreenHeight = 1080;
#endif

#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
static GPGManager::GPGIds gpg_ids[kMaxStats];
static std::vector<std::string> achievement_ids;
#endif

std::string PieNoonGame::overlay_name_;

// Return the elapsed milliseconds since the start of the program. This number
// will loop back to 0 after about 49 days; always take the difference to
// properly handle the wrap-around case.
static inline WorldTime CurrentWorldTime(const fplbase::InputSystem& input) {
  return static_cast<WorldTime>(input.Time() * 1000);
}

static inline const UiGroup* TitleScreenButtons(const Config& config) {
#ifdef __ANDROID__
  const bool android_title_screen = true;
#else
  const bool android_title_screen =
      config.always_use_android_title_screen() != 0;
#endif
  return android_title_screen ? config.title_screen_buttons_android()
                              : config.title_screen_buttons_non_android();
}

/// kVersion is used by Google developers to identify which
/// applications uploaded to Google Play are derived from this application.
/// This allows the development team at Google to determine the popularity of
/// this application.
/// How it works: Applications that are uploaded to the Google Play Store are
/// scanned for this version string.  We track which applications are using it
/// to measure popularity.  You are free to remove it (of course) but we would
/// appreciate if you left it in.
static const char kVersion[] = "Pie Noon 1.2.0";

PieNoonGame::PieNoonGame()
    : state_(kUninitialized),
      state_entry_time_(0),
      matman_(renderer_),
      stick_front_(nullptr),
      stick_back_(nullptr),
      shader_lit_textured_normal_(nullptr),
      shader_simple_shadow_(nullptr),
      shader_textured_(nullptr),
      shader_grayscale_(nullptr),
      shadow_mat_(nullptr),
      prev_world_time_(0),
      debug_previous_states_(),
      full_screen_fader_(&renderer_),
      fade_exit_state_(kUninitialized),
      ambience_channel_(),
      stinger_channel_(),
      music_channel_(),
      next_achievement_index_(0) {
  fplbase::SetLoadFileFunction(PieNoonGame::LoadFile);
  version_ = kVersion;
  for (size_t i = 0; i < RenderableId_Count; ++i) {
    cardboard_backs_[i] = nullptr;
  }
}

PieNoonGame::~PieNoonGame() {
  for (int i = 0; i < RenderableId_Count; ++i) {
    std::vector<fplbase::Mesh*>& fronts = cardboard_fronts_[i];
    for (size_t j = 0; j < fronts.size(); ++j) {
      delete fronts[j];
      fronts[j] = nullptr;
    }

    delete cardboard_backs_[i];
    cardboard_backs_[i] = nullptr;
  }

  delete stick_front_;
  stick_front_ = nullptr;

  delete stick_back_;
  stick_back_ = nullptr;
}

bool PieNoonGame::InitializeConfig() {
  if (!LoadFile(kConfigFileName, &config_source_)) {
    fplbase::LogError(fplbase::kError, "can't load %s\n", kConfigFileName);
    return false;
  }
  return true;
}

#ifdef ANDROID_HMD
bool PieNoonGame::InitializeCardboardConfig() {
  if (!LoadFile(kCardboardConfigFileName, &cardboard_config_source_)) {
    fplbase::LogError(fplbase::kError, "can't load %s\n", kCardboardConfigFileName);
    return false;
  }
  return true;
}
#endif  // ANDROID_HMD

// Initialize the 'renderer_' member. No other members have been initialized at
// this point.
bool PieNoonGame::InitializeRenderer() {
  const Config& config = GetConfig();

#ifdef __ANDROID__
  auto max_screen_size =
      fplbase::Vec2i(kAndroidMaxScreenWidth, kAndroidMaxScreenHeight);
  auto window_size = &max_screen_size;
#else
  auto window_size = config.window_size();
#endif
  assert(window_size);
  if (!renderer_.Initialize(LoadVec2i(window_size),
                            config.window_title()->c_str())) {
    fplbase::LogError(fplbase::kError, "Renderer initialization error: %s\n",
             renderer_.last_error().c_str());
    return false;
  }

#ifdef __ANDROID__
  // Restart the app if HW scaler setting failed.
  auto retry = fplbase::LoadPreference("HWScalerRetry", 0);
  const auto kMaxRetry = 3;
  auto current_window_size = fplbase::AndroidGetScalerResolution();
  if (current_window_size.x() != window_size->x() ||
      current_window_size.y() != window_size->y()) {
    if (retry < kMaxRetry) {
      fplbase::LogError("HW Scalar failed. Restarting application.");
      fplbase::SavePreference("HWScalerRetry", retry + 1);
      fplbase::RelaunchApplication();
      return false;
    }
    // The HW may not support the API. Fallback to native resolution pass until
    // the API success next time.
  } else {
    // HW scaler setting was success. Clear retry counter.
    fplbase::SavePreference("HWScalerRetry", 0);
  }
#endif

#ifdef ANDROID_HMD
  vec2i size = fplbase::AndroidGetScalerResolution();
  const vec2i viewport_size =
      size.x() && size.y() ? size : renderer_.window_size();
  fplbase::InitializeUndistortFramebuffer(viewport_size.x(), viewport_size.y());
#endif

  renderer_.set_color(mathfu::kOnes4f);
  // Initialize the first frame as black.
  renderer_.ClearFrameBuffer(mathfu::kZeros4f);
  return true;
}

struct NormalMappedVertex {
  mathfu::vec3_packed pos;
  mathfu::vec2_packed tc;
  mathfu::vec3_packed norm;
  mathfu::vec4_packed tangent;
};

// Initializes 'vertices' at the specified position, aligned up-and-down.
// 'vertices' must be an array of length kQuadNumVertices.
static void CreateVerticalQuad(const vec3& offset, const vec2& geo_size,
                               const vec2& texture_coord_size,
                               NormalMappedVertex* vertices) {
  const float half_width = geo_size[0] * 0.5f;
  const vec3 bottom_left = offset + vec3(-half_width, 0.0f, 0.0f);
  const vec3 top_right = offset + vec3(half_width, geo_size[1], 0.0f);

  vertices[0].pos = bottom_left;
  vertices[1].pos = vec3(top_right[0], bottom_left[1], offset[2]);
  vertices[2].pos = vec3(bottom_left[0], top_right[1], offset[2]);
  vertices[3].pos = top_right;

  const float coord_half_width = texture_coord_size[0] * 0.5f;
  const vec2 coord_bottom_left(0.5f - coord_half_width, 1.0f);
  const vec2 coord_top_right(0.5f + coord_half_width,
                             1.0f - texture_coord_size[1]);

  vertices[0].tc = coord_bottom_left;
  vertices[1].tc = vec2(coord_top_right[0], coord_bottom_left[1]);
  vertices[2].tc = vec2(coord_bottom_left[0], coord_top_right[1]);
  vertices[3].tc = coord_top_right;

  fplbase::Mesh::ComputeNormalsTangents(vertices, &kQuadIndices[0],
      kQuadNumVertices, kQuadNumIndices);
}

// Creates a mesh of a single quad (two triangles) vertically upright.
// The quad's has x and y size determined by the size of the texture.
// The quad is offset in (x,y,z) space by the 'offset' variable.
// Returns a mesh with the quad and texture, or nullptr if anything went wrong.
fplbase::Mesh* PieNoonGame::CreateVerticalQuadMesh(
    const flatbuffers::String* material_name, const vec3& offset,
    const vec2& pixel_bounds, float pixel_to_world_scale) {
  // Don't try to load obviously invalid materials. Suppresses error logs from
  // the material manager.
  if (material_name == nullptr || material_name->c_str()[0] == '\0')
    return nullptr;

  // Load the material from file, and check validity.
  auto material = matman_.LoadMaterial(material_name->c_str());
  bool material_valid = material != nullptr && material->textures().size() > 0;
  if (!material_valid) return nullptr;

  // Create vertex geometry in proportion to the texture size.
  // This is nice for the artist since everything is at the scale of the
  // original artwork.
  assert(pixel_bounds.x() && pixel_bounds.y());
  const vec2 texture_size = vec2(mathfu::RoundUpToPowerOf2(pixel_bounds.x()),
                                 mathfu::RoundUpToPowerOf2(pixel_bounds.y()));
  const vec2 texture_coord_size = pixel_bounds / texture_size;
  const vec2 geo_size = pixel_bounds * vec2(pixel_to_world_scale);

  // Initialize a vertex array in the requested position.
  NormalMappedVertex vertices[kQuadNumVertices];
  CreateVerticalQuad(offset, geo_size, texture_coord_size, vertices);

  // Create mesh and add in quad indices.
  auto mesh = new fplbase::Mesh(vertices, kQuadNumVertices,
                                sizeof(NormalMappedVertex), kQuadMeshFormat);
  mesh->AddIndices(kQuadIndices, kQuadNumIndices, material);
  return mesh;
}

// Load textures for cardboard into 'materials_'. The 'renderer_' and 'matman_'
// members have been initialized at this point.
bool PieNoonGame::InitializeRenderingAssets() {
  const Config& config = GetConfig();

  // Check data validity.
  if (config.renderables()->Length() != RenderableId_Count) {
    fplbase::LogError(fplbase::kError,
                      "%s's 'renderables' array has %d entries, needs %d.\n",
                      kConfigFileName, config.renderables()->Length(),
                      RenderableId_Count);
    return false;
  }

  // Force these textures to be queued up first, since we want to use them for
  // the loading screen.
  matman_.LoadMaterial(config.loading_material()->c_str());
  matman_.LoadMaterial(config.loading_logo()->c_str());
  matman_.LoadMaterial(config.fade_material()->c_str());

  // Create a mesh for the front and back of each cardboard cutout.
  const vec3 front_z_offset(0.0f, 0.0f, config.cardboard_front_z_offset());
  const vec3 back_z_offset(0.0f, 0.0f, config.cardboard_back_z_offset());
  for (int id = 0; id < RenderableId_Count; ++id) {
    auto renderable = config.renderables()->Get(id);
    const vec3 offset = renderable->offset() == nullptr
                            ? mathfu::kZeros3f
                            : LoadVec3(renderable->offset());
    const vec3 front_offset = offset + front_z_offset;
    const vec3 back_offset = offset + back_z_offset;
    const auto pixel_bounds_ptr = renderable->pixel_bounds();
    const vec2 pixel_bounds(pixel_bounds_ptr == nullptr
                                ? mathfu::kZeros2i
                                : LoadVec2i(pixel_bounds_ptr));
    const float pixel_to_world_scale =
        renderable->geometry_scale() * config.pixel_to_world_scale();

    const auto front = renderable->cardboard_fronts();
    cardboard_fronts_[id].resize(front->size(), nullptr);
    for (size_t i = 0; i < front->size(); ++i) {
      cardboard_fronts_[id][i] = CreateVerticalQuadMesh(
          front->Get(i), front_offset, pixel_bounds, pixel_to_world_scale);
    }

    cardboard_backs_[id] =
        CreateVerticalQuadMesh(renderable->cardboard_back(), back_offset,
                               pixel_bounds, pixel_to_world_scale);
  }

  // We default to the invalid texture, so it has to exist.
  if (!cardboard_fronts_[RenderableId_Invalid][0]) {
    fplbase::LogError(fplbase::kError, "Can't load backup texture.\n");
    return false;
  }

  // Create stick front and back meshes.
  const vec3 stick_front_offset(0.0f, config.stick_y_offset(),
                                config.stick_front_z_offset());
  const vec3 stick_back_offset(0.0f, config.stick_y_offset(),
                               config.stick_back_z_offset());
  stick_front_ = CreateVerticalQuadMesh(
      config.stick_front(), stick_front_offset, LoadVec2(config.stick_bounds()),
      config.pixel_to_world_scale());
  stick_back_ = CreateVerticalQuadMesh(config.stick_back(), stick_back_offset,
                                       LoadVec2(config.stick_bounds()),
                                       config.pixel_to_world_scale());

  // Load all shaders we use:
  shader_lit_textured_normal_ =
      matman_.LoadShader("shaders/lit_textured_normal");
  shader_cardboard = matman_.LoadShader("shaders/cardboard");
  shader_simple_shadow_ = matman_.LoadShader("shaders/simple_shadow");
  shader_textured_ = matman_.LoadShader("shaders/textured");
  shader_grayscale_ = matman_.LoadShader("shaders/grayscale");
  if (!(shader_lit_textured_normal_ && shader_cardboard &&
        shader_simple_shadow_ && shader_textured_ && shader_grayscale_))
    return false;

  // Load shadow material:
  shadow_mat_ = matman_.LoadMaterial("materials/floor_shadows.fplmat");
  if (!shadow_mat_) return false;

  // Load debug shader if available
  gui_menu_.LoadDebugShaderAndOptions(&config, &matman_);

  // Load all the menu textures.
  gui_menu_.LoadAssets(TitleScreenButtons(config), &matman_);
  gui_menu_.LoadAssets(config.touchscreen_zones(), &matman_);
  gui_menu_.LoadAssets(config.pause_screen_buttons(), &matman_);
  gui_menu_.LoadAssets(config.multiplayer_host(), &matman_);
  gui_menu_.LoadAssets(config.multiplayer_client(), &matman_);
  gui_menu_.LoadAssets(config.join_screen_buttons(), &matman_);
  gui_menu_.LoadAssets(config.extras_screen_buttons(), &matman_);
  gui_menu_.LoadAssets(config.msx_screen_buttons(), &matman_);
  gui_menu_.LoadAssets(config.msx_pleasewait_screen_buttons(), &matman_);
  gui_menu_.LoadAssets(config.msx_waitingforplayers_screen_buttons(), &matman_);
  gui_menu_.LoadAssets(config.msx_waitingforgame_screen_buttons(), &matman_);
  gui_menu_.LoadAssets(config.msx_searching_screen_buttons(), &matman_);
  gui_menu_.LoadAssets(config.msx_connecting_screen_buttons(), &matman_);
  gui_menu_.LoadAssets(config.msx_cant_host_game_screen_buttons(), &matman_);
  gui_menu_.LoadAssets(config.msx_connection_lost_screen_buttons(), &matman_);
  gui_menu_.LoadAssets(config.msx_host_disconnected_screen_buttons(), &matman_);
  gui_menu_.LoadAssets(config.msx_all_players_disconnected_screen_buttons(),
                       &matman_);
  gui_menu_.LoadAssets(config.game_modes_screen_buttons(), &matman_);
  // Configure the full screen fader.
  full_screen_fader_.set_material(
      matman_.FindMaterial(config.fade_material()->c_str()));
  full_screen_fader_.set_shader(shader_textured_);

  // Start the thread that actually loads all assets we requested above.
  matman_.StartLoadingTextures();

  return true;
}

// Create state matchines, characters, controllers, etc. present in
// 'gamestate_'.
bool PieNoonGame::InitializeGameState() {
  const Config& config = GetConfig();

  game_state_.set_config(&config);
  game_state_.set_cardboard_config(&GetCardboardConfig());

  // Register the motivator types with the MotiveEngine.
  motive::OvershootInit::Register();
  motive::SplineInit::Register();
  motive::MatrixInit::Register();

  // Load flatbuffer into buffer.
  if (!LoadFile("character_state_machine_def.piestate",
                &state_machine_source_)) {
    fplbase::LogError(fplbase::kError,
                      "Error loading character state machine.\n");
    return false;
  }

  // Grab the state machine from the buffer.
  auto state_machine_def = GetStateMachine();
  if (!CharacterStateMachineDef_Validate(state_machine_def)) {
    fplbase::LogError(fplbase::kError, "State machine is invalid.\n");
    return false;
  }

  for (int i = 0; i < ControlScheme::kDefinedControlSchemeCount; i++) {
    PlayerController* controller = new PlayerController();
    controller->Initialize(&input_, ControlScheme::GetDefaultControlScheme(i));
    AddController(controller);
  }

  // Add a touch screen controller into the controller list, so that touch
  // inputs are processed correctly and assigned a character:
  touch_controller_ = new TouchscreenController();

  vec2 window_size = vec2(static_cast<float>(renderer_.window_size().x()),
                          static_cast<float>(renderer_.window_size().y()));
  touch_controller_->Initialize(&input_, window_size, &config, &game_state_);

  AddController(touch_controller_);

  // Add a cardboard controller into the controller list, so that input
  // from a cardboard device can be handled correctly
  cardboard_controller_ = new CardboardController();

  cardboard_controller_->Initialize(&game_state_, &input_);

  AddController(cardboard_controller_);

  // Create characters.
  for (unsigned int i = 0; i < config.character_count(); ++i) {
    AiController* controller = new AiController();
    controller->Initialize(&game_state_, &config, i);
    game_state_.characters().push_back(std::unique_ptr<Character>(
        new Character(i, controller, config, state_machine_def)));
    AddController(controller);
    controller->Initialize(&game_state_, &config, i);
  }

  multiplayer_director_.reset(new MultiplayerDirector());
  multiplayer_director_->Initialize(&game_state_, &config);
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
  multiplayer_director_->RegisterGPGMultiplayer(&gpg_multiplayer_);
#else
  multiplayer_director_->SetDebugInputSystem(&input_);
#endif

  for (unsigned int i = 0; i < config.character_count(); ++i) {
    MultiplayerController* controller = new MultiplayerController();
    controller->Initialize(&game_state_, &config);
    AddController(controller);
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
    multiplayer_director_->RegisterController(controller);
#endif
  }

  debug_previous_states_.resize(config.character_count(), -1);
  game_state_.RegisterMultiplayerDirector(multiplayer_director_.get());

  return true;
}

class AudioEngineVolumeControl {
 public:
  AudioEngineVolumeControl(pindrop::AudioEngine* audio) : audio_(audio) {}
  void operator()(void* userdata) {
    SDL_Event* event = static_cast<SDL_Event*>(userdata);
    switch (event->type) {
      case SDL_APP_WILLENTERBACKGROUND:
        audio_->Pause(true);
        break;
      case SDL_APP_DIDENTERFOREGROUND:
        audio_->Pause(false);
        break;
      default:
        break;
    }
  }

 private:
  pindrop::AudioEngine* audio_;
};

bool PieNoonGame::InitializeGpgIds() {
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
  const Config& config = GetConfig();

  // Load the Google Play Games ids from Android resources.
  std::vector<std::string> leaderboards;
  std::vector<std::string> events;
  StringArrayResource(config.gpg_leaderboards_resource()->c_str(),
                      &leaderboards);
  StringArrayResource(config.gpg_events_resource()->c_str(), &events);
  StringArrayResource(config.gpg_achievements_resource()->c_str(),
                      &achievement_ids);
  const bool ids_valid = static_cast<int>(leaderboards.size()) == kMaxStats &&
                         static_cast<int>(events.size()) == kMaxStats;
  if (!ids_valid) return false;

  // Convert them to our global variable that stores values.
  // TODO: Load directly into these arrays to eliminate this copy.
  for (int i = 0; i < kMaxStats; ++i) {
    gpg_ids[i].leaderboard = leaderboards[i];
    gpg_ids[i].event = events[i];
  }
#endif  // PIE_NOON_USES_GOOGLE_PLAY_GAMES
  return true;
}

bool PieNoonGame::LoadFile(const char* filename, std::string* dest) {
  const char* read_filename = filename;
  std::string overlay;
  if (!overlay_name_.empty()) {
    overlay = "overlays/" + overlay_name_ + "/" + std::string(filename);
    const char* overlay_filename = overlay.c_str();
    auto handle = SDL_RWFromFile(overlay_filename, "rb");
    if (handle) {
      SDL_RWclose(handle);
      read_filename = overlay_filename;
    }
  }
  return fplbase::LoadFileRaw(read_filename, dest);
}

// Initialize each member in turn. This is logically just one function, since
// the order of initialization cannot be changed. However, it's nice for
// debugging and readability to have each section lexographically separate.
bool PieNoonGame::Initialize(const char* const binary_directory) {
  fplbase::LogInfo(fplbase::kApplication, "PieNoon initializing...\n");

  if (!fplbase::ChangeToUpstreamDir(binary_directory, kAssetsDir)) return false;

  if (overlay_name_ == "") {
    std::string default_overlay;
    if (LoadFile(kDefaultOverlayFile, &default_overlay)) {
      // trim whitespace from the end of the file contents
      default_overlay.erase(default_overlay.find_last_not_of(" \n\r\t") + 1);
      if (default_overlay != "") {
        fplbase::LogInfo(fplbase::kApplication,
                         "Forcing default overlay of %s\n",
                default_overlay.c_str());
        PieNoonGame::SetOverlayName(default_overlay.c_str());
      }
    }
  }

  if (!InitializeConfig()) return false;
#ifdef ANDROID_HMD
  if (!InitializeCardboardConfig()) return false;
#endif
  if (!InitializeGpgIds()) return false;

  if (!InitializeRenderer()) return false;

  if (!InitializeRenderingAssets()) return false;

  input_.Initialize();

  // Some people are having trouble loading the audio engine, and it's not
  // strictly necessary for gameplay, so don't die if the audio engine fails to
  // initialize.
  if (!audio_engine_.Initialize(GetConfig().audio())) {
    fplbase::LogError(fplbase::kApplication,
                      "Failed to initialize audio engine.\n");
  }

  if (!audio_engine_.LoadSoundBank("sound_banks/sound_assets.pinbank")) {
    fplbase::LogError(fplbase::kApplication, "Failed to load sound bank.\n");
  }

  // Start loading sounds
  audio_engine_.StartLoadingSoundFiles();

  input_.AddAppEventCallback(AudioEngineVolumeControl(&audio_engine_));

  if (!InitializeGameState()) return false;

#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
  if (!gpg_manager.Initialize(fplbase::LoadPreference("logged_in", 1) != 0))
    return false;

  if (!gpg_multiplayer_.Initialize(GetConfig()
                                       .multiscreen_options()
                                       ->nearby_connections_service_id()
                                       ->c_str())) {
    fplbase::LogError(fplbase::kApplication,
                      "GPGMultiplayer::Initialize failed\n");
    return false;
  }
  for (unsigned int i = 0; i < GetConfig()
                                   .multiscreen_options()
                                   ->nearby_connections_app_identifiers()
                                   ->Length();
       i++) {
    auto app_id = GetConfig()
                      .multiscreen_options()
                      ->nearby_connections_app_identifiers()
                      ->Get(i);

    gpg_multiplayer_.AddAppIdentifier(app_id->c_str());
  }
  gpg_multiplayer_.set_max_connected_players_allowed(
      GetConfig().multiscreen_options()->max_players());
#endif

  fplbase::LogInfo(fplbase::kApplication, "PieNoon initialization complete\n");
  return true;
}

// Returns the mesh for renderable_id, if we have one, or the pajama mesh
// (a mesh with a texture that's obviously wrong), if we don't.
fplbase::Mesh* PieNoonGame::GetCardboardFront(int renderable_id, int variant) {
  // Return the invalid mesh if the indices are out of bounds.
  auto invalid_front = cardboard_fronts_[RenderableId_Invalid][0];
  if (renderable_id < 0 || RenderableId_Count <= renderable_id) {
    return invalid_front;
  }

  // Clamp the variant to the valid range.
  // Return it, if available. Otherwise, return the invalid front.
  auto& fronts = cardboard_fronts_[renderable_id];
  const int variant_clamped =
      mathfu::Clamp(variant, 0, static_cast<int>(fronts.size()) - 1);
  auto front = fronts[variant_clamped];
  return front == nullptr ? invalid_front : front;
}

void PieNoonGame::RenderCardboard(const SceneDescription& scene,
                                  const mat4& camera_transform) {
  const Config& config = GetConfig();

  for (size_t i = 0; i < scene.renderables().size(); ++i) {
    const auto& renderable = scene.renderables()[i];
    const int id = renderable->id();

    // Set up vertex transformation into projection space.
    const mat4 mvp = camera_transform * renderable->world_matrix();
    renderer_.set_model_view_projection(mvp);

    // Set the camera and light positions in object space.
    const mat4 world_matrix_inverse = renderable->world_matrix().Inverse();
    renderer_.set_camera_pos(world_matrix_inverse *
                             game_state_.camera().Position());

    // TODO: check amount of lights.
    renderer_.set_light_pos(world_matrix_inverse * (*scene.lights()[0]));

    // The popsicle stick and cardboard back are always uncolored.
    renderer_.set_color(mathfu::kOnes4f);

    // Note: Draw order is back-to-front, so draw the cardboard back, then
    // popsicle stick, then cardboard front--in that order.
    //
    // If we have a back, draw the back too, slightly offset.
    // The back is the *inside* of the cardboard, representing corrugation.
    if (cardboard_backs_[id]) {
      shader_cardboard->Set(renderer_);
      cardboard_backs_[id]->Render(renderer_);
    }

    // Draw the popsicle stick that props up the cardboard.
    if (config.renderables()->Get(id)->stick() && stick_front_ != nullptr &&
        stick_back_ != nullptr) {
      shader_textured_->Set(renderer_);
      stick_front_->Render(renderer_);
      stick_back_->Render(renderer_);
    }

    renderer_.set_color(renderable->color());

    if (config.renderables()->Get(id)->cardboard()) {
      shader_cardboard->Set(renderer_);
      shader_cardboard->SetUniform(
          "ambient_material", LoadVec3(config.cardboard_ambient_material()));
      shader_cardboard->SetUniform(
          "diffuse_material", LoadVec3(config.cardboard_diffuse_material()));
      shader_cardboard->SetUniform(
          "specular_material", LoadVec3(config.cardboard_specular_material()));
      shader_cardboard->SetUniform("shininess", config.cardboard_shininess());
      shader_cardboard->SetUniform("normalmap_scale",
                                   config.cardboard_normalmap_scale());
    } else {
      shader_textured_->Set(renderer_);
    }
    auto front = GetCardboardFront(id, renderable->variant());
    front->Render(renderer_);
  }
}

void PieNoonGame::Render(const SceneDescription& scene) {
  if (game_state_.is_in_cardboard()) {
    RenderForCardboard(scene);
  } else {
    RenderForDefault(scene);
  }
}

void PieNoonGame::RenderForDefault(const SceneDescription& scene) {
  RenderScene(scene, mat4::Identity(), renderer_.window_size());
}

void PieNoonGame::RenderForCardboard(const SceneDescription& scene) {
#ifdef ANDROID_HMD
  fplbase::HeadMountedDisplayViewSettings view_settings;
  HeadMountedDisplayRenderStart(
      input_.head_mounted_display_input(), &renderer_, mathfu::kZeros4f,
      game_state_.use_undistort_rendering(), &view_settings);
  auto res = renderer_.window_size();
  vec2i half_res(res.x() / 2.0f, res.y());
  // Perform two render passes, one for each half of the screen
  for (int i = 0; i < 2; i++) {
    GL_CALL(glViewport(view_settings.viewport_extents[i][0],
                       view_settings.viewport_extents[i][1],
                       view_settings.viewport_extents[i][2],
                       view_settings.viewport_extents[i][3]));
    // Convert the transforms from cardboard space to game space
    CorrectCardboardCamera(view_settings.viewport_transforms[i]);
    RenderScene(scene, view_settings.viewport_transforms[i], half_res);
  }
  HeadMountedDisplayRenderEnd(&renderer_,
                              game_state_.use_undistort_rendering());
#else
  (void)scene;
#endif  // ANDROID_HMD
}

void PieNoonGame::RenderScene(const SceneDescription& scene,
                              const mat4& additional_camera_changes,
                              const vec2i& resolution) {
  const Config& config = GetConfig();
  const Config& cardboard_config = GetCardboardConfig();

  float viewport_angle = game_state_.is_in_cardboard()
                             ? cardboard_config.viewport_angle()
                             : config.viewport_angle();
  // Final matrix that applies the view frustum to bring into screen space.
  mat4 perspective_matrix_ = mat4::Perspective(
      viewport_angle, resolution.x() / static_cast<float>(resolution.y()),
      config.viewport_near_plane(), config.viewport_far_plane(), -1.0f);

  const mat4 camera_transform =
      perspective_matrix_ * (additional_camera_changes * scene.camera());

  // Render a ground plane.
  // TODO: Replace with a regular environment prop. Calculate scale_bias from
  // environment prop size.
  renderer_.set_model_view_projection(camera_transform);
  renderer_.set_color(mathfu::kOnes4f);
  shader_textured_->Set(renderer_);
  auto ground_mat = matman_.LoadMaterial("materials/floor.fplmat");
  assert(ground_mat);
  ground_mat->Set(renderer_);
  const float ground_width = game_state_.is_in_cardboard()
                                 ? cardboard_config.ground_plane_width()
                                 : config.ground_plane_width();
  const float ground_depth = game_state_.is_in_cardboard()
                                 ? cardboard_config.ground_plane_depth()
                                 : config.ground_plane_depth();
  fplbase::Mesh::RenderAAQuadAlongX(vec3(-ground_width, 0, 0),
                           vec3(ground_width, 0, ground_depth), vec2(0, 0),
                           vec2(1.0f, 1.0f));
  const vec4 world_scale_bias(1.0f / (2.0f * ground_width), 1.0f / ground_depth,
                              0.5f, 0.0f);

  // Render shadows for all Renderables first, with depth testing off so
  // they blend properly.
  renderer_.DepthTest(false);
  // This is a bit of a hack - We want to be in kBlendModeAlpha, but
  // FPLBase's renderer assumes that no one else is messing with the openGL
  // settings besides it.  Since cardboard mode makes its own openGL calls,
  // we have to set some other blend mode first, so that it will recognize
  // that things have change, and it should call glBlendMode(GL_ENABLE) again.
  renderer_.SetBlendMode(fplbase::kBlendModeOff);
  renderer_.SetBlendMode(fplbase::kBlendModeAlpha);
  renderer_.set_model_view_projection(camera_transform);
  renderer_.set_light_pos(*scene.lights()[0]);  // TODO: check amount of lights.
  shader_simple_shadow_->SetUniform("world_scale_bias", world_scale_bias);
  for (size_t i = 0; i < scene.renderables().size(); ++i) {
    const auto& renderable = scene.renderables()[i];
    const int id = renderable->id();
    auto front = GetCardboardFront(id, renderable->variant());
    if (config.renderables()->Get(id)->shadow()) {
      renderer_.set_model(renderable->world_matrix());
      shader_simple_shadow_->Set(renderer_);
      // The first texture of the shadow shader has to be that of the
      // billboard.
      shadow_mat_->textures()[0] = front->GetMaterial(0)->textures()[0];
      shadow_mat_->Set(renderer_);
      front->Render(renderer_, true);
    }
  }
  renderer_.DepthTest(true);

  // Now render the Renderables normally, on top of the shadows.
  RenderCardboard(scene, camera_transform);

  // Render any UI/HUD/Splash on top
  Render2DElements(scene, additional_camera_changes);
}

void PieNoonGame::Render2DElements(const SceneDescription& scene,
                                   const mat4& additional_camera_changes) {
  // Set up an ortho camera for all 2D elements, with (0, 0) in the top left,
  // and the bottom right the windows size in pixels.
  mathfu::vec2i res = renderer_.window_size();

  if (!game_state_.is_in_cardboard()) {
    mat4 ortho_mat = mathfu::OrthoHelper<float>(
        0.0f, static_cast<float>(res.x()), static_cast<float>(res.y()), 0.0f,
        -1.0f, 1.0f);
    renderer_.set_model_view_projection(ortho_mat);
  } else {
    // Center it at 0, 0
    const mat4 translate_mat =
        mat4::FromTranslationVector(vec3(-res.x() / 2, -res.y() / 2, 0));
    // Scale it into a more reasonable size:
    const mat4 scale_mat = mat4::FromScaleVector(vec3(0.010f, -0.010f, -1.0f));
    // Move it to a nice spot in world space:
    const mat4 translate2_mat =
        mat4::FromTranslationVector(vec3(0.0f, 4.0f, 12.6f));
    const Config& config = GetConfig();
    const Config& cardboard_config = GetCardboardConfig();

    float viewport_angle = cardboard_config.viewport_angle();

    mat4 perspective_matrix_ = mat4::Perspective(
        viewport_angle, res.x() / static_cast<float>(res.y()),
        config.viewport_near_plane(), config.viewport_far_plane(), -1.0f);

    const mat4 camera_transform =
        perspective_matrix_ * (additional_camera_changes * scene.camera());

    mat4 rotate_towards_camera = mat4::FromRotationMatrix(
        mathfu::quat::FromAngleAxis(0.5f * static_cast<float>(M_PI) / 2.0f,
                                    mathfu::kAxisY3f)
            .ToMatrix());

    renderer_.set_model_view_projection(camera_transform * translate2_mat *
                                        rotate_towards_camera * scale_mat *
                                        translate_mat);
  }

// Update the currently drawing Google Play Games image. Displays "Sign In"
// when currently signed-out, and "Sign Out" when currently signed in.
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
  bool is_logged_in = gpg_manager.LoggedIn();
  const int material_index = is_logged_in ? 0 : 1;

  auto gpg_button = gui_menu_.FindButtonById(ButtonId_MenuSignIn);
  if (gpg_button) gpg_button->set_current_up_material(material_index);

  auto gpg_text = gui_menu_.FindImageById(ButtonId_MenuSignInText);
  if (gpg_text) gpg_text->set_current_material_index(material_index);

  auto achievements_button =
      gui_menu_.FindButtonById(ButtonId_MenuAchievements);
  if (achievements_button) achievements_button->set_is_active(is_logged_in);
  auto leaderboards_button = gui_menu_.FindButtonById(ButtonId_MenuLeaderboard);
  if (leaderboards_button) leaderboards_button->set_is_active(is_logged_in);

#endif

  if (!fplbase::SupportsHeadMountedDisplay()) {
    auto cardboard_button = gui_menu_.FindButtonById(ButtonId_MenuCardboard);
    if (cardboard_button) cardboard_button->set_is_visible(false);
  }

  auto sushi_button = gui_menu_.FindButtonById(ButtonId_Sushi);
  if (sushi_button) {
    bool show_sushi_button = false;
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
    // Magic strings comes from res/values/play_games.xml
    // if the first achievement is unlocked, display the sushi.
    show_sushi_button = (show_sushi_button ||
                         gpg_manager.IsAchievementUnlocked(achievement_ids[0]));
#endif  // PIE_NOON_USES_GOOGLE_PLAY_GAMES
    sushi_button->set_is_visible(show_sushi_button);
  }

  // Loop through the 2D elements. Draw each subsequent one slightly closer
  // to the camera so that they appear on top of the previous ones.
  gui_menu_.Render(&renderer_);
}

void PieNoonGame::CorrectCardboardCamera(mat4& cardboard_camera) {
  // The game's coordinate system has x and y reversed from the cardboard
  const mat4 rotation = mat4::FromScaleVector(vec3(-1, -1, 1));
  cardboard_camera = rotation * cardboard_camera * rotation;
}

// Debug function to print out state machine transitions.
void PieNoonGame::DebugPrintCharacterStates() {
  // Display the state changes, at least until we get real rendering up.
  for (size_t i = 0; i < game_state_.characters().size(); ++i) {
    auto& character = game_state_.characters()[i];
    auto id = character->state_machine()->current_state()->id();
    if (debug_previous_states_[i] != id) {
      fplbase::LogInfo(fplbase::kApplication,
                       "character %d - Health %2d, State %s [%d]\n", i,
              character->health(), EnumNameStateId(id), id);
      debug_previous_states_[i] = id;
    }
  }
}

// Debug function to print out the state of each AirbornePie.
void PieNoonGame::DebugPrintPieStates() {
  for (unsigned int i = 0; i < game_state_.pies().size(); ++i) {
    auto& pie = game_state_.pies()[i];
    const vec3 position = pie->Position();
    fplbase::LogInfo(fplbase::kApplication,
            "Pie from [%i]->[%i] w/ %i dmg at pos[%.2f, %.2f, %.2f]\n",
            pie->source(), pie->target(), pie->damage(), position.x(),
            position.y(), position.z());
  }
}

const Config& PieNoonGame::GetConfig() const {
  return *fpl::pie_noon::GetConfig(config_source_.c_str());
}

const Config& PieNoonGame::GetCardboardConfig() const {
#ifdef ANDROID_HMD
  return *fpl::pie_noon::GetConfig(cardboard_config_source_.c_str());
#else
  return GetConfig();
#endif
}

const CharacterStateMachineDef* PieNoonGame::GetStateMachine() const {
  return fpl::pie_noon::GetCharacterStateMachineDef(
      state_machine_source_.c_str());
}

struct ButtonToTranslation {
  int button;
  vec3 translation;
};

// Debug function to move the camera if the mouse button is down.
void PieNoonGame::DebugCamera() {
  const Config& config = GetConfig();

  // Only move the camera if the left mouse button (or first finger) is down.
  if (!input_.GetButton(fplbase::K_POINTER1).is_down()) return;

  static const ButtonToTranslation kDebugCameraButtons[] = {
      {'d', mathfu::kAxisX3f}, {'a', -mathfu::kAxisX3f},
      {'w', mathfu::kAxisZ3f}, {'s', -mathfu::kAxisZ3f},
      {'q', mathfu::kAxisY3f}, {'e', -mathfu::kAxisY3f},
  };

  // Convert key presses to translations along camera axes.
  vec3 camera_translation(mathfu::kZeros3f);
  for (size_t i = 0; i < PIE_ARRAYSIZE(kDebugCameraButtons); ++i) {
    const ButtonToTranslation& button = kDebugCameraButtons[i];
    if (input_.GetButton(button.button).is_down()) {
      camera_translation += button.translation;
    }
  }

  // Camera rotation is a function of how much the mouse is moved (or finger
  // is dragged).
  const vec2 mouse_delta = vec2(input_.get_pointers()[0].mousedelta);

  // Return early if there is no change on the camera.
  const bool translate = camera_translation[0] != 0.0f ||
                         camera_translation[1] != 0.0f ||
                         camera_translation[2] != 0.0f;
  const bool rotate = mouse_delta[0] != 0.0f || mouse_delta[1] != 0.0f;
  if (!translate && !rotate) return;

  // Calculate the ortho-normal axes of camera space.
  GameCamera& camera = game_state_.camera();
  const vec3 forward = camera.Forward();
  const vec3 side = camera.Side();
  const vec3 up = camera.Up();

  // Convert translation from camera space to world space and scale.
  if (translate) {
    const vec3 scale = LoadVec3(config.button_to_camera_translation_scale());
    const vec3 world_translation =
        scale * (camera_translation[0] * side + camera_translation[1] * up +
                 camera_translation[2] * forward);
    const vec3 new_position = camera.Position() + world_translation;
    camera.OverridePosition(new_position);

    if (config.print_camera_orientation()) {
      fplbase::LogInfo(fplbase::kApplication,
                       "camera position (%.5ff, %.5ff, %.5ff)\n",
              new_position[0], new_position[1], new_position[2]);
    }
  }

  // Move the camera target in the camera plane.
  if (rotate) {
    // Apply mouse movement along up and side axes. Scale so that no matter
    // distance, the same angle is applied.
    const float dist = camera.Dist();
    const float scale = dist * config.mouse_to_camera_rotation_scale();
    const vec3 unscaled_delta = mouse_delta.x() * side + mouse_delta.y() * up;
    const vec3 target_delta = scale * unscaled_delta;
    const vec3 new_target = camera.Target() + target_delta;
    camera.OverrideTarget(new_target);

    if (config.print_camera_orientation()) {
      fplbase::LogInfo(fplbase::kApplication,
                       "camera target (%.5ff, %.5ff, %.5ff)\n",
              new_target[0], new_target[1], new_target[2]);
    }
  }
}

// The join menu has a series of images that disappear one-by-one.
// This functions as a countdown timer. This function converts the current
// time into the id of the image that is currently disappearing.
ButtonId PieNoonGame::CurrentlyAnimatingJoinImage(WorldTime time) const {
  const WorldTime time_in_state = time - join_animation_start_time_;
  const int seconds_in_state = time_in_state / kMillisecondsPerSecond;
  const int id = countdown_start_button_ + seconds_in_state;
  const bool valid_id = id <= ButtonId_Counter5;
  return valid_id ? static_cast<ButtonId>(id) : ButtonId_Undefined;
}

PieNoonState PieNoonGame::UpdatePieNoonState() {
  const Config& config = GetConfig();
  const WorldTime time = CurrentWorldTime(input_);
  // If a full screen fade is active.
  if (Fading()) {
    // If the fade hits the halfway point (opaque) enter the fade exit state.
    if (full_screen_fader_.Render(time)) {
      return fade_exit_state_;
    }
    // If the fade is complete, stop the transition process.
    if (full_screen_fader_.Finished(time)) {
      fade_exit_state_ = kUninitialized;
    }
  }
  switch (state_) {
    case kLoadingInitialMaterials: {
      if (matman_.FindMaterial(config.loading_material()->c_str())
              ->textures()[0]
              ->id() &&
          matman_.FindMaterial(config.loading_logo()->c_str())
              ->textures()[0]
              ->id() &&
          full_screen_fader_.material()->textures()[0]->id()) {
        // Fade in the loading screen.
        FadeToPieNoonState(kLoading, config.full_screen_fade_time(),
                           mathfu::kZeros4f, false);
      }
      break;
    }
    case kLoading: {
      // When we initialized assets, we kicked off a thread to load all
      // textures. Here we check if those have finished loading.
      // We also leave the loading screen up for a minimum amount of time.
      if (!Fading() && matman_.TryFinalize() && audio_engine_.TryFinalize() &&
          (time - state_entry_time_) > config.min_loading_time()) {
        // If we've already displayed the tutorial before, jump straight to
        // the game. If we don't have the capability to record our previous
        // tutorial views, also jump straight to the game.
        int displayed_tutorial = fplbase::LoadPreference("displayed_tutorial",
                                                         0);
        const PieNoonState first_state =
            displayed_tutorial ? kFinished : kTutorial;
        tutorial_slide_time_ = time;

        // Fade out the loading screen and fade in the scene or tutorial.
        FadeToPieNoonState(first_state, config.full_screen_fade_time(),
                           mathfu::kZeros4f, true);
      }
      break;
    }
    case kJoining: {
      if (!game_state_.is_multiscreen()) {
        // Allow players to join with any key press.
        HandlePlayersJoining();

        UpdateCountdownImage(time);

        // After a few seconds, start the game.
        if (join_id_ == ButtonId_Undefined) {
          game_state_.PreGameLogging();
          // Fade to the game
          FadeToPieNoonState(kPlaying, GetConfig().full_screen_fade_time(),
                             mathfu::kZeros4f, true);
        }
      } else {
        return kPlaying;
      }
      break;
    }
    case kPlaying: {
      if (input_.GetButton(fplbase::FPLK_AC_BACK).went_down() ||
          input_.GetButton(fplbase::FPLK_p).went_down() ||
          input_.minimized_frame() == input_.frames()) {
        SendTrackerEvent(kCategoryUi, kActionClickedButton, kLabelPauseButton,
                         game_state_.is_multiscreen());
        pause_time_ = time;
        return kPaused;
      }

      if (game_state_.IsGameOver() && stinger_channel_.Valid() &&
          !stinger_channel_.Playing()) {
        game_state_.PostGameLogging();
        if (game_state_.is_multiscreen() && multiplayer_director_ != nullptr) {
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
          multiplayer_director_->SendEndGameMsg();
          SendTrackerEvent(kCategoryMultiscreen, kActionFinish, kLabelGameHost);
          gpg_multiplayer_.StartAdvertising();
#endif
          return kMultiplayerWaiting;
        } else {
          return kFinished;
        }
      }
      break;
    }
    case kPaused: {
      if (input_.GetButton(fplbase::FPLK_AC_BACK).went_down()) {
        SendTrackerEvent(kCategoryUi, kActionClickedButton, kLabelUnpauseButton,
                         time - pause_time_);
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
        gpg_multiplayer_.ResetToIdle();
#endif
        gui_menu_.Setup(TitleScreenButtons(config), &matman_);
        return kFinished;
      }
      return HandleMenuButtons(time);
    }
    case kMultiplayerWaiting: {
      if (input_.GetButton(fplbase::FPLK_AC_BACK).went_down()) {
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
        gpg_multiplayer_.ResetToIdle();
#endif
        gui_menu_.Setup(config.msx_screen_buttons(), &matman_);
        return kFinished;
      }
      return HandleMenuButtons(time);
    }

    case kFinished: {
      if (input_.GetButton(fplbase::FPLK_AC_BACK).went_down()) {
        const bool in_submenu =
            (gui_menu_.menu_def() == config.extras_screen_buttons() ||
             gui_menu_.menu_def() == config.msx_screen_buttons() ||
             gui_menu_.menu_def() == config.game_modes_screen_buttons());
        if (in_submenu) {
          gui_menu_.Setup(TitleScreenButtons(config), &matman_);
        } else if (game_state_.is_in_cardboard()) {
          gui_menu_.Setup(TitleScreenButtons(config), &matman_);
          game_state_.set_is_in_cardboard(false);
          game_state_.Reset();
        } else {
          input_.set_exit_requested(true);
        }
      }
      return HandleMenuButtons(time);
    }
    case kTutorial: {
      const size_t num_slides = tutorial_slides_->size();
      const bool past_last_slide =
          tutorial_slide_index_ >= static_cast<int>(num_slides);

      if (past_last_slide && !Fading()) {
        // Record that we've successfully displayed the tutorial so that we
        // don't display it again next time.
        if (!game_state_.is_multiscreen()) {
          fplbase::SavePreference("displayed_tutorial", 1);

          // Fade out the tutorial screen and fade in the main menu.
          FadeToPieNoonState(kFinished, config.full_screen_fade_time(),
                             mathfu::kZeros4f, true);
        } else {
          // we are in multi-screen mode
          gui_menu_.Setup(GetConfig().msx_screen_buttons(), &matman_);
          return kFinished;
        }
      }
      break;
    }
    case kMultiscreenClient: {
      if (input_.GetButton(fplbase::FPLK_AC_BACK).went_down()) {
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
        gpg_multiplayer_.DisconnectAll();
#endif  // PIE_NOON_USES_GOOGLE_PLAY_GAMES
        gui_menu_.Setup(config.msx_screen_buttons(), &matman_);
      } else {
        UpdateMultiscreenMenuIcons();
        return HandleMenuButtons(time);
      }
    }
    default:
      assert(false);
  }
  return state_;
}

void PieNoonGame::InitCountdownImage(int seconds) {
  join_animation_start_time_ = CurrentWorldTime(input_);
  join_id_ = ButtonId_Undefined;
  countdown_start_button_ = (ButtonId)(ButtonId_Counter5 - seconds + 1);
  if (countdown_start_button_ > ButtonId_Counter5)
    countdown_start_button_ = ButtonId_Counter5;

  for (int b = ButtonId_Counter1; b <= ButtonId_Counter5; b++) {
    StaticImage* image = gui_menu_.FindImageById((ButtonId)b);
    if (image != nullptr) {
      image->set_is_visible(true);
      if (b < countdown_start_button_) {
        // Start with this button already hidden!
        if (image != nullptr) {
          image->set_scale(mathfu::kZeros2f);
        }
      }
    }
  }
}

void PieNoonGame::UpdateCountdownImage(WorldTime time) {
  // Count down by deactivating pies images.
  const ButtonId id = CurrentlyAnimatingJoinImage(time);
  if (id < ButtonId_Counter1 && id != ButtonId_Undefined) {
    // don't actually animate anything if we are before the first button
    return;
  }
  // We've moved to animating a new pie.
  if (id != join_id_) {
    // Vanish the previous pie.
    StaticImage* prev_image = gui_menu_.FindImageById(join_id_);
    if (prev_image != nullptr) {
      prev_image->set_scale(mathfu::kZeros2f);
    }
    const Config& config = GetConfig();

    // Reset the motivator animation, if we've moved to a new image.
    motive::OvershootInit init;
    motive::OvershootInitFromFlatBuffers(*config.join_motivator_def(), &init);
    const motive::MotiveTarget1f t(motive::CurrentToTarget1f(
        config.join_motivator_start_value(),
        config.join_motivator_start_velocity(),
        config.join_motivator_target_value(), 0.0f, 1));
    join_motivator_.InitializeWithTarget(init, &game_state_.engine(), t);
    join_id_ = id;

    // Play a sound to aid with the countdown feeling.
    audio_engine_.PlaySound("StartMatch");
  }

  // Scale the pie to show some pleasing movement.
  StaticImage* image = gui_menu_.FindImageById(id);
  if (image != nullptr) {
    image->set_scale(vec2(join_motivator_.Value()));
  }
}

void PieNoonGame::TransitionToPieNoonState(PieNoonState next_state) {
  assert(state_ != next_state);  // Must actually transition.
  const Config& config = GetConfig();

  if (next_state == kPaused) {
    audio_engine_.Pause(true);
  } else if (state_ == kPaused) {
    audio_engine_.Pause(false);
  }

  switch (next_state) {
    case kLoadingInitialMaterials: {
      break;
    }
    case kLoading: {
      break;
    }
    case kJoining: {
      if (!game_state_.is_multiscreen()) {
        gui_menu_.Setup(config.join_screen_buttons(), &matman_);
        InitCountdownImage(config.join_number_of_pies());
      }

      game_state_.EnterJoiningMode();
      break;
    }
    case kPlaying: {
      if (game_state_.is_multiscreen() && multiplayer_director_ != nullptr) {
        gui_menu_.Setup(config.multiplayer_host(), &matman_);
      } else {
        gui_menu_.Setup(touch_controller_->character_id() == kNoCharacter
                            ? nullptr
                            : config.touchscreen_zones(),
                        &matman_);
      }

      if (state_ != kPaused) {
        if (ambience_channel_.Valid()) {
          ambience_channel_.Stop();
          ambience_channel_.Clear();
        }
        if (music_channel_.Valid()) {
          music_channel_.Stop();
          music_channel_.Clear();
        }
        audio_engine_.PlaySound("StartMatch");
        music_channel_ = audio_engine_.PlaySound("MusicAction");
        ambience_channel_ = audio_engine_.PlaySound("Ambience");
        game_state_.Reset(GameState::kTrackAnalytics);
      }
      break;
    }
    case kPaused: {
      gui_menu_.Setup(config.pause_screen_buttons(), &matman_);
      break;
    }
    case kMultiplayerWaiting: {
      if (game_state_.is_multiscreen() && multiplayer_director_ != nullptr) {
        multiplayer_director_->EndGame();
      }
      if (ambience_channel_.Valid()) {
        ambience_channel_.Stop();
      }
      stinger_channel_ = pindrop::Channel(nullptr);
      break;
    }
    case kFinished: {
      if (state_ == kTutorial && game_state_.is_multiscreen()) {
        // If we're in the multiscreen tutorial, go back to the multiscreen
        // menu.
        gui_menu_.Setup(config.msx_screen_buttons(), &matman_);
      } else if (game_state_.is_in_cardboard()) {
        gui_menu_.Setup(config.cardboard_screen_buttons(), &matman_);
      } else {
        gui_menu_.Setup(TitleScreenButtons(config), &matman_);
      }
      if (ambience_channel_.Valid()) {
        ambience_channel_.Stop();
        ambience_channel_.Clear();
      }
      if (music_channel_.Valid()) {
        music_channel_.Stop();
        music_channel_.Clear();
      }
      stinger_channel_.Clear();
      music_channel_ = audio_engine_.PlaySound("MusicMenu");
      for (flatbuffers::uoffset_t i = 0; i < game_state_.characters().size();
           ++i) {
        auto& character = game_state_.characters()[i];
        if (character->controller()->controller_type() != Controller::kTypeAI) {
          // Assign characters AI characters while the menu is up.
          // Players will have to press A again to get themselves re-assigned.
          // Find unused AI character:
          for (auto it = active_controllers_.begin();
               it != active_controllers_.end(); ++it) {
            if ((*it)->controller_type() == Controller::kTypeAI &&
                (*it)->character_id() == kNoCharacter) {
              character->controller()->set_character_id(kNoCharacter);
              character->set_controller(it->get());
              (*it)->set_character_id(i);
              break;
            }
          }
          // There are as many AI controllers as there are players, so this
          // should never fail:
          assert(character->controller()->controller_type() ==
                 Controller::kTypeAI);
        }
      }
      // This should only happen if we just finished a game, not if we
      // end up in this state after loading.
      if (state_ == kPlaying) {
        UploadEvents();
      }
      break;
    }
    case kTutorial: {
      tutorial_slide_index_ = 0;
      tutorial_slides_ = game_state_.is_multiscreen()
                             ? GetConfig().multiscreen_tutorial_slides()
                             : fplbase::IsTvDevice()
                                   ? GetConfig().gamepad_tutorial_slides()
                                   : GetConfig().tutorial_slides();
      tutorial_aspect_ratio_ =
          game_state_.is_multiscreen()
              ? GetConfig().multiscreen_tutorial_aspect_ratio()
              : GetConfig().tutorial_aspect_ratio();

      LoadInitialTutorialSlides();
      break;
    }
    case kMultiscreenClient: {
      if (music_channel_.Valid() && music_channel_.Playing()) {
        music_channel_.Stop();
        music_channel_.Clear();
      }
      break;
    }
    default:
      assert(false);
  }

  state_ = next_state;
  state_entry_time_ = prev_world_time_;
}

// Update the current game state and perform a state transition if requested.
PieNoonState PieNoonGame::UpdatePieNoonStateAndTransition() {
  const PieNoonState next_state = UpdatePieNoonState();
  if (next_state != state_) {
    TransitionToPieNoonState(next_state);
  }
  return next_state;
}

// Queue up a transition to the specified game state with a full screen fade
// between the states.
void PieNoonGame::FadeToPieNoonState(PieNoonState next_state,
                                     const WorldTime& fade_time,
                                     const mathfu::vec4& color,
                                     const bool fade_in) {
  if (!Fading()) {
    full_screen_fader_.Start(CurrentWorldTime(input_), fade_time, color,
                             fade_in);
    fade_exit_state_ = next_state;
  }
}

void PieNoonGame::UploadEvents() {
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
  // Now upload all stats:
  // TODO: this assumes player 0 == the logged in player.
  Character* character = game_state_.characters()[0].get();
  for (int ps = kWins; ps < kMaxStats; ps++) {
    gpg_manager.IncrementEvent(
        gpg_ids[ps].event.c_str(),
        character->GetStat(static_cast<PlayerStats>(ps)));
  }
  character->ResetStats();
#endif
}

// Called every frame to see if we've gotten any new achievements.
void PieNoonGame::CheckForNewAchievements() {
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
  // We're assuming that player 0 is the one who's stats we care about.
  Character* character = game_state_.characters()[0].get();
  if (character->State() == StateId_Throwing &&
      character->state_last_update() != StateId_Throwing) {
    for (size_t i = 0; i < achievement_ids.size(); i++) {
      gpg_manager.IncrementAchievement(achievement_ids[i].c_str());
    }
  }
#endif
}

void PieNoonGame::UploadAndShowLeaderboards() {
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
  gpg_manager.ShowLeaderboards(gpg_ids,
                               sizeof(gpg_ids) / sizeof(GPGManager::GPGIds));
#endif
}

void PieNoonGame::UpdateGamepadControllers() {
#ifdef ANDROID_GAMEPAD
  // Iterate over list of currently known gamepads.
  for (auto it = input_.GamepadMap().begin(); it != input_.GamepadMap().end();
       ++it) {
    int device_id = it->first;
    // if we find one that doesn't have a player associated with it...
    if (gamepad_to_controller_map_.find(device_id) ==
        gamepad_to_controller_map_.end()) {
      GamepadController* controller = new GamepadController();
      controller->Initialize(&input_, device_id);
      gamepad_to_controller_map_[device_id] = AddController(controller);
    }
  }
#endif  // ANDROID_GAMEPAD
}

// Returns the characterId of the first AI player we can find.
// Returns kNoCharacter if none were found.
CharacterId PieNoonGame::FindAiPlayer() {
  for (CharacterId char_id = 0;
       char_id < static_cast<CharacterId>(game_state_.characters().size());
       char_id++) {
    if (game_state_.characters()[char_id]->controller()->controller_type() ==
        Controller::kTypeAI) {
      return char_id;
    }
  }
  return kNoCharacter;
}

// Add a new controller into the list of known active controllers and assign
// an ID to it.
ControllerId PieNoonGame::AddController(Controller* new_controller) {
  for (ControllerId new_id = 0;
       new_id < static_cast<ControllerId>(active_controllers_.size());
       new_id++) {
    if (active_controllers_[new_id].get() == nullptr) {
      active_controllers_[new_id] = std::unique_ptr<Controller>(new_controller);
      return new_id;
    }
  }
  active_controllers_.push_back(std::unique_ptr<Controller>(new_controller));
  return static_cast<int>(active_controllers_.size()) - 1;
}

// Returns a controller as specified by its ID
Controller* PieNoonGame::GetController(ControllerId id) {
  return (id >= 0 && id < static_cast<ControllerId>(active_controllers_.size()))
             ? active_controllers_[id].get()
             : nullptr;
}

void PieNoonGame::HandlePlayersJoining(Controller* controller) {
  if (controller == nullptr || controller->character_id() != kNoCharacter ||
      controller->controller_type() == Controller::kTypeAI)
    return;

  CharacterId open_slot = FindAiPlayer();
  if (open_slot == kNoCharacter) return;

  auto character = game_state_.characters()[open_slot].get();
  character->controller()->set_character_id(kNoCharacter);
  character->set_controller(controller);
  controller->set_character_id(open_slot);
  character->set_just_joined_game(true);
}

void PieNoonGame::HandlePlayersJoining() {
  for (auto it = active_controllers_.begin(); it != active_controllers_.end();
       ++it) {
    // Any input on a controller signals that the controller wants to join in.
    Controller* controller = it->get();
    const bool has_input = controller != nullptr &&
                           (controller->went_up() || controller->went_down());
    if (has_input) {
      HandlePlayersJoining(controller);
    }
  }
}

void PieNoonGame::AttachMultiplayerControllers() {
  fplbase::LogInfo(fplbase::kApplication, "AttachMultiplayerControllers");
  for (auto it = active_controllers_.begin(); it != active_controllers_.end();
       ++it) {
    if (it->get()->controller_type() == Controller::kTypeMultiplayer) {
      HandlePlayersJoining(it->get());
    }
  }
}

#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES

void PieNoonGame::ProcessMultiplayerMessages() {
  while (gpg_multiplayer_.HasMessage()) {
    std::pair<std::string, std::vector<uint8_t>> msg_info =
        gpg_multiplayer_.GetNextMessage();
    std::string sender = msg_info.first;
    if (!msg_info.second.empty()) {
      // Verify the message contents are trustworthy.
      flatbuffers::Verifier verifier(msg_info.second.data(),
                                     msg_info.second.size());

      const multiplayer::MessageRoot* message =
          multiplayer::GetMessageRoot(msg_info.second.data());

      // Make sure the message has valid data.
      if (multiplayer::VerifyMessageRootBuffer(verifier)) {
        if (message->data_type() == multiplayer::Data_PlayerAssignment) {
          const multiplayer::PlayerAssignment* player_assignment =
              (const multiplayer::PlayerAssignment*)message->data();
          fplbase::LogInfo(fplbase::kApplication,
                           "Process a player assignment: %d\n",
                  player_assignment->player_id());
          StartMultiscreenGameAsClient(
              (CharacterId)player_assignment->player_id());
        } else if (message->data_type() == multiplayer::Data_PlayerCommand) {
          const multiplayer::PlayerCommand* player_command =
              (const multiplayer::PlayerCommand*)message->data();
          // process a player command
          if (game_state_.is_multiscreen() &&
              multiplayer_director_ != nullptr) {
            int player_id =
                gpg_multiplayer_.GetPlayerNumberByInstanceId(sender);
            if (player_id >= 0) {
              multiplayer_director_->InputPlayerCommand(player_id,
                                                        *player_command);
            }
          }
        } else if (message->data_type() == multiplayer::Data_StartTurn) {
          const multiplayer::StartTurn* start_turn =
              (const multiplayer::StartTurn*)message->data();
          fplbase::LogInfo(fplbase::kApplication,
                           "Multiplayer message: StartTurn.");
          multiscreen_turn_number_++;
          // start the countdown for another turn
          multiscreen_turn_end_time_ =
              CurrentWorldTime(input_) +
              start_turn->seconds() * kMillisecondsPerSecond;

          ProcessPlayerStatusMessage(*start_turn->player_status());

#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
          SendMultiscreenPlayerCommand();
#endif
          // Reload the current menu to reset all the buttons.
          ReloadMultiscreenMenu();
          UpdateMultiscreenMenuIcons();
          InitCountdownImage(start_turn->seconds());

        } else if (message->data_type() == multiplayer::Data_EndGame) {
          const multiplayer::EndGame* end_game =
              (const multiplayer::EndGame*)message->data();
          fplbase::LogInfo(fplbase::kApplication,
                           "Multiplayer message: EndGame.");
          ProcessPlayerStatusMessage(*end_game->player_status());
          // The game is over, go to the wait screen.
          TransitionToPieNoonState(kMultiplayerWaiting);
        } else if (message->data_type() == multiplayer::Data_PlayerStatus) {
          const multiplayer::PlayerStatus* player_status =
              (const multiplayer::PlayerStatus*)message->data();
          ProcessPlayerStatusMessage(*player_status);
        } else {
          fplbase::LogError(fplbase::kApplication,
                   "Multiplayer message has a data type of NONE.");
        }
      } else {
        fplbase::LogError(fplbase::kApplication, "Got a malformed multiplayer message!");
      }
    }
  }

  // If any players were disconnected and have reconnected, re-send them
  // their player number.
  while (gpg_multiplayer_.HasReconnectedPlayer()) {
    int player = gpg_multiplayer_.GetReconnectedPlayer();
    fplbase::LogInfo(
        fplbase::kApplication,
        "Got reconnected player %d (instance %s), send his assignment again.",
        player);
    auto instance_id = gpg_multiplayer_.GetInstanceIdByPlayerNumber(player);
    if (instance_id != "") {
      fplbase::LogInfo(
          fplbase::kApplication,
          "Got reconnected player %d (instance %s), send his assignment again.",
          player, instance_id.c_str());
      multiplayer_director_->SendPlayerAssignmentMsg(instance_id, player);
      SendTrackerEvent(kCategoryMultiscreen, kActionStart, kLabelReconnection);
    }
  }
}

void PieNoonGame::ProcessPlayerStatusMessage(
    const multiplayer::PlayerStatus& status) {
  // Iterate through characters and player healths.
  auto c = game_state_.characters().begin();
  auto h = status.player_health()->begin();
  for (; c != game_state_.characters().end() &&
         h != status.player_health()->end();
       ++c, ++h) {
    (*c)->set_health(*h);
  }
  unsigned char splats;
  if (multiscreen_my_player_id_ >=
          static_cast<int>(status.player_splats()->Length()) ||
      game_state_.characters()[multiscreen_my_player_id_]->health() <= 0) {
    // we're an invalid player (or a dead one), don't show our splats.
    splats = 0;
  } else {
    splats = status.player_splats()->Get(multiscreen_my_player_id_);
  }

  int new_splats = 0;
  for (int i = 0; i < GetConfig().multiscreen_options()->max_players(); i++) {
    if (splats & (1 << i)) {
      // splat i is active
      if (ShowMultiscreenSplat(i)) {
        new_splats++;
      }
    } else {
      // splat i is inactive
      auto splat =
          gui_menu_.FindImageById((ButtonId)(ButtonId_Multiplayer_Splat1 + i));
      if (splat != nullptr) {
        splat->set_is_visible(false);
      }
    }
  }
  if (new_splats > 0) {
    // play a sound effect for the new splat(s) we got
    audio_engine_.PlaySound("HitWithLargePie");
  }
}
#endif  // PIE_NOON_USES_GOOGLE_PLAY_GAMES

bool PieNoonGame::ShowMultiscreenSplat(int splat_num) {
  auto splat = gui_menu_.FindImageById(
      (ButtonId)(ButtonId_Multiplayer_Splat1 + splat_num));
  auto button = gui_menu_.FindButtonById(
      (ButtonId)(ButtonId_Multiplayer_Button1 + splat_num));
  if (splat != nullptr && button != nullptr) {
    if (!splat->is_visible()) {
      splat->set_texture_position(
          LoadVec2(button->button_def()->texture_position()));
      splat->set_color(mathfu::kOnes4f);
      splat->set_scale(LoadVec2(splat->image_def()->draw_scale()) *
                       GetConfig().multiscreen_options()->splat_start_scale());
      splat->set_is_visible(true);
      return true;  // we displayed a new splat
    }
  }
  return false;  // no new splat displayed
}

// static
// Load a string array from Android resources.
void PieNoonGame::StringArrayResource(const char* resource_name,
                                      std::vector<std::string>* strings) {
  strings->empty();

#ifdef __ANDROID__
  JNIEnv* env = reinterpret_cast<JNIEnv*>(fplbase::AndroidGetJNIEnv());
  jobject activity = reinterpret_cast<jobject>(fplbase::AndroidGetActivity());
  jclass fpl_class = env->GetObjectClass(activity);
  jstring resource_name_java = env->NewStringUTF(resource_name);

  // Get ids of the class methods.
  jmethodID len_string_array_resource = env->GetMethodID(
      fpl_class, "LenStringArrayResource", "(Ljava/lang/String;)I");
  jmethodID get_string_array_resource =
      env->GetMethodID(fpl_class, "GetStringArrayResource",
                       "(Ljava/lang/String;I)Ljava/lang/String;");

  // For now, we read each of the strings with a different JNI call.
  // TODO: Compact these into one function that returns an array of strings.
  int len_array = env->CallIntMethod(activity, len_string_array_resource,
                                     resource_name_java);
  strings->reserve(len_array);
  for (int i = 0; i < len_array; ++i) {
    jobject string_java_obj = env->CallObjectMethod(
        activity, get_string_array_resource, resource_name_java, i);
    jstring string_java = static_cast<jstring>(string_java_obj);
    const char* string_c = env->GetStringUTFChars(string_java, NULL);
    strings->push_back(std::string(string_c));
    env->ReleaseStringUTFChars(string_java, string_c);
    env->DeleteLocalRef(string_java_obj);
  }

  env->DeleteLocalRef(resource_name_java);
  env->DeleteLocalRef(fpl_class);
  env->DeleteLocalRef(activity);
#else
  (void)resource_name;
#endif
}

static void DisplayDialogBox(const char* title, const char* text_file_name,
                             bool html) {
#ifdef __ANDROID__
  std::string dialog_text;
  if (!fplbase::LoadFile(text_file_name, &dialog_text)) {
    fplbase::LogError(fplbase::kError, "can't load %s", text_file_name);
    return;
  }
  JNIEnv* env = reinterpret_cast<JNIEnv*>(fplbase::AndroidGetJNIEnv());
  jobject activity = reinterpret_cast<jobject>(fplbase::AndroidGetActivity());
  jclass fpl_class = env->GetObjectClass(activity);
  jmethodID is_text_dialog_open =
      env->GetMethodID(fpl_class, "isTextDialogOpen", "()Z");
  jboolean open = env->CallBooleanMethod(activity, is_text_dialog_open);
  if (!open) {
    jmethodID show_text_dialog =
        env->GetMethodID(fpl_class, "showTextDialog",
                         "(Ljava/lang/String;Ljava/lang/String;Z)V");
    jstring titlej = env->NewStringUTF(title);
    jstring dialog_textj = env->NewStringUTF(dialog_text.c_str());
    env->CallVoidMethod(activity, show_text_dialog, titlej, dialog_textj, html);
    env->DeleteLocalRef(dialog_textj);
    env->DeleteLocalRef(titlej);
  }
  env->DeleteLocalRef(fpl_class);
  env->DeleteLocalRef(activity);
#else
  (void)title;
  (void)text_file_name;
  (void)html;
#endif
}

PieNoonState PieNoonGame::HandleMenuButtons(WorldTime time) {
  const ButtonId previous_focus = gui_menu_.GetFocus();
  for (flatbuffers::uoffset_t i = 0; i < active_controllers_.size(); i++) {
    Controller* controller = active_controllers_[i].get();
    if (controller != nullptr &&
        controller->controller_type() != Controller::kTypeAI) {
      gui_menu_.HandleControllerInput(controller->went_down(), i);
    }
  }
  if (previous_focus != gui_menu_.GetFocus()) {
    audio_engine_.PlaySound("FocusMenuItem");
  }

  for (MenuSelection menu_selection = gui_menu_.GetRecentSelection();
       menu_selection.button_id != ButtonId_Undefined;
       menu_selection = gui_menu_.GetRecentSelection()) {
    switch (menu_selection.button_id) {
      case ButtonId_MenuSignIn: {
        fplbase::LogInfo(fplbase::kApplication, "Menu: SIGN IN/OUT");
        bool signed_in = false;
        audio_engine_.PlaySound("JoinMatch");
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
        signed_in = gpg_manager.LoggedIn();
        gpg_manager.ToggleSignIn();
#endif
        SendTrackerEvent(kCategoryUi, kActionClickedButton,
                         kLabelSignInOutButton, signed_in);
        break;
      }
      case ButtonId_MenuLicense:
        SendTrackerEvent(kCategoryUi, kActionClickedButton,
                         kLabelLicenseButton);
        audio_engine_.PlaySound("JoinMatch");
        DisplayDialogBox("Open Source Licenses", "licenses.txt", false);
        break;
      case ButtonId_MenuAbout:
        SendTrackerEvent(kCategoryUi, kActionClickedButton, kLabelAboutButton);
        audio_engine_.PlaySound("JoinMatch");
        DisplayDialogBox("About", "about.html", true);
        break;
      case ButtonId_MenuStart:
        fplbase::LogInfo(fplbase::kApplication, "Menu: START pressed");
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
        if (state_ == kMultiplayerWaiting) {
          if (gpg_multiplayer_.is_hosting() &&
              gpg_multiplayer_.GetNumConnectedPlayers() >= 1) {
            // We have at least one player, let's start the game.
            fplbase::LogInfo(fplbase::kApplication, "Multiplayer start button");
            StartMultiscreenGameAsHost();
            AttachMultiplayerControllers();

            SendTrackerEvent(kCategoryUi, kActionClickedButton,
                             kLabelStartButton, 1);
            return kPlaying;
          }
        }
#endif

        if (state_ == kFinished) {
          game_state_.set_is_multiscreen(false);
          SendTrackerEvent(kCategoryUi, kActionClickedButton,
                           kLabelStartButton);
          audio_engine_.PlaySound("JoinMatch");
          if (game_state_.is_in_cardboard()) {
            // If we are currently in the cardboard device, we assume
            // that it will be the controller running the game
            HandlePlayersJoining(cardboard_controller_);
            return kPlaying;
          } else if (menu_selection.controller_id == kTouchController) {
            // When a touch controller exists, we assume it is the unique
            // input system for the game. We make the touch controller join the
            // game, and then start the game immediately.
            HandlePlayersJoining(touch_controller_);
            return kPlaying;
          }
          return kJoining;
        }
        break;
      case ButtonId_MenuResume:
        SendTrackerEvent(kCategoryUi, kActionClickedButton, kLabelUnpauseButton,
                         pause_time_ - time);
        if (state_ == kPaused) {
          return kPlaying;
        }
        break;
      case ButtonId_MenuAchievements:
        SendTrackerEvent(kCategoryUi, kActionClickedButton,
                         kLabelAchievementsButton);
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
        gpg_manager.ShowAchievements();
#endif
        break;
      case ButtonId_InvalidInput:
        audio_engine_.PlaySound("InvalidInput");
        break;
      case ButtonId_MenuExtras: {
        SendTrackerEvent(kCategoryUi, kActionClickedButton, kLabelExtrasButton);
        game_state_.set_is_multiscreen(false);
        const Config& config = GetConfig();
        gui_menu_.Setup(config.extras_screen_buttons(), &matman_);
        break;
      }
      case ButtonId_MenuGameModes: {
        SendTrackerEvent(kCategoryUi, kActionClickedButton,
                         kLabelGameModesButton);
        const Config& config = GetConfig();
        gui_menu_.Setup(config.game_modes_screen_buttons(), &matman_);
        break;
      }
      case ButtonId_MenuMultiScreen: {
        game_state_.set_is_multiscreen(true);
        SendTrackerEvent(kCategoryUi, kActionClickedButton,
                         kLabelMultiscreenButton);
        const Config& config = GetConfig();
        gui_menu_.Setup(config.msx_screen_buttons(), &matman_);

        break;
      }
      case ButtonId_MenuMultiScreenJoin: {
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
        const Config& config = GetConfig();
        if (config.multiscreen_options()->use_full_name_as_instance_name() &&
            gpg_manager.player_data() != nullptr) {
          gpg_multiplayer_.set_my_instance_name(
              gpg_manager.player_data()->Name());
        }
        gpg_multiplayer_.set_auto_connect(
            GetConfig().multiscreen_options()->auto_connect_on_client());
        SendTrackerEvent(kCategoryMultiscreen, kActionStart, kLabelDiscovery);
        gpg_multiplayer_.StartDiscovery();
        TransitionToPieNoonState(kMultiplayerWaiting);
        gui_menu_.Setup(config.msx_searching_screen_buttons(), &matman_);
#endif
        break;
      }
      case ButtonId_MenuMultiScreenHost: {
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
        const Config& config = GetConfig();
        if (gpg_manager.player_data() != nullptr) {
          if (config.multiscreen_options()->use_full_name_as_instance_name() &&
              gpg_manager.player_data() != nullptr) {
            gpg_multiplayer_.set_my_instance_name(
                gpg_manager.player_data()->Name());
          }
        }
        gpg_multiplayer_.set_auto_connect(
            GetConfig().multiscreen_options()->auto_connect_on_host());
        SendTrackerEvent(kCategoryMultiscreen, kActionStart, kLabelAdvertising);
        gpg_multiplayer_.StartAdvertising();
        TransitionToPieNoonState(kMultiplayerWaiting);
        gui_menu_.Setup(config.msx_waitingforplayers_screen_buttons(),
                        &matman_);
        SetupWaitingForPlayersMenu();
#endif
        break;
      }
      case ButtonId_MenuCardboard: {
        SendTrackerEvent(kCategoryUi, kActionClickedButton,
                         kLabelCardboardButton);
#ifdef ANDROID_HMD
        game_state_.set_is_in_cardboard(true);
        game_state_.Reset();
        input_.head_mounted_display_input().ResetHeadTracker();
        TransitionToPieNoonState(kFinished);
        const Config& config = GetConfig();
        gui_menu_.Setup(config.cardboard_screen_buttons(), &matman_);
#endif  // ANDROID_HMD
        break;
      }

#if defined(__ANDROID__)
      case ButtonId_Sushi: {
        JNIEnv* env = reinterpret_cast<JNIEnv*>(fplbase::AndroidGetJNIEnv());
        jobject activity = reinterpret_cast<jobject>(fplbase::AndroidGetActivity());
        jclass activity_class = env->GetObjectClass(activity);
        jmethodID launch_zooshi =
            env->GetMethodID(activity_class, "LaunchZooshiSanta", "()V");
        env->CallVoidMethod(activity, launch_zooshi);
        env->DeleteLocalRef(activity_class);
        env->DeleteLocalRef(activity);
        break;
      }
#endif  // defined(__ANDROID__)

      case ButtonId_MenuBack: {
        const Config& config = GetConfig();
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
        gpg_multiplayer_.ResetToIdle();
#endif  // PIE_NOON_USES_GOOGLE_PLAY_GAMES
        SendTrackerEvent(kCategoryUi, kActionClickedButton,
                         kLabelExtrasBackButton, game_state_.is_multiscreen());
        UpdateControllers(0);  // clear went_down()
        if (state_ == kMultiplayerWaiting) TransitionToPieNoonState(kFinished);
        if (game_state_.is_in_cardboard()) {
          game_state_.set_is_in_cardboard(false);
          game_state_.Reset();
          TransitionToPieNoonState(kFinished);
        }
        gui_menu_.Setup(TitleScreenButtons(config), &matman_);
        break;
      }
      case ButtonId_MenuHowToPlay:
        SendTrackerEvent(kCategoryUi, kActionClickedButton,
                         kLabelHowToPlayButton, game_state_.is_multiscreen());
        tutorial_slide_time_ = time;
        return kTutorial;
      case ButtonId_MenuLeaderboard:
        SendTrackerEvent(kCategoryUi, kActionClickedButton,
                         kLabelLeaderboardButton);
        UploadAndShowLeaderboards();
        break;
      case ButtonId_Multiplayer_Button1:
      case ButtonId_Multiplayer_Button2:
      case ButtonId_Multiplayer_Button3:
      case ButtonId_Multiplayer_Button4: {
        // Make sure we are during a turn, otherwise you can't toggle.
        int button_num = static_cast<int>(menu_selection.button_id) -
                         ButtonId_Multiplayer_Button1;
        if (button_num == multiscreen_my_player_id_) {
          // Toggle the action
          if (multiscreen_action_to_perform_ == ButtonId_Attack)
            multiscreen_action_to_perform_ = ButtonId_Defend;
          else if (multiscreen_action_to_perform_ == ButtonId_Defend)
            multiscreen_action_to_perform_ = ButtonId_Cancel;
          else  // is ButtonId_Cancel
            multiscreen_action_to_perform_ = ButtonId_Attack;
        } else {
          multiscreen_action_aim_at_ = button_num;
        }
        if (multiscreen_turn_end_time_ > CurrentWorldTime(input_)) {
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
          SendMultiscreenPlayerCommand();
#endif
        }
        UpdateMultiscreenMenuIcons();
        break;
      }

      default:
        break;
    }
  }
  return state_;
}

#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES

void PieNoonGame::StartMultiscreenGameAsHost() {
  fplbase::LogInfo(fplbase::kApplication,
                   "Multiplayer StartMultiscreenGameAsHost");
  gpg_multiplayer_.StopAdvertising();
  int connected_players = gpg_multiplayer_.GetNumConnectedPlayers();
  // send each player their player ID and start the game
  for (int i = 0; i < connected_players; i++) {
    const auto& instance_id = gpg_multiplayer_.GetInstanceIdByPlayerNumber(i);
    fplbase::LogInfo(fplbase::kApplication,
                     "Multiplayer Send assignment %d to instance %s", i,
            instance_id.c_str());
    multiplayer_director_->SendPlayerAssignmentMsg(instance_id, i);
  }
  // If we have less than the max number of players, set the rest to AI.
  multiplayer_director_->set_num_ai_players(
      GetConfig().multiscreen_options()->max_players() - connected_players);
  game_state_.Reset(GameState::kNoAnalytics);
  multiplayer_director_->StartGame();
  TransitionToPieNoonState(kJoining);
  SendTrackerEvent(kCategoryMultiscreen, kActionStart, kLabelGameHost,
                   connected_players);
}

void PieNoonGame::StartMultiscreenGameAsClient(CharacterId id) {
  fplbase::LogInfo(fplbase::kApplication,
                   "Multiplayer StartMultiscreenGameAsClient");
  // Set up the menu screen.
  gui_menu_.Setup(GetConfig().multiplayer_client(), &matman_);
  game_state_.Reset(GameState::kNoAnalytics);
  int num_players = GetConfig().character_count();
  // Set multiplayer_action_button to the correct button ID, and color-code the
  // other buttons to correspond to the players.
  multiscreen_my_player_id_ = id;
  multiscreen_action_to_perform_ = ButtonId_Cancel;
  multiscreen_action_aim_at_ = (id + 1) % num_players;
  multiscreen_turn_number_ = 0;
  multiscreen_turn_end_time_ = 0;
  SendMultiscreenPlayerCommand();
  UpdateMultiscreenMenuIcons();
  TransitionToPieNoonState(kMultiscreenClient);
  SendTrackerEvent(kCategoryMultiscreen, kActionStart, kLabelGameClient);
}

void PieNoonGame::SendMultiscreenPlayerCommand() {
  flatbuffers::FlatBufferBuilder builder;
  auto message_root = multiplayer::CreateMessageRoot(
      builder, multiplayer::Data_PlayerCommand,
      multiplayer::CreatePlayerCommand(
          builder, multiscreen_action_aim_at_,
          (multiscreen_action_to_perform_ == ButtonId_Attack),
          (multiscreen_action_to_perform_ == ButtonId_Defend))
          .Union());

  builder.Finish(message_root);

  const multiplayer::MessageRoot* msgtest =
      multiplayer::GetMessageRoot(builder.GetBufferPointer());
  fplbase::LogInfo(fplbase::kApplication, "SendMessage data type of %d",
                   msgtest->data_type());

  std::vector<uint8_t> message(builder.GetBufferPointer(),
                               builder.GetBufferPointer() + builder.GetSize());
  gpg_multiplayer_.BroadcastMessage(message, true);
}

#endif  // PIE_NOON_USES_GOOGLE_PLAY_GAMES

void PieNoonGame::ReloadMultiscreenMenu() {
  if (gui_menu_.menu_def() == GetConfig().multiplayer_client()) {
    // Generally, we just need to reload the current menu, but just in
    // case we currently have "splats" displayed on screen, we need to preserve
    // their position, alpha, and scale (but only if they are visible right
    // now).
    struct SavedSplatState {
      ButtonId button_id;
      mathfu::vec2_packed position;
      mathfu::vec2_packed scale;
      mathfu::vec4_packed color;
      SavedSplatState(ButtonId b, mathfu::vec2 pos, mathfu::vec2 sc,
                      const mathfu::vec4& col)
          : button_id(b), position(pos), scale(sc), color(col) {}
    };
    std::vector<SavedSplatState> states;
    // Save any visible splats.
    for (int i = 0; i < GetConfig().multiscreen_options()->max_players(); i++) {
      ButtonId b = (ButtonId)(ButtonId_Multiplayer_Splat1 + i);
      auto splat = gui_menu_.FindImageById(b);
      if (splat->is_visible()) {
        states.push_back(SavedSplatState(b, splat->texture_position(),
                                         splat->scale(), splat->color()));
      }
    }
    // Now reload the menu.
    gui_menu_.Setup(gui_menu_.menu_def(), &matman_);
    // Now restore the saved state for any visible splats.
    for (auto iter = states.begin(); iter != states.end(); ++iter) {
      const SavedSplatState& state = *iter;
      auto splat = gui_menu_.FindImageById(state.button_id);
      if (splat != nullptr) {
        splat->set_is_visible(true);
        splat->set_texture_position(vec2(state.position));
        splat->set_scale(vec2(state.scale));
        splat->set_color(vec4(state.color));
      }
    }
  }
}

void PieNoonGame::UpdateMultiscreenMenuIcons() {
  int num_players = GetConfig().character_count();
  int replace_button =
      (ButtonId)(ButtonId_Multiplayer_Button1 + multiscreen_my_player_id_);
  bool i_am_dead =
      (game_state_.characters()[multiscreen_my_player_id_]->health() <= 0);
  bool is_in_turn = multiscreen_turn_end_time_ != 0 &&
                    CurrentWorldTime(input_) <= multiscreen_turn_end_time_;
  bool turn_is_soon =
      !is_in_turn &&
      (CurrentWorldTime(input_) - multiscreen_turn_end_time_ > 2000);
  for (int i = 0; i < num_players; i++) {
    ButtonId b = (ButtonId)(ButtonId_Multiplayer_Button1 + i);
    auto button = gui_menu_.FindButtonById(b);
    auto image =
        gui_menu_.FindImageById((ButtonId)(ButtonId_Multiplayer_Label1 + i));
    if (button != nullptr) {
      if (b == replace_button) {
        // Replace the button to show the current command
        const int material_throw = 4;
        const int material_wait = 2;
        const int material_block = 3;
        const int material_dead = 5;

        const int label_wait = 0;
        const int label_block = 1;
        const int label_throw = 2;
        if (game_state_.characters()[i]->health() <= 0) {
          button->set_current_up_material(material_dead);
          if (image != nullptr) {
            image->set_is_visible(false);
          }
        } else if (multiscreen_action_to_perform_ == ButtonId_Attack) {
          button->set_current_up_material(material_throw);
          if (image != nullptr) {
            image->set_is_visible(true);
            image->set_current_material_index(label_throw);
          }
        } else if (multiscreen_action_to_perform_ == ButtonId_Cancel) {
          button->set_current_up_material(material_wait);
          if (image != nullptr) {
            image->set_is_visible(true);
            image->set_current_material_index(label_wait);
          }
        } else if (multiscreen_action_to_perform_ == ButtonId_Defend) {
          if (image != nullptr) {
            image->set_is_visible(true);
            image->set_current_material_index(label_block);
          }
          button->set_current_up_material(material_block);
        }
      } else {
        // Show the other player's face, tinted, either alive or dead.
        const int material_alive = 0;
        const int material_koed = 1;
        if (game_state_.characters()[i]->health() > 0)
          button->set_current_up_material(material_alive);
        else
          button->set_current_up_material(material_koed);
        button->set_color(game_state_.characters()[i]->ButtonColor());

        if (image != nullptr) image->set_is_visible(false);
      }
      if (is_in_turn && game_state_.characters()[i]->health() > 0 &&
          !i_am_dead) {
        button->set_is_active(true);
      } else {
        button->set_is_active(false);
        if (image != nullptr) image->set_is_visible(false);
      }

      // Finally, if there is a splat on screen in slot N, disable button N.
      auto splat =
          gui_menu_.FindImageById((ButtonId)(ButtonId_Multiplayer_Splat1 + i));
      if (splat != nullptr && splat->is_visible()) {
        if (button != nullptr) button->set_is_active(false);
        if (image != nullptr) image->set_is_visible(false);
      }
    }
  }

  gui_menu_.SetFocus(
      (ButtonId)(ButtonId_Multiplayer_Button1 + multiscreen_action_aim_at_));

  auto go = gui_menu_.FindImageById(ButtonId_Multiplayer_Go);
  auto look = gui_menu_.FindImageById(ButtonId_Multiplayer_Look);
  if (turn_is_soon || i_am_dead) {
    if (go != nullptr) go->set_is_visible(false);
    if (look != nullptr) look->set_is_visible(false);
  } else if (is_in_turn && multiscreen_turn_number_ >= 1) {
    if (go != nullptr) go->set_is_visible(true);
    if (look != nullptr) look->set_is_visible(false);
  } else {
    if (go != nullptr) go->set_is_visible(false);
    if (look != nullptr) look->set_is_visible(true);
  }
  if (multiscreen_turn_number_ == 0 || i_am_dead) {
    auto img = gui_menu_.FindImageById(ButtonId_Counter1);
    if (img != nullptr) img->set_is_visible(false);
    img = gui_menu_.FindImageById(ButtonId_Counter2);
    if (img != nullptr) img->set_is_visible(false);
    img = gui_menu_.FindImageById(ButtonId_Counter3);
    if (img != nullptr) img->set_is_visible(false);
    img = gui_menu_.FindImageById(ButtonId_Counter4);
    if (img != nullptr) img->set_is_visible(false);
    img = gui_menu_.FindImageById(ButtonId_Counter5);
    if (img != nullptr) img->set_is_visible(false);
  }
}

void PieNoonGame::SetupWaitingForPlayersMenu() {
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
  auto players = gui_menu_.FindImageById(ButtonId_Multiplayer_NumPlayers);
  int num_players = gpg_multiplayer_.GetNumConnectedPlayers();
  if (players != nullptr && num_players >= 0 && num_players <= 4) {
    players->set_current_material_index(num_players);
  }

  auto button = gui_menu_.FindButtonById(ButtonId_MenuStart);
  if (num_players == 0) {
    button->set_is_active(false);
  } else {
    button->set_is_active(true);
  }
#endif
}

// Call AdvanceFrame on every controller that we're listening to
// and care about.  (Not all are connected to players, but we want
// to keep them up to date so we can check their inputs as needed.)
void PieNoonGame::UpdateControllers(WorldTime delta_time) {
  for (size_t i = 0; i < active_controllers_.size(); i++) {
    if (active_controllers_[i].get() != nullptr) {
      active_controllers_[i]->AdvanceFrame(delta_time);
    }
  }
}

void PieNoonGame::UpdateTouchButtons(WorldTime delta_time) {
  gui_menu_.AdvanceFrame(delta_time, &input_, vec2(renderer_.window_size()));
}

pindrop::Channel PieNoonGame::PlayStinger() {
  auto& characters = game_state_.characters();
  int player_winners = 0;
  int ai_winners = 0;
  for (size_t i = 0; i < characters.size(); ++i) {
    auto& character = characters[i];
    if (character->victory_state() == kVictorious) {
      if (character->controller()->controller_type() == Controller::kTypeAI) {
        ++ai_winners;
      } else {
        ++player_winners;
      }
    }
  }
  // If there's a single human winner, play the victory stinger.
  // If no humans won, play the lose stinger.
  // If more than one character won, or if they all lost, play the draw stinger.
  // This logic should work equally well for all game modes.
  if (player_winners == 1 && ai_winners == 0) {
    return audio_engine_.PlaySound("StingerWin");
  } else if (player_winners == 0 && ai_winners > 0) {
    return audio_engine_.PlaySound("StingerLose");
  } else {
    return audio_engine_.PlaySound("StingerDraw");
  }
}

// Return the file name for the material at slide_index. If slide_index is
// invalid, return nullptr.
const char* PieNoonGame::TutorialSlideName(int slide_index) {
  const int num_slides = static_cast<int>(tutorial_slides_->size());
  return (slide_index < 0 || slide_index >= num_slides)
             ? nullptr
             : tutorial_slides_->Get(slide_index)->image()->c_str();
}

static bool ControllerHasPress(const Controller* controller) {
  return controller != nullptr &&
         controller->controller_type() != Controller::kTypeAI &&
         controller->went_down();
}

// Return true if a button press or touch screen touch has happened this frame.
bool PieNoonGame::AnyControllerPresses() {
  for (auto it = active_controllers_.begin(); it != active_controllers_.end();
       ++it) {
    const Controller* controller = it->get();
    if (ControllerHasPress(controller)) return true;
  }
  return input_.GetPointerButton(static_cast<fplbase::FingerId>(
                                   fplbase::K_POINTER1)).went_down();
}

// Load into memory the tutorial slide at slide_index, if slide_index is valid.
// We preload some tutorial slides so that we can transition to them.
void PieNoonGame::LoadTutorialSlide(int slide_index) {
  const int num_slides = static_cast<int>(tutorial_slides_->size());
  if (slide_index < 0 || slide_index >= num_slides) return;

  const char* slide_name = TutorialSlideName(slide_index);
  matman_.LoadMaterial(slide_name);
}

// Preload the initial few tutorial slides to prime the slide load-unload
// pipeline.
void PieNoonGame::LoadInitialTutorialSlides() {
  const Config& config = GetConfig();
  const int num_to_load =
      static_cast<int>(config.tutorial_num_future_slides_to_load());
  for (int slide_index = 0; slide_index < num_to_load; ++slide_index) {
    LoadTutorialSlide(slide_index);
  }
}

// Scale material by (aspect_ratio, 1) and then scale again so that it covers as
// much of the screen as possible.
void PieNoonGame::RenderInMiddleOfScreen(const mat4& ortho_mat,
                                         float aspect_ratio,
                                         fplbase::Material* material) {
  // Calculate the texture scale. We want to fill the screen as much as we can,
  // but not change the aspect ratio. That means we letterbox either
  // horizontally or vertically.
  const vec2 window_size = vec2(renderer_.window_size());
  const vec2 texture_size =
      vec2(material->textures()[0]->size()) * vec2(aspect_ratio, 1.0f);
  const vec2 scale_xy = window_size / texture_size;
  const float scale = std::min(scale_xy.x(), scale_xy.y());

  // Calculate the extreme corners of the texture, in 3D.
  const vec2 mid = 0.5f * window_size;
  const vec2 tex = 0.5f * scale * texture_size;
  const vec3 bottom_left(mid.x() - tex.x(), mid.y() + tex.y(), 0.0f);
  const vec3 top_right(mid.x() + tex.x(), mid.y() - tex.y(), 0.0f);

  // Render the texture to take up the whole screen.
  renderer_.set_model_view_projection(ortho_mat);
  renderer_.set_color(mathfu::kOnes4f);
  material->Set(renderer_);
  shader_textured_->Set(renderer_);
  fplbase::Mesh::RenderAAQuadAlongX(bottom_left, top_right, vec2(0, 1),
                                    vec2(1, 0));
}

bool PieNoonGame::ShouldTransitionFromSlide(WorldTime world_time) {
  // Never transition while the fader is active.
  if (!full_screen_fader_.Finished(world_time)) return false;

  // Don't transition when already passed the number of slides.
  const int num_slides = static_cast<int>(tutorial_slides_->size());
  if (tutorial_slide_index_ >= num_slides) return false;

  // Always transition on controller presses.
  if (AnyControllerPresses()) return true;

  // If no display time is specified for this slide, don't transition
  // automatically.
  const Slide* slide = tutorial_slides_->Get(tutorial_slide_index_);
  const WorldTime display_time = slide->display_time();
  if (display_time == 0) return false;

  // If we're exceeded our wait time, transition.
  const WorldTime time_in_slide = world_time - tutorial_slide_time_;
  return time_in_slide >= display_time;
}

void PieNoonGame::Run() {
  // Initialize so that we don't sleep the first time through the loop.
  const Config& config = GetConfig();
  const WorldTime min_update_time = config.min_update_time();
  const WorldTime max_update_time = config.max_update_time();
  prev_world_time_ = CurrentWorldTime(input_) - min_update_time;
  TransitionToPieNoonState(kLoadingInitialMaterials);
  game_state_.Reset(GameState::kNoAnalytics);

  while (!input_.exit_requested() &&
         !input_.GetButton(fplbase::FPLK_ESCAPE).went_down()) {
    // Process input device messages since the last game loop.
    // Update render window size.
    input_.AdvanceFrame(&renderer_.window_size());

    // Milliseconds elapsed since last update. To avoid burning through the
    // CPU, enforce a minimum time between updates. For example, if
    // min_update_time is 1, we will not exceed 1000Hz update time.
    const WorldTime world_time = CurrentWorldTime(input_);
    const WorldTime delta_time =
        std::min(world_time - prev_world_time_, max_update_time);
    if (delta_time < min_update_time) {
      input_.Delay((min_update_time - delta_time) / 1000.0);
      continue;
    }

    // TODO: Can we move these to 'Render'?
    renderer_.AdvanceFrame(input_.minimized(), input_.Time());
    renderer_.ClearFrameBuffer(mathfu::kZeros4f);

    UpdateGamepadControllers();
    UpdateControllers(delta_time);
    UpdateTouchButtons(delta_time);

    // Update the full screen fader dimensions.
    const auto res = renderer_.window_size();
    const auto ortho_mat = mathfu::OrthoHelper<float>(
        0.0f, static_cast<float>(res.x()), static_cast<float>(res.y()), 0.0f,
        -1.0f, 1.0f);
    full_screen_fader_.set_ortho_mat(ortho_mat);
    full_screen_fader_.set_extents(res);

#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
    gpg_multiplayer_.Update();
#endif

    // If we're all done loading, run & render the game as usual.
    switch (state_) {
      case kJoining:
      case kPlaying:
      case kPaused:
      case kMultiplayerWaiting:
      case kMultiscreenClient:
      case kFinished: {
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
        if (state_ == kMultiplayerWaiting) {
          if (!gpg_multiplayer_.is_hosting()) {
            // Show the correct "Joining" screen.
            if (gpg_multiplayer_.state() == GPGMultiplayer::kDiscovering &&
                gui_menu_.menu_def() != config.msx_searching_screen_buttons()) {
              gui_menu_.Setup(config.msx_searching_screen_buttons(), &matman_);
            } else if (gpg_multiplayer_.state() ==
                           GPGMultiplayer::kDiscoveringPromptedUser &&
                       gui_menu_.menu_def() !=
                           config.msx_pleasewait_screen_buttons()) {
              gui_menu_.Setup(config.msx_pleasewait_screen_buttons(), &matman_);
            } else if (gpg_multiplayer_.state() ==
                           GPGMultiplayer::kDiscoveringWaitingForHost &&
                       gui_menu_.menu_def() !=
                           config.msx_connecting_screen_buttons()) {
              gui_menu_.Setup(config.msx_connecting_screen_buttons(), &matman_);
            } else if (gpg_multiplayer_.state() == GPGMultiplayer::kConnected &&
                       !gpg_multiplayer_.is_hosting() &&
                       gui_menu_.menu_def() !=
                           config.msx_waitingforgame_screen_buttons()) {
              gui_menu_.Setup(config.msx_waitingforgame_screen_buttons(),
                              &matman_);
            } else if (gpg_multiplayer_.state() == GPGMultiplayer::kIdle) {
              // TODO(jsimantov): show a connection error
            }
          } else {
            // Show the correct "Hosting" screen.
            if (gui_menu_.menu_def() !=
                config.msx_waitingforplayers_screen_buttons()) {
              gui_menu_.Setup(config.msx_waitingforplayers_screen_buttons(),
                              &matman_);
            }
            SetupWaitingForPlayersMenu();
          }
        }

        ProcessMultiplayerMessages();
        if (game_state_.is_multiscreen() && multiplayer_director_ != nullptr &&
            state_ == kPlaying) {
          multiplayer_director_->AdvanceFrame(delta_time);
          bool show_look = (multiplayer_director_->start_turn_timer() < 1000 &&
                            (multiplayer_director_->turn_timer() == 0 ||
                             multiplayer_director_->turn_timer() > 2000));
          if (game_state_.IsGameOver()) show_look = false;

          if (gui_menu_.menu_def() == config.multiplayer_host()) {
            auto go = gui_menu_.FindImageById(ButtonId_Multiplayer_Go);
            auto look = gui_menu_.FindImageById(ButtonId_Multiplayer_Look);
            if (show_look) {
              if (go != nullptr) go->set_is_visible(true);
              if (look != nullptr) look->set_is_visible(true);
            } else {
              if (go != nullptr) go->set_is_visible(false);
              if (look != nullptr) look->set_is_visible(false);
            }
          } else {
            // restore the buttons to visible, if they exist
            auto go = gui_menu_.FindImageById(ButtonId_Multiplayer_Go);
            auto look = gui_menu_.FindImageById(ButtonId_Multiplayer_Look);
            if (look != nullptr) look->set_is_visible(true);
            if (go != nullptr) go->set_is_visible(true);
          }

          if (!gpg_multiplayer_.IsConnected()) {
            if (gpg_multiplayer_.HasError()) {
              gpg_multiplayer_.ResetToIdle();
              TransitionToPieNoonState(kFinished);
              // Show "connection error" screen.
              gui_menu_.Setup(config.msx_connection_lost_screen_buttons(),
                              &matman_);
              SendTrackerEvent(kCategoryMultiscreen, kActionError,
                               kLabelConnectionLost, 1);
            } else {
              gpg_multiplayer_.ResetToIdle();
              TransitionToPieNoonState(kFinished);
              // Show "all players disconnected" screen.
              SendTrackerEvent(kCategoryMultiscreen, kActionError,
                               kLabelClientsDisconnected);
              gui_menu_.Setup(
                  config.msx_all_players_disconnected_screen_buttons(),
                  &matman_);
            }
          }
        }

        if (state_ == kMultiscreenClient) {
          // do multiscreen client logic
          if (CurrentWorldTime(input_) <= multiscreen_turn_end_time_) {
            // We are during a turn, update timer and splats.
            UpdateCountdownImage(CurrentWorldTime(input_));
            // Update splat alpha fade-out. Fades out during turn.
            // Get the fraction of the turn we are through. For example if we
            // are 2 seconds into a 5-second turn, we are 0.4 through the turn.
            float turn_duration =
                multiscreen_turn_end_time_ - join_animation_start_time_;
            if (turn_duration == 0) turn_duration = kMillisecondsPerSecond;
            float turn_progress =
                (CurrentWorldTime(input_) - join_animation_start_time_) /
                turn_duration;
            if (turn_progress < 0) turn_progress = 0;
            if (turn_progress > 1) turn_progress = 1;

            for (int i = 0; i < config.multiscreen_options()->max_players();
                 i++) {
              auto splat = gui_menu_.FindImageById(
                  (ButtonId)(ButtonId_Multiplayer_Splat1 + i));
              if (splat != nullptr && splat->is_visible()) {
                splat->set_color(
                    mathfu::vec4(splat->color()[0], splat->color()[1],
                                 splat->color()[2], 1 - turn_progress));
              }
            }
          }

          // update any on-screen splats covering the buttons
          for (int i = 0; i < config.multiscreen_options()->max_players();
               i++) {
            auto splat = gui_menu_.FindImageById(
                (ButtonId)(ButtonId_Multiplayer_Splat1 + i));
            if (splat != nullptr && splat->is_visible()) {
              mathfu::vec2 base_scale =
                  LoadVec2(splat->image_def()->draw_scale());
              if (splat->scale()[0] > base_scale[0]) {
                mathfu::vec2 scale = splat->scale();
                scale =
                    scale * config.multiscreen_options()->splat_scale_speed();
                if (scale[0] <= base_scale[0]) {
                  splat->set_scale(base_scale);
                } else {
                  splat->set_scale(scale);
                }
                // override the splat fade fade-out with a fade-in on scale
                float scale_ratio =
                    (splat->scale()[0] - base_scale[0]) /
                    config.multiscreen_options()->splat_start_scale();
                splat->set_color(
                    mathfu::vec4(splat->color()[0], splat->color()[1],
                                 splat->color()[2], 1 - scale_ratio));
              }
              // slowly slide the splat down
              splat->set_texture_position(mathfu::vec2(
                  splat->texture_position()[0],
                  splat->texture_position()[1] +
                      config.multiscreen_options()->splat_drip_speed()));
            }
          }
          if (!gpg_multiplayer_.IsConnected()) {
            if (gui_menu_.menu_def() == config.multiplayer_client()) {
              // something caused us to become disconnected
              if (gpg_multiplayer_.HasError()) {
                gpg_multiplayer_.ResetToIdle();
                TransitionToPieNoonState(kFinished);
                // Show "connection error" screen.
                SendTrackerEvent(kCategoryMultiscreen, kActionError,
                                 kLabelConnectionLost, 0);
                gui_menu_.Setup(config.msx_connection_lost_screen_buttons(),
                                &matman_);
              } else {
                gpg_multiplayer_.ResetToIdle();
                TransitionToPieNoonState(kFinished);
                // Show "host disconnected" screen.
                SendTrackerEvent(kCategoryMultiscreen, kActionError,
                                 kLabelHostDisconnected);
                gui_menu_.Setup(config.msx_host_disconnected_screen_buttons(),
                                &matman_);
              }
            } else {
              gpg_multiplayer_.ResetToIdle();
              TransitionToPieNoonState(kFinished);
              // Show "host disconnected" screen.
              gui_menu_.Setup(TitleScreenButtons(config), &matman_);
            }
          }
        }
#endif

        if (state_ != kPaused && state_ != kMultiscreenClient) {
          // Update game logic by a variable number of milliseconds.
          game_state_.AdvanceFrame(delta_time, &audio_engine_);
        } else {
          // We are the client, we only update a few small things.
          game_state_.particle_manager().AdvanceFrame(
              static_cast<TimeStep>(delta_time));
          game_state_.engine().AdvanceFrame(delta_time);
        }

        if (state_ == kPlaying && !stinger_channel_.Valid() &&
            game_state_.IsGameOver()) {
          game_state_.DetermineWinnersAndLosers();
          stinger_channel_ = PlayStinger();
        }

        // Update audio engine state.
        audio_engine_.AdvanceFrame(world_time);

        // Issue draw calls for the 'scene'.
        if (state_ != kMultiscreenClient) {
          // Populate 'scene' from the game state--all the positions,
          // orientations, and renderable-ids (which specify materials) of the
          // characters and props. Also specify the camera matrix.
          game_state_.PopulateScene(&scene_);

          // Issue draw calls for the 'scene'.
          Render(scene_);
        } else {
          Render2DElements(scene_, mat4::Identity());
        }

        // Output debug information.
        if (config.print_character_states()) {
          DebugPrintCharacterStates();
        }
        if (config.print_pie_states()) {
          DebugPrintPieStates();
        }
        if (config.allow_camera_movement()) {
          DebugCamera();
        }

        // Remember the real-world time from this frame.
        prev_world_time_ = world_time;

        // Advance to the next play state, if required.
        UpdatePieNoonStateAndTransition();

        // For testing,
        // we'll check if a sixth finger went down on the touch screen,
        // if so we update the leaderboards and show the UI:
        if (input_.GetButton(fplbase::K_POINTER6).went_down()) {
          UploadEvents();
          // For testing, show UI:
          UploadAndShowLeaderboards();
        }
#ifdef PIE_NOON_USES_GOOGLE_PLAY_GAMES
        gpg_manager.Update();
        fplbase::SavePreference("logged_in",
                                static_cast<int>(gpg_manager.LoggedIn()));
        CheckForNewAchievements();
#endif
        break;
      }

      case kLoading: {
        // If even the loading textures haven't loaded yet, remain on a black
        // screen, otherwise render the loading texture spinning and the
        // logo below.
        // Textures are still loading. Display a loading screen.
        auto spinmat = matman_.FindMaterial(config.loading_material()->c_str());
        auto logomat = matman_.FindMaterial(config.loading_logo()->c_str());
        assert(spinmat && logomat);
        assert(spinmat->textures()[0]->id() && logomat->textures()[0]->id());
        const auto mid = res / 2;
        const float time = static_cast<float>(world_time) /
                           static_cast<float>(kMillisecondsPerSecond);
        const mat3 rot_mat = mat3::RotationZ(
            sin(time * 3.0f) *
            Angle::FromDegrees(config.loading_anim_amplitude() / 2)
                .ToRadians());
        renderer_.set_model_view_projection(
            ortho_mat *
            mat4::FromTranslationVector(
                vec3(static_cast<float>(mid.x()),
                     res.y() * config.loading_anim_vert_translation(), 0.0f)) *
            mat4::FromRotationMatrix(rot_mat));
        auto extend = vec2(spinmat->textures()[0]->size());
        renderer_.set_color(mathfu::kOnes4f);
        spinmat->Set(renderer_);
        shader_textured_->Set(renderer_);
        fplbase::Mesh::RenderAAQuadAlongX(vec3(-extend.x(), extend.y(), 0),
                                 vec3(extend.x(), -extend.y(), 0), vec2(0, 1),
                                 vec2(1, 0));

        extend = vec2(logomat->textures()[0]->size()) / 10;
        renderer_.set_model_view_projection(
            ortho_mat *
            mat4::FromTranslationVector(vec3(
                static_cast<float>(mid.x()),
                res.y() * config.loading_texture_vert_translation(), 0.0f)));
        renderer_.set_color(mathfu::kOnes4f);
        logomat->Set(renderer_);
        shader_textured_->Set(renderer_);
        fplbase::Mesh::RenderAAQuadAlongX(vec3(-extend.x(), extend.y(), 0),
                                 vec3(extend.x(), -extend.y(), 0), vec2(0, 1),
                                 vec2(1, 0));
      }  // Fallthrough

      case kLoadingInitialMaterials:
        // Finalize the materials that have been loaded thus far.
        matman_.TryFinalize();

        if (UpdatePieNoonStateAndTransition() == kFinished) {
          game_state_.Reset(GameState::kNoAnalytics);
        }
        break;

      case kTutorial: {
        matman_.TryFinalize();
        audio_engine_.TryFinalize();
        const bool should_transition = ShouldTransitionFromSlide(world_time);
        if (should_transition) {
          // Start fade-out --> fade-in transition.
          const WorldTime fade_time =
              tutorial_slides_->Get(tutorial_slide_index_)->fade_time();
          if (fade_time > 0) {
            full_screen_fader_.Start(world_time, fade_time, mathfu::kZeros4f,
                                     false);
          }

          // Initiate asynchronous loading of a slide, several slides before
          // it is needed.
          const int future_slide_index =
              tutorial_slide_index_ +
              config.tutorial_num_future_slides_to_load();
          LoadTutorialSlide(future_slide_index);
        }

        // Draw the slide covering the entire screen.
        const char* slide_name = TutorialSlideName(tutorial_slide_index_);
        if (slide_name != nullptr) {
          auto slide = matman_.FindMaterial(slide_name);
          if (slide->textures()[0]->id()) {
            RenderInMiddleOfScreen(ortho_mat, tutorial_aspect_ratio_, slide);
          }
        }

        const int num_slides = static_cast<int>(tutorial_slides_->size());
        if (tutorial_slide_index_ < num_slides) {
          // Overlay the darkening texture.
          bool advance_slide = should_transition;
          if (!full_screen_fader_.Finished(world_time)) {
            advance_slide = full_screen_fader_.Render(world_time);
          }
          if (advance_slide) {
            // Unload current slide to save memory.
            if (slide_name != nullptr) {
              matman_.UnloadMaterial(slide_name);
            }

            const unsigned int SLIDE_NUMBER_BUFFER_SIZE = 32;
            char slide_number[SLIDE_NUMBER_BUFFER_SIZE];
            snprintf(slide_number, sizeof(slide_number),
                     game_state_.is_multiscreen() ? kLabelMSSlideDurationFmt
                                                  : kLabelSlideDurationFmt,
                     tutorial_slide_index_);
            SendTrackerEvent(kCategoryUi, game_state_.is_multiscreen()
                                              ? kActionViewedMSTutorialSlide
                                              : kActionViewedTutorialSlide,
                             slide_number, world_time - tutorial_slide_time_);

            // When completely dark, transition to the next slide.
            tutorial_slide_index_++;
            tutorial_slide_time_ = world_time;
          }
        }

        UpdatePieNoonStateAndTransition();
        break;
      }

      default:
        assert(false);
    }
  }
}

#if defined(__ANDROID__)
void PieNoonGame::ParseViewIntentData(const std::string& intent_data,
                                      std::string* launch_mode,
                                      std::string* overlay) {
  static const char kDefaultLaunchMode[] = "default";
  static const char kPathPrefix[] = "http://google.github.io/pienoon/launch/";
  assert(launch_mode);
  assert(overlay);
  *launch_mode = kDefaultLaunchMode;
  *overlay = "";
  if (intent_data.empty()) return;

  LogInfo("Started with view intent %s", intent_data.c_str());
  size_t path_prefix_length = strlen(kPathPrefix);
  if (intent_data.compare(0, path_prefix_length, kPathPrefix) == 0) {
    std::string launch_arguments =
        intent_data.substr(path_prefix_length, std::string::npos);
    size_t split_pos = launch_arguments.find("/");
    if (split_pos != std::string::npos) {
      *launch_mode = launch_arguments.substr(0, split_pos);
      *overlay = launch_arguments.substr(split_pos + 1, std::string::npos);
    }
    fplbase::LogInfo(fplbase::kApplication,
                     "Detected launch URL %s (mode=%s, overlay=%s)\n",
            launch_arguments.c_str(), launch_mode->c_str(), overlay->c_str());
  }
}
#endif  // defined(__ANDROID__)

}  // pie_noon
}  // fpl
