/*******************************************************************************
 * LVGL v8 + Arduino_GFX + CHSC touch
 ******************************************************************************/

#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include "touch.h"

// ============================================================
// Touch pins
// ============================================================

#define TP_SDA       9
#define TP_SCL       46
#define TP_INT_PIN   8
#define TP_RST_PIN   3

// ============================================================
// Display
// ============================================================

#define SCREEN_WIDTH   320
#define SCREEN_HEIGHT  170

#define TFT_BL 38
#define GFX_BL DF_GFX_BL

Arduino_DataBus *bus = new Arduino_ESP32SPI(
  11,                  // DC
  10,                  // CS
  12,                  // SCK
  13,                  // MOSI
  GFX_NOT_DEFINED      // MISO
);

Arduino_GFX *gfx = new Arduino_GC9A01(
  bus,
  1,       // RST
  1,       // rotation
  true,    // IPS
  170,     // width
  320,     // height
  35,      // col offset 1
  0,       // row offset 1
  35,      // col offset 2
  0        // row offset 2
);

// ============================================================
// LVGL variables
// ============================================================

static uint32_t screenWidth;
static uint32_t screenHeight;

lv_disp_draw_buf_t draw_buf;

lv_color_t *disp_draw_buf1;
lv_color_t *disp_draw_buf2;

lv_disp_drv_t disp_drv;
lv_event_code_t code;

static uint16_t x = 0;
static uint16_t y = 0;
static uint8_t ret = 0;

SemaphoreHandle_t xMutex;

// ============================================================
// Display flush
// ============================================================

void my_disp_flush(
  lv_disp_drv_t *disp,
  const lv_area_t *area,
  lv_color_t *color_p
) {

  if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(10)) == pdTRUE) {

    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

#if (LV_COLOR_16_SWAP != 0)

    gfx->draw16bitBeRGBBitmap(
      area->x1,
      area->y1,
      (uint16_t *)&color_p->full,
      w,
      h
    );

#else

    gfx->draw16bitRGBBitmap(
      area->x1,
      area->y1,
      (uint16_t *)&color_p->full,
      w,
      h
    );

#endif

    lv_disp_flush_ready(disp);

    xSemaphoreGive(xMutex);

  } else {

    Serial.println("Display mutex failed");

  }
}

// ============================================================
// Touch input
// ============================================================

void my_touchpad_read(
  lv_indev_drv_t *indev_driver,
  lv_indev_data_t *data
) {

  if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(10)) == pdTRUE) {

    ret = CHSC6413_Scan(&x, &y);

    // --------------------------------------------------------
    // Touch detected
    // --------------------------------------------------------

    if (ret == 1) {

      data->state = LV_INDEV_STATE_PR;

      data->point.x = x;
      data->point.y = y;

      Serial.print("TOUCH X=");
      Serial.print(x);
      Serial.print(" Y=");
      Serial.println(y);

    }

    // --------------------------------------------------------
    // No touch
    // --------------------------------------------------------

    else {

      data->state = LV_INDEV_STATE_REL;

    }

    xSemaphoreGive(xMutex);

  } else {

    Serial.println("Touch mutex failed");

  }
}

// ============================================================
// BandFlow task UI
// ============================================================

enum AppScreen {
  SCREEN_TASKS,
  SCREEN_MATRIX,
  SCREEN_DETAILS,
  SCREEN_ACTIVE,
  SCREEN_COMPLETE
};

static AppScreen appScreen = SCREEN_TASKS;
static lv_obj_t *uiRoot;
static lv_timer_t *completeTimer;
static lv_point_t completeCheckPoints[] = { { 10, 24 }, { 20, 34 }, { 38, 14 } };

static const lv_color_t COLOR_BG = lv_color_hex(0x11194A);
static const lv_color_t COLOR_PANEL = lv_color_hex(0x2A225E);
static const lv_color_t COLOR_PANEL_DARK = lv_color_hex(0x211B50);
static const lv_color_t COLOR_PURPLE = lv_color_hex(0x7650F5);
static const lv_color_t COLOR_BLUE = lv_color_hex(0x4F6CF6);
static const lv_color_t COLOR_GREEN = lv_color_hex(0x32D98A);
static const lv_color_t COLOR_TEXT = lv_color_hex(0xF5F3FF);
static const lv_color_t COLOR_MUTED = lv_color_hex(0x9A95C1);

static lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, lv_coord_t xPos,
                           lv_coord_t yPos, lv_coord_t width, lv_coord_t height,
                           lv_color_t color, lv_coord_t fontSize) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_pos(label, xPos, yPos);
  lv_obj_set_size(label, width, height);
  lv_obj_set_style_text_color(label, color, 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  return label;
}

static lv_obj_t *makeButton(lv_obj_t *parent, const char *text, lv_coord_t xPos,
                            lv_coord_t yPos, lv_coord_t width, lv_coord_t height,
                            lv_color_t color, lv_event_cb_t callback) {
  lv_obj_t *button = lv_btn_create(parent);
  lv_obj_set_pos(button, xPos, yPos);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_bg_color(button, color, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
  lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
  makeLabel(button, text, 0, 0, width, height, COLOR_TEXT, 12);
  lv_obj_set_style_text_align(lv_obj_get_child(button, 0), LV_TEXT_ALIGN_CENTER, 0);
  if (callback != NULL) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
  return button;
}

static void clearUi(void) {
  if (completeTimer != NULL) {
    lv_timer_del(completeTimer);
    completeTimer = NULL;
  }
  if (uiRoot != NULL) lv_obj_del(uiRoot);
  lv_obj_set_style_radius(lv_scr_act(), 0, 0);
  uiRoot = lv_obj_create(lv_scr_act());
  lv_obj_set_size(uiRoot, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_pos(uiRoot, 0, 0);
  lv_obj_set_style_bg_color(uiRoot, COLOR_BG, 0);
  lv_obj_set_style_bg_opa(uiRoot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(uiRoot, 0, 0);
  lv_obj_set_style_radius(uiRoot, 0, 0);
  lv_obj_set_style_pad_all(uiRoot, 0, 0);
  lv_obj_clear_flag(uiRoot, LV_OBJ_FLAG_SCROLLABLE);
}

static void goTo(AppScreen nextScreen);

static void onTaskClick(lv_event_t *) { goTo(SCREEN_DETAILS); }
static void onMatrixClick(lv_event_t *) { goTo(SCREEN_MATRIX); }
static void onStartClick(lv_event_t *) { goTo(SCREEN_ACTIVE); }
static void onNoClick(lv_event_t *) { goTo(SCREEN_DETAILS); }
static void onYesClick(lv_event_t *) { goTo(SCREEN_COMPLETE); }
static void onCompleteTap(lv_event_t *) { goTo(SCREEN_TASKS); }

static void returnToTasks(lv_timer_t *) { goTo(SCREEN_TASKS); }

static void drawHeader(const char *rightText, lv_event_cb_t rightCallback) {
  makeLabel(uiRoot, "BandFlow", 12, 5, 105, 18, COLOR_TEXT, 16);
  lv_obj_t *right = makeButton(uiRoot, rightText, 224, 5, 82, 20, COLOR_PURPLE, rightCallback);
  lv_obj_set_style_radius(right, 10, LV_PART_MAIN);
}

static void drawTasks(void) {
  drawHeader("3 TASKS", onMatrixClick);
  makeLabel(uiRoot, "13:17", 210, 5, 40, 18, COLOR_MUTED, 12);

  const char *names[] = { "API Integration", "Design Review", "Write Tests" };
  const char *values[] = { "85%", "30%", "10%" };
  const lv_color_t dots[] = { lv_color_hex(0xFF665E), lv_color_hex(0xFFD43B), COLOR_GREEN };
  for (int i = 0; i < 3; i++) {
    lv_coord_t top = 29 + (i * 34);
    lv_obj_t *card = lv_obj_create(uiRoot);
    lv_obj_set_pos(card, 8, top);
    lv_obj_set_size(card, 304, 29);
    lv_obj_set_style_bg_color(card, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x514582), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, onTaskClick, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dot = lv_obj_create(card);
    lv_obj_set_pos(dot, 8, 9); lv_obj_set_size(dot, 7, 7);
    lv_obj_set_style_bg_color(dot, dots[i], 0); lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    makeLabel(card, names[i], 21, 3, 200, 14, COLOR_TEXT, 12);
    makeLabel(card, values[i], 265, 3, 32, 14, COLOR_MUTED, 12);
    lv_obj_t *bar = lv_obj_create(card);
    lv_obj_set_pos(bar, 21, 20); lv_obj_set_size(bar, 275, 3);
    lv_obj_set_style_bg_color(bar, i == 0 ? COLOR_PURPLE : lv_color_hex(0x8D80D8), 0);
    lv_obj_set_style_radius(bar, 2, 0); lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_width(bar, i == 0 ? 165 : (i == 1 ? 58 : 20));
  }
}

static void drawMatrix(void) {
  lv_obj_t *back = makeButton(uiRoot, "TASKS", 224, 5, 82, 20, COLOR_PURPLE,
                              [] (lv_event_t *) { goTo(SCREEN_TASKS); });
  lv_obj_set_style_radius(back, 10, LV_PART_MAIN);
  makeLabel(uiRoot, "Priority Matrix", 14, 5, 130, 18, COLOR_TEXT, 12);
  makeLabel(uiRoot, "Tap to start", 238, 23, 65, 12, COLOR_MUTED, 12);
  const char *titles[] = { "DO FIRST", "SCHEDULE", "DELEGATE", "DROP" };
  const char *items[] = { "API Integration", "Design Review", "Write Tests", "Meeting Notes" };
  const lv_color_t colors[] = { lv_color_hex(0xF0524D), COLOR_PURPLE, lv_color_hex(0xFFD43B), COLOR_MUTED };
  for (int i = 0; i < 4; i++) {
    lv_coord_t col = i % 2, row = i / 2;
    lv_obj_t *cell = lv_obj_create(uiRoot);
    lv_obj_set_pos(cell, 13 + col * 150, 29 + row * 45); lv_obj_set_size(cell, 136, 40);
    lv_obj_set_style_bg_color(cell, COLOR_PANEL, 0); lv_obj_set_style_radius(cell, 8, 0);
    lv_obj_set_style_border_color(cell, colors[i], 0); lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_pad_all(cell, 0, 0); lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    if (i < 3) lv_obj_add_event_cb(cell, onTaskClick, LV_EVENT_CLICKED, NULL);
    makeLabel(cell, titles[i], 7, 2, 120, 16, colors[i], 12);
    makeLabel(cell, items[i], 7, 17, 125, 21, COLOR_TEXT, 12);
  }
  makeLabel(uiRoot, "< Low urgency", 14, 151, 110, 17, COLOR_MUTED, 12);
  makeLabel(uiRoot, "High urgency >", 218, 151, 88, 17, COLOR_MUTED, 12);
}

static void drawDetails(void) {
  lv_obj_t *count = makeButton(uiRoot, "3 SUBTASKS", 224, 5, 82, 20, COLOR_PURPLE, NULL);
  lv_obj_set_style_radius(count, 10, LV_PART_MAIN);
  makeLabel(uiRoot, "API Integration", 13, 7, 180, 18, COLOR_TEXT, 16);
  makeLabel(uiRoot, "AI", 15, 29, 42, 17, COLOR_PURPLE, 12);
  const char *items[] = { "Set up OAuth endpoints", "Token refresh logic", "Error handling + retry" };
  for (int i = 0; i < 3; i++) {
    lv_obj_t *check = lv_checkbox_create(uiRoot);
    lv_checkbox_set_text(check, items[i]); lv_obj_set_pos(check, 13, 48 + i * 20);
    lv_obj_set_style_text_color(check, i == 0 ? COLOR_MUTED : COLOR_TEXT, 0);
    lv_obj_set_style_text_font(check, &lv_font_montserrat_14, 0);
    if (i == 0) lv_obj_add_state(check, LV_STATE_CHECKED);
  }
  makeButton(uiRoot, "START TASK", 13, 124, 269, 27, COLOR_PURPLE, onStartClick);
}

static void drawActive(void) {
  makeLabel(uiRoot, "ACTIVE", 0, 13, 320, 12, COLOR_MUTED, 12);
  lv_obj_set_style_text_align(lv_obj_get_child(uiRoot, 0), LV_TEXT_ALIGN_CENTER, 0);
  makeLabel(uiRoot, "API Integration", 0, 27, 320, 18, COLOR_TEXT, 16);
  makeLabel(uiRoot, "00:02", 0, 50, 320, 22, COLOR_PURPLE, 16);
  makeLabel(uiRoot, "Is current work finished?", 0, 83, 320, 14, COLOR_MUTED, 12);
  makeButton(uiRoot, "No", 13, 108, 131, 37, COLOR_PANEL, onNoClick);
  makeButton(uiRoot, "Yes", 151, 108, 129, 37, COLOR_PURPLE, onYesClick);
}

static void drawComplete(void) {
  lv_obj_t *circle = lv_obj_create(uiRoot);
  lv_obj_set_pos(circle, 128, 35); lv_obj_set_size(circle, 48, 48);
  lv_obj_set_style_bg_color(circle, COLOR_GREEN, 0); lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(circle, 0, 0);
  lv_obj_t *check = lv_line_create(circle);
  lv_line_set_points(check, completeCheckPoints, 3);
  lv_obj_set_size(check, 48, 48);
  lv_obj_set_style_line_color(check, COLOR_TEXT, LV_PART_MAIN);
  lv_obj_set_style_line_width(check, 3, LV_PART_MAIN);
  lv_obj_set_style_line_rounded(check, true, LV_PART_MAIN);
  lv_obj_t *title = makeLabel(uiRoot, "Task Complete!", 0, 95, 320, 18, COLOR_TEXT, 16);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t *sub = makeLabel(uiRoot, "API Integration", 0, 116, 320, 14, COLOR_MUTED, 12);
  lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t *hint = makeLabel(uiRoot, "Returning to task list...", 0, 140, 320, 14, COLOR_MUTED, 12);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_add_event_cb(uiRoot, onCompleteTap, LV_EVENT_CLICKED, NULL);
  completeTimer = lv_timer_create(returnToTasks, 1800, NULL);
  lv_timer_set_repeat_count(completeTimer, 1);
}

static void goTo(AppScreen nextScreen) {
  appScreen = nextScreen;
  clearUi();
  if (appScreen == SCREEN_TASKS) drawTasks();
  else if (appScreen == SCREEN_MATRIX) drawMatrix();
  else if (appScreen == SCREEN_DETAILS) drawDetails();
  else if (appScreen == SCREEN_ACTIVE) drawActive();
  else drawComplete();
}

void lv_example_btn_1(void) { goTo(SCREEN_TASKS); }

// ============================================================
// LVGL task
// ============================================================

void lvglTask(void *pvParameters) {

  while (1) {

    lv_tick_inc(8);
    lv_timer_handler();

    vTaskDelay(
      pdMS_TO_TICKS(8)
    );
  }
}

// ============================================================
// Setup
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(2000);

  Serial.println();
  Serial.println("================================");
  Serial.println("LVGL + TOUCH TEST");
  Serial.println("================================");

  // ----------------------------------------------------------
  // Display initialization
  // ----------------------------------------------------------

  Serial.println("Initializing display...");

  gfx->begin(80000000);

  gfx->invertDisplay(true);

#ifdef TFT_BL

  pinMode(
    TFT_BL,
    OUTPUT
  );

  digitalWrite(
    TFT_BL,
    HIGH
  );

#endif

  Serial.println("Display initialized.");

  // ----------------------------------------------------------
  // Touch initialization
  // ----------------------------------------------------------

  Serial.println("Initializing touch...");

  CHSC6413_init();

  Serial.println("Touch screen initialized.");

  // ----------------------------------------------------------
  // LVGL initialization
  // ----------------------------------------------------------

  lv_init();

  delay(10);

  screenWidth = gfx->width();
  screenHeight = gfx->height();

  Serial.print("Screen width: ");
  Serial.println(screenWidth);

  Serial.print("Screen height: ");
  Serial.println(screenHeight);

  // ----------------------------------------------------------
  // Allocate LVGL buffers
  // ----------------------------------------------------------

  disp_draw_buf1 =
    (lv_color_t *)heap_caps_malloc(
      sizeof(lv_color_t) *
      screenWidth *
      screenHeight / 8,
      MALLOC_CAP_INTERNAL |
      MALLOC_CAP_8BIT
    );

  disp_draw_buf2 =
    (lv_color_t *)heap_caps_malloc(
      sizeof(lv_color_t) *
      screenWidth *
      screenHeight / 8,
      MALLOC_CAP_INTERNAL |
      MALLOC_CAP_8BIT
    );

  if (!disp_draw_buf1 || !disp_draw_buf2) {

    Serial.println(
      "ERROR: LVGL draw buffer allocation failed!"
    );

    return;
  }

  Serial.println(
    "LVGL draw buffers allocated."
  );

  // ----------------------------------------------------------
  // Initialize LVGL draw buffer
  // ----------------------------------------------------------

  lv_disp_draw_buf_init(
    &draw_buf,
    disp_draw_buf1,
    disp_draw_buf2,
    screenWidth *
    screenHeight / 8
  );

  // ----------------------------------------------------------
  // Initialize display driver
  // ----------------------------------------------------------

  lv_disp_drv_init(
    &disp_drv
  );

  disp_drv.hor_res =
    screenWidth;

  disp_drv.ver_res =
    screenHeight;

  disp_drv.flush_cb =
    my_disp_flush;

  disp_drv.draw_buf =
    &draw_buf;

  lv_disp_drv_register(
    &disp_drv
  );

  Serial.println(
    "LVGL display driver registered."
  );

  // ----------------------------------------------------------
  // Initialize touch input driver
  // ----------------------------------------------------------

  static lv_indev_drv_t indev_drv;

  lv_indev_drv_init(
    &indev_drv
  );

  indev_drv.type =
    LV_INDEV_TYPE_POINTER;

  indev_drv.read_cb =
    my_touchpad_read;

  lv_indev_drv_register(
    &indev_drv
  );

  Serial.println(
    "LVGL touch driver registered."
  );

  // ----------------------------------------------------------
  // Create button
  // ----------------------------------------------------------

  lv_example_btn_1();

  Serial.println(
    "BandFlow interface created."
  );

  // ----------------------------------------------------------
  // Create mutex
  // ----------------------------------------------------------

  xMutex =
    xSemaphoreCreateMutex();

  if (xMutex == NULL) {

    Serial.println(
      "ERROR: Mutex creation failed!"
    );

    return;
  }

  Serial.println(
    "Mutex created."
  );

  // ----------------------------------------------------------
  // Start LVGL task
  // ----------------------------------------------------------

Serial.println("Starting LVGL task...");

BaseType_t result = xTaskCreatePinnedToCore(
  lvglTask,
  "LVGL Task",
  8192,
  NULL,
  1,
  NULL,
  1
);

if (result == pdPASS) {
  Serial.println("LVGL task started successfully.");
} else {
  Serial.println("ERROR: LVGL task failed to start!");
}

Serial.println("SETUP COMPLETE");
  Serial.println();
  Serial.println("================================");
  Serial.println("SETUP COMPLETE");
  Serial.println("Touch the screen now.");
  Serial.println("================================");
}

// ============================================================
// Main loop
// ============================================================

void loop() {

  // I leave the loop empty because LVGL runs in lvglTask().

} //beegii the gay