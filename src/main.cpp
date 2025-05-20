#include <climits>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#define SDL_MAIN_USE_CALLBACKS 1 /* use the callbacks instead of main() */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#define WINDOW_START_WIDTH 400
#define WINDOW_START_HEIGHT 350
static std::string ASSETS_PATH = "./assets/";

#define PIXELS_FALLEN_PER_FRAME 5
#define PIXELS_FLAPPED_MULTIPLIER 0.9
#define FRAMES_PER_FLAP 17

#define FRAMES_PER_PIPE 200
#define PIXEL_SPACE_BETWEEN_PIPES                                              \
  320 // Space between top and bottom of 1 line of pipe
#define PIXELS_MOVED_PER_FRAME_PIPES 2

/////////////////////////////////////////////////////////////////////////////
/// Structs
/////////////////////////////////////////////////////////////////////////////

struct TextureSet {
  SDL_Texture *birdTex1, *birdTex2, *pipeTex1, *pipeTex2, *cloudTex;
};

struct BirdContext {
  float width;
  float height;
  float x_loc;
  float y_loc;
  int flapTimer;
  bool isFlapping;
  bool isDead;
  SDL_Texture *curTexture;
};

struct PipeContext {
  float x_top_loc; // Center top texture of pipe
  float y_top_loc; // Center top texture of pipe
  float x_bot_loc; // Center bottom texture of pipe
  float y_bot_loc; // Center bottom texture of pipe
};

struct AppContext {
  int frameNumber;
  int score;
  SDL_Window *window;
  SDL_Renderer *renderer;
  std::unique_ptr<TextureSet> textureSet;
  SDL_AudioDeviceID audioDevice;
  SDL_AppResult app_quit = SDL_APP_CONTINUE;
  std::unique_ptr<BirdContext> bird;
  float floorHeight;
  std::unique_ptr<std::deque<std::unique_ptr<PipeContext>>> pipes;
  std::unique_ptr<std::vector<SDL_FPoint>> cloudCenters;
  Uint64 fps;
  Uint64 prevTickCount;
};

/////////////////////////////////////////////////////////////////////////////
/// Helper Functions
/////////////////////////////////////////////////////////////////////////////

SDL_AppResult SDL_Fail() {
  SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "Error %s", SDL_GetError());
  return SDL_APP_FAILURE;
}

SDL_AppResult LoadTextureFromPath(AppContext **appPtr, SDL_Texture **texture,
                                  const char *path) {
  AppContext *app = *appPtr;

  SDL_Surface *surface = IMG_Load(path);
  if (not surface) {
    return SDL_Fail();
  }

  *texture = SDL_CreateTextureFromSurface(app->renderer, surface);
  if (not texture) {
    return SDL_Fail();
  }

  SDL_DestroySurface(surface);
  return SDL_APP_CONTINUE;
}

// x and y are pixel locations the center of the texture should be at
// (with origin at upper left)
SDL_AppResult DisplayTextureAt(AppContext *app, SDL_Texture *texture, float x,
                               float y, float texture_width,
                               float texture_height, float rotation = 0.0f) {
  SDL_FRect dst_rect;
  dst_rect.x = x - texture_width / 2.0f;
  dst_rect.y = y - texture_height / 2.0f;
  dst_rect.w = (float)texture_width;
  dst_rect.h = (float)texture_height;

  SDL_FPoint center;
  center.x = texture_width / 2.0f;
  center.y = texture_height / 2.0f;

  SDL_RenderTextureRotated(app->renderer, texture, NULL, &dst_rect, rotation,
                           &center, SDL_FLIP_NONE);

  return SDL_APP_CONTINUE;
}

// x and y are pixel locations the center of the texture should be at
// (with origin at upper left)
SDL_AppResult DisplayTextureAt(AppContext *app, SDL_Texture *texture, float x,
                               float y, float rotation = 0.0f) {
  float texture_width, texture_height;
  SDL_GetTextureSize(texture, &texture_width, &texture_height);

  return DisplayTextureAt(app, texture, x, y, texture_width, texture_height,
                          rotation);
}

SDL_AppResult LoadTextures(AppContext **appPtr) {
  AppContext *app = *appPtr;

  SDL_Texture *birdTex1, *birdTex2, *pipeTex1, *pipeTex2, *cloudTex;
  LoadTextureFromPath(appPtr, &birdTex1, (ASSETS_PATH + "bird1.png").c_str());
  LoadTextureFromPath(appPtr, &birdTex2, (ASSETS_PATH + "bird2.png").c_str());
  LoadTextureFromPath(appPtr, &pipeTex1, (ASSETS_PATH + "pipe1.png").c_str());
  LoadTextureFromPath(appPtr, &pipeTex2, (ASSETS_PATH + "pipe2.png").c_str());
  LoadTextureFromPath(appPtr, &cloudTex, (ASSETS_PATH + "cloud.png").c_str());

  if (not birdTex1 || not birdTex2 || not pipeTex1 || not pipeTex2 ||
      not cloudTex) {
    return SDL_Fail();
  }

  app->textureSet = std::make_unique<TextureSet>(TextureSet{
      .birdTex1 = birdTex1,
      .birdTex2 = birdTex2,
      .pipeTex1 = pipeTex1,
      .pipeTex2 = pipeTex2,
      .cloudTex = cloudTex,
  });

  return SDL_APP_CONTINUE;
}

// Generate a pipe from the right with a random location
// Location will be generated between the top of the screen and the top of the
// floor, with a buffer the size of floor_height
PipeContext CreatePipe(float app_width, float app_height, float floor_height) {
  float locationOffset =
      (float)(rand() % ((int)app_height - 3 * (int)floor_height -
                        PIXEL_SPACE_BETWEEN_PIPES));
  PipeContext pipe = PipeContext{
      .x_top_loc = app_width,
      .y_top_loc = floor_height + locationOffset,
      .x_bot_loc = app_width,
      .y_bot_loc = floor_height + locationOffset + PIXEL_SPACE_BETWEEN_PIPES,
  };
  return pipe;
}

bool BirdPipeCollision(float pipe_x, float pipe_top, float pipe_bot,
                       BirdContext *birdCtx) {
  if (birdCtx->x_loc + birdCtx->width >= pipe_x &&
      birdCtx->x_loc - birdCtx->width <= pipe_x) {
    return birdCtx->y_loc - birdCtx->width <= pipe_top ||
           birdCtx->y_loc + birdCtx->width >= pipe_bot;
  }
  return false;
}

// Move the bird and check if it has died
SDL_AppResult UpdateBirdInfo(AppContext *app, float app_width,
                             float app_height) {
  if (app->bird->isDead) {
    return SDL_APP_CONTINUE; // Do nothing if dead
  }

  if (app->bird->isFlapping) {
    // Update flapping
    if (app->bird->flapTimer == 0) {
      app->bird->isFlapping = false;
      app->bird->curTexture = app->textureSet->birdTex2;
    } else {
      app->bird->flapTimer -= 1;
      float flappedPixels = PIXELS_FLAPPED_MULTIPLIER * app->bird->flapTimer;
      if (app->bird->y_loc > app->bird->width / 2.0f + flappedPixels)
        app->bird->y_loc -= flappedPixels;
    }
  } else {
    // Update bird location after falling
    if (app->bird->y_loc <
        app_height - app->floorHeight - app->bird->width / 2.0f) {
      app->bird->y_loc += PIXELS_FALLEN_PER_FRAME;
    } else {
      app->bird->y_loc =
          app_height - app->floorHeight - app->bird->width / 2.0f;
    }
    app->bird->x_loc = app_width / 2.0f;
  }

  // Check if bird dies after moving
  if (app->bird->y_loc ==
      app_height - app->floorHeight - app->bird->width / 2.0f) {
    // Bird has hit the ground
    app->bird->isDead = true;
  }

  for (auto &&pipeCtx : *app->pipes) {
    if (BirdPipeCollision(pipeCtx->x_bot_loc, pipeCtx->y_top_loc,
                          pipeCtx->y_bot_loc, app->bird.get())) {
      app->bird->isDead = true;
      break;
    }
  }

  return SDL_APP_CONTINUE;
}

SDL_AppResult UpdatePipeLocations(AppContext *app, float app_width,
                                  float app_height) {
  for (auto &&pipeCtx : *app->pipes) {
    pipeCtx->x_bot_loc -= PIXELS_MOVED_PER_FRAME_PIPES;
    pipeCtx->x_top_loc -= PIXELS_MOVED_PER_FRAME_PIPES;

    if (pipeCtx->x_bot_loc <= app_width / 2.0f &&
        pipeCtx->x_bot_loc > app_width / 2.0f - PIXELS_MOVED_PER_FRAME_PIPES) {
      app->score++;
    }
  }

  if (!app->pipes->empty() &&
      app->pipes->at(0)->x_top_loc < -app->bird->width) {
    app->pipes->pop_front();
  }

  return SDL_APP_CONTINUE;
}

// Generates some random non overlapping circles within the window and
// draws a cloud in the center of each one of them
//
// Clouds are only generated above 3/4 of app_width
SDL_AppResult GenerateClouds(AppContext *app, int app_width, int app_height) {
  int numClouds = 2 + (rand() % 2);
  float radiusSquared = 15.0f * app->bird->width * app->bird->width;

  // Repeat below until numClouds clouds reached
  int curCloudCount = 0;
  while (curCloudCount != numClouds) {
    SDL_FPoint potentialLoc;
    potentialLoc.x = (float)(rand() % app_width);
    potentialLoc.y = (float)(rand() % (app_height / 4 * 3));

    bool isOverlapping = false;
    for (SDL_FPoint center : *app->cloudCenters) {
      float x_diff = center.x - potentialLoc.x;
      float y_diff = center.y - potentialLoc.y;
      if (x_diff * x_diff + y_diff * y_diff < radiusSquared) {
        isOverlapping = true;
        break;
      }
    }

    if (!isOverlapping) {
      app->cloudCenters->push_back(potentialLoc);
      curCloudCount++;
    }
  }

  return SDL_APP_CONTINUE;
}

/////////////////////////////////////////////////////////////////////////////
/// Rendering Functions
/////////////////////////////////////////////////////////////////////////////

// Renders clouds
SDL_AppResult RenderBackground(AppContext *app, int app_width, int app_height) {
  for (SDL_FPoint point : *app->cloudCenters) {
    DisplayTextureAt(app, app->textureSet->cloudTex, point.x, point.y);
  }

  return SDL_APP_CONTINUE;
}

SDL_AppResult RenderFloor(AppContext *app, int app_width, int app_height) {
  SDL_FRect floorRect;
  floorRect.x = 0;
  floorRect.y = app_height - (int)app->floorHeight;
  floorRect.w = app_width;
  floorRect.h = app->floorHeight;

  SDL_SetRenderDrawColor(app->renderer, 60, 180, 100, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(app->renderer, &floorRect);

  SDL_FRect topFloorRect;
  topFloorRect.x = 0;
  topFloorRect.y = app_height - (int)app->floorHeight;
  topFloorRect.w = app_width;
  topFloorRect.h = 10;

  SDL_SetRenderDrawColor(app->renderer, 30, 45, 45, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(app->renderer, &topFloorRect);

  return SDL_APP_CONTINUE;
}

SDL_AppResult RenderBird(AppContext *app) {
  DisplayTextureAt(app, app->bird->curTexture, app->bird->x_loc,
                   app->bird->y_loc, app->bird->width, app->bird->height);
  return SDL_APP_CONTINUE;
}

SDL_AppResult RenderPipes(AppContext *app, float app_width, float app_height) {
  float pipe_tex_width, pipe_tex_height;
  SDL_GetTextureSize(app->textureSet->pipeTex1, &pipe_tex_width,
                     &pipe_tex_height);

  for (auto &&pipeCtx : *app->pipes) {
    DisplayTextureAt(app, app->textureSet->pipeTex1, pipeCtx->x_top_loc,
                     pipeCtx->y_top_loc, pipe_tex_width, pipe_tex_height,
                     180.0f); // Draw top pipe
    DisplayTextureAt(app, app->textureSet->pipeTex1, pipeCtx->x_bot_loc,
                     pipeCtx->y_bot_loc, pipe_tex_width,
                     pipe_tex_height); // Draw bottom pipe

    // Draw top pipe parts
    for (float y_top_loc = pipeCtx->y_top_loc - pipe_tex_height;
         y_top_loc > -pipe_tex_height / 2.0f; y_top_loc -= pipe_tex_height) {
      DisplayTextureAt(app, app->textureSet->pipeTex2, pipeCtx->x_top_loc,
                       y_top_loc, pipe_tex_width, pipe_tex_height, 180.0f);
    }

    // Draw bottom pipe parts
    for (float y_bot_loc = pipeCtx->y_bot_loc + pipe_tex_height;
         y_bot_loc < app_height + pipe_tex_height / 2.0f;
         y_bot_loc += pipe_tex_height) {
      DisplayTextureAt(app, app->textureSet->pipeTex2, pipeCtx->x_bot_loc,
                       y_bot_loc, pipe_tex_width, pipe_tex_height);
    }
  }
  return SDL_APP_CONTINUE;
}

/////////////////////////////////////////////////////////////////////////////
/// Initialization & Shutdown
/////////////////////////////////////////////////////////////////////////////

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
  // init the library, here we make a window so we only need the Video
  // capabilities.
  if (not SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
    return SDL_Fail();
  }

  // init TTF
  if (not TTF_Init()) {
    return SDL_Fail();
  }

  // create a window
  // SDL_WINDOW_BORDERLESS | SDL_WINDOW_TRANSPARENT for transparent
  SDL_Window *window =
      SDL_CreateWindow("birdflapgame", WINDOW_START_WIDTH, WINDOW_START_HEIGHT,
                       SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (not window) {
    return SDL_Fail();
  }

  // create a renderer
  SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
  if (not renderer) {
    return SDL_Fail();
  }

  // init SDL Mixer
  auto audioDevice =
      SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
  if (not audioDevice) {
    return SDL_Fail();
  }

  // print some information about the window
  SDL_ShowWindow(window);
  int width, height, bbwidth, bbheight;
  SDL_GetWindowSize(window, &width, &height);
  SDL_GetWindowSizeInPixels(window, &bbwidth, &bbheight);
  SDL_Log("Window size: %ix%i", width, height);
  SDL_Log("Backbuffer size: %ix%i", bbwidth, bbheight);
  if (width != bbwidth) {
    SDL_Log("This is a highdpi environment.");
  }
  // set up the application data
  *appstate = new AppContext{
      .frameNumber = 0,
      .score = 0,
      .window = window,
      .renderer = renderer,
      .textureSet = nullptr,
      .audioDevice = audioDevice,
      .bird = std::make_unique<BirdContext>(BirdContext{
          .width = -1,  // To be set later in init
          .height = -1, // To be set later in init
          .x_loc = (float)bbwidth / 2.0f,
          .y_loc = (float)bbheight / 2.0f,
          .flapTimer = 0,
          .isFlapping = false,
          .isDead = false,
          .curTexture = nullptr, // To be set later in init

      }),
      .floorHeight = std::min((float)bbheight / 20.0f, 50.0f),
      .pipes = std::make_unique<std::deque<std::unique_ptr<PipeContext>>>(),
      .cloudCenters = std::make_unique<std::vector<SDL_FPoint>>(),
      .fps = 0, // Set every 10 frames
      .prevTickCount = SDL_GetTicks(),
  };
  auto *app = (AppContext *)*appstate;

  SDL_AppResult loadTexResult = LoadTextures(&app);
  if (loadTexResult != SDL_APP_CONTINUE) {
    return SDL_Fail();
  }

  // Set default bird texture
  app->bird->curTexture = app->textureSet->birdTex2;

  // Set bird width and height
  SDL_GetTextureSize(app->bird->curTexture, &app->bird->width,
                     &app->bird->height);

  SDL_SetRenderVSync(renderer, -1);                          // enable vysnc
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND); // enable blending

  srand(time(0)); // Seed rand() for pipe generation

  GenerateClouds(app, bbwidth, bbheight);

  SDL_Log("Application started successfully!");

  return SDL_APP_CONTINUE;
}

/* This function runs once at shutdown. */
void SDL_AppQuit(void *appstate, SDL_AppResult result) {}

/////////////////////////////////////////////////////////////////////////////
/// Event handler
///
/// This function runs when a new event (mouse input, keypresses, etc) occurs
/////////////////////////////////////////////////////////////////////////////

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
  if (event->type == SDL_EVENT_QUIT) {
    return SDL_APP_SUCCESS; /* end the program, reporting success to the OS. */
  }

  auto *app = (AppContext *)appstate;
  if (event->type == SDL_EVENT_KEY_DOWN) {
    if (event->key.key == SDLK_SPACE) {
      // Set the bird to be flapping if it has not flapped recently
      if (app->bird->flapTimer <= 3 && !app->bird->isDead) {
        app->bird->isFlapping = true;
        app->bird->curTexture = app->textureSet->birdTex1;
        app->bird->flapTimer = FRAMES_PER_FLAP;
      }

    } else if (event->key.key == SDLK_ESCAPE) {
      return SDL_APP_SUCCESS; // end the program, reporting success to the OS.
    } else if (event->key.key == SDLK_R && app->bird->isDead) {
      // TODO: Restart
      // Reset bird location
      int app_width, app_height;
      SDL_GetWindowSizeInPixels(app->window, &app_width, &app_height);
      app->bird->x_loc = (float)app_width / 2.0f;
      app->bird->y_loc = (float)app_height / 2.0f;

      // Reset score
      app->score = 0;

      // Reset pipes
      app->frameNumber = 0;
      while (!app->pipes->empty()) {
        app->pipes->pop_front();
      }

      // Reset death.
      app->bird->isDead = false;
    }
  }

  return SDL_APP_CONTINUE;
}

/////////////////////////////////////////////////////////////////////////////
/// Per-Frame Callback
/////////////////////////////////////////////////////////////////////////////

SDL_AppResult SDL_AppIterate(void *appstate) {
  auto *app = (AppContext *)appstate;

  Uint64 frameStart = SDL_GetPerformanceCounter();

  int app_width, app_height;
  SDL_GetWindowSizeInPixels(app->window, &app_width, &app_height);

  SDL_SetRenderDrawColor(app->renderer, 100, 150, 230, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(app->renderer); // Clear renderer before displaying textures

  RenderBackground(app, app_width, app_height);

  RenderPipes(app, (float)app_width, (float)app_height);

  RenderFloor(app, app_width, app_height);

  RenderBird(app);

  // Display score
  std::string scoreStr = std::to_string(app->score);
  float scale = 5.0f;
  int score_x = ((app_width / scale) -
                 SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scoreStr.length()) /
                2;
  int score_y = ((app_height / scale) - SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) / 6;
  SDL_SetRenderScale(app->renderer, scale, scale);

  // Render some offset to make text more visible
  SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderDebugText(app->renderer, score_x + 1.0f, score_y + 1.0f,
                      scoreStr.c_str());

  // Render text
  SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, SDL_ALPHA_OPAQUE);
  SDL_RenderDebugText(app->renderer, score_x, score_y, scoreStr.c_str());

  SDL_SetRenderScale(app->renderer, 1.0f, 1.0f);

  if (app->bird->isDead) {
    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 180);

    SDL_FRect screenRect;
    screenRect.x = screenRect.y = 0;
    screenRect.w = app_width;
    screenRect.h = app_height;
    SDL_RenderFillRect(app->renderer, &screenRect);

    // Add some text to to cue you to reset
    std::string resetStr = "Press R to reset!";
    int resetScale = 2.0f;
    int reset_x = ((app_width / resetScale) -
                   SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * resetStr.length()) /
                  2;
    int reset_y =
        ((app_height / resetScale) - SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) / 8;
    SDL_SetRenderScale(app->renderer, resetScale, resetScale);
    SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, SDL_ALPHA_OPAQUE);
    SDL_RenderDebugText(app->renderer, reset_x, reset_y, resetStr.c_str());
    SDL_SetRenderScale(app->renderer, 1.0f, 1.0f);
  }

  SDL_RenderPresent(app->renderer);
  if (app->frameNumber == INT_MAX) {
    app->frameNumber = 0;
  } else {
    app->frameNumber++;
  }

  // Log framerate
  if (app->frameNumber % 10 == 0) {
    Uint64 curTicks = SDL_GetTicks();
    app->fps = 10000 / (curTicks - app->prevTickCount);
    SDL_Log("FPS: %llu", app->fps);
    app->prevTickCount = curTicks;
  }

  if (!app->bird->isDead) { // Update only if bird is alive
    UpdateBirdInfo(app, (float)app_width, (float)app_height);
    UpdatePipeLocations(app, (float)app_width, (float)app_height);

    if (app->frameNumber % FRAMES_PER_PIPE == 0) {
      app->pipes->push_back(std::make_unique<PipeContext>(
          CreatePipe((float)app_width, (float)app_height, app->floorHeight)));
    }
  }

  Uint64 frameEnd = SDL_GetPerformanceCounter();
  float elapsedMS =
      (frameEnd - frameStart) / (float)SDL_GetPerformanceFrequency();

  // Cap framerate to 90fps
  SDL_Delay(floor(11.1111f - elapsedMS));

  return SDL_APP_CONTINUE;
}
