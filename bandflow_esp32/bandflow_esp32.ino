/* BandFlow wristband: a touch UI in the app's worker theme, plus the BLE link to the Raspberry Pi.
 *
 * Two BLE channels share one service:
 *  - Step channel (TASK_CHAR_UUID / STATUS_CHAR_UUID): the Pi writes the current subtask, and
 *    tapping DONE notifies "DONE" so the bridge can complete it and send the next one.
 *  - Session channel (RPC_RX / RPC_TX): newline-terminated JSON. The watch asks who is linked,
 *    asks for a pairing code, and loads the matrix, works and tasks. The bridge relays each
 *    request to the BandFlow server and writes the reply back.
 * See contracts/ble-bridge-v2.md in the app repository for the message list. */

#include <NimBLEDevice.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <string>
#include "touch.h"

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 170
#define TFT_BL        38
// hello hello hello

// These match the worker screens in the app (WorkerDashboard.tsx / TaskDetail.tsx, dark theme).
#define COLOR_BG            0x07111F
#define COLOR_HERO          0x102B59
#define COLOR_HERO_BORDER   0x2D4772
#define COLOR_SURFACE       0x0B1627
#define COLOR_SURFACE_PRESS 0x16263F
#define COLOR_BORDER        0x263244
#define COLOR_TRACK         0x1E2B42
#define COLOR_TITLE         0xF4F8FF
#define COLOR_BODY          0xA7B4C9
#define COLOR_ACCENT        0x56A7FF
#define COLOR_ACCENT_TEXT   0x04111F
#define COLOR_SUCCESS       0x7CE1BB
#define COLOR_SUCCESS_PRESS 0x5CC7A0
#define COLOR_DANGER        0xFF8F8F
#define COLOR_DANGER_PRESS  0xE07575
#define COLOR_AMBER         0xFFD285
#define COLOR_DISABLED      0x1B2A40
#define COLOR_PRIORITY_HIGH 0xE84545
#define COLOR_PRIORITY_MED  0xD99324
#define COLOR_PRIORITY_LOW  0x2EAD72

// Only Montserrat 14 is guaranteed by lv_conf.h; larger sizes are used when they are enabled.
#define FONT_SMALL (&lv_font_montserrat_14)
#if LV_FONT_MONTSERRAT_16
#define FONT_STEP (&lv_font_montserrat_16)
#else
#define FONT_STEP (&lv_font_montserrat_14)
#endif
#if LV_FONT_MONTSERRAT_32
#define FONT_CODE (&lv_font_montserrat_32)
#elif LV_FONT_MONTSERRAT_28
#define FONT_CODE (&lv_font_montserrat_28)
#elif LV_FONT_MONTSERRAT_24
#define FONT_CODE (&lv_font_montserrat_24)
#elif LV_FONT_MONTSERRAT_20
#define FONT_CODE (&lv_font_montserrat_20)
#else
#define FONT_CODE FONT_STEP
#endif

// I keep these UUIDs stable so the Raspberry Pi can use the same BLE contract.
#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define TASK_CHAR_UUID       "12345678-1234-1234-1234-1234567890ac"
#define STATUS_CHAR_UUID     "12345678-1234-1234-1234-1234567890ad"
#define RPC_RX_CHAR_UUID     "12345678-1234-1234-1234-1234567890ae"  // bridge writes replies here
#define RPC_TX_CHAR_UUID     "12345678-1234-1234-1234-1234567890af"  // watch notifies requests here

NimBLECharacteristic *taskCharacteristic;
NimBLECharacteristic *statusCharacteristic;
NimBLECharacteristic *rpcTxCharacteristic;

Arduino_DataBus *displayBus = new Arduino_ESP32SPI(
  11,  // DC
  10,  // CS
  12,  // SCK
  13,  // MOSI
  GFX_NOT_DEFINED   // MISO
);

Arduino_GFX *display = new Arduino_GC9A01(
  displayBus,
  1,  // reset pin used by the vendor port
  1,  // rotation
  true,
  170,
  320,
  35,
  0,
  35,
  0
);

static lv_disp_draw_buf_t displayBuffer;
static lv_disp_drv_t displayDriver;
static lv_color_t *displayBuffer1;
static lv_color_t *displayBuffer2;
static lv_obj_t *screenRoot;
static lv_obj_t *modalRoot;
static lv_obj_t *listObj;
static lv_obj_t *timerLabel;

// ---- State shared with the BLE tasks -------------------------------------------------------

// The BLE callbacks run on the NimBLE task, so they only touch these under taskMux / rxMutex.
static portMUX_TYPE taskMux = portMUX_INITIALIZER_UNLOCKED;
static char pendingTask[256] = "";
static volatile bool taskChanged = false;
static volatile bool bleConnected = false;
static volatile uint16_t bleConnHandle = 0;

static SemaphoreHandle_t rxMutex;
static std::string rxAccum;
static std::string rxLines[4];
static uint8_t rxLineCount = 0;

// ---- UI model --------------------------------------------------------------------------------

enum RpcKind { RPC_NONE, RPC_STATUS, RPC_PAIR, RPC_LINK_STATUS, RPC_MATRIX, RPC_WORKS, RPC_SUBTASKS, RPC_REMOVE, RPC_UNLINK };
enum Screen { SCR_CONNECTING, SCR_PAIRING, SCR_MATRIX, SCR_WORKS, SCR_SUBTASKS, SCR_STEP };
enum LinkState { LINK_UNKNOWN, LINK_UNPAIRED, LINK_PAIRED };
enum StepMode { STEP_NONE, STEP_ACTIVE, STEP_SENT };
enum LoadState { LOAD_LOADING, LOAD_READY, LOAD_ERROR };
enum ModalKind { MODAL_NONE, MODAL_TASK, MODAL_TASK_CONFIRM, MODAL_BUSY, MODAL_ERROR, MODAL_ACCOUNT, MODAL_ACCOUNT_CONFIRM };
enum Action {
  ACT_NONE, ACT_BACK, ACT_OPEN_CAT, ACT_OPEN_WORK, ACT_OPEN_TASK, ACT_OPEN_STEP, ACT_DONE, ACT_RETRY,
  ACT_MODAL_CLOSE, ACT_TASK_REMOVE_ASK, ACT_TASK_REMOVE_YES, ACT_ACCOUNT_OPEN, ACT_ACCOUNT_UNLINK_ASK,
  ACT_ACCOUNT_UNLINK_YES
};

// Quadrant order is the on-screen order: urgent column first, important row first.
static const char *const CAT_KEYS[4] = {"do_first", "schedule", "delegate", "eliminate"};
static const char *const CAT_TITLES[4] = {"DO FIRST", "SCHEDULE", "DELEGATE", "ELIMINATE"};
static const char *const CAT_NAMES[4] = {"Do first", "Schedule", "Delegate", "Eliminate"};
static const uint32_t CAT_COLORS[4] = {COLOR_DANGER, COLOR_ACCENT, COLOR_AMBER, COLOR_BODY};

struct WorkRow {
  char id[40];
  char title[44];
  char priority;
  char due[12];
  uint8_t progress;
};

struct TaskRow {
  char id[40];
  char text[84];
  char state;  // 'd' done, 'a' on the watch, 'p' pending
};

static const uint8_t MAX_WORKS = 12;
static const uint8_t MAX_TASKS = 16;

static Screen screen = SCR_CONNECTING;
static LinkState linkState = LINK_UNKNOWN;
static char userName[32] = "";
static char pairCode[8] = "";
static char connectError[96] = "";  // why the server could not be reached, shown while connecting or pairing

static int matrixCounts[4] = {0, 0, 0, 0};
static bool matrixLoaded = false;

static WorkRow works[MAX_WORKS];
static uint8_t workCount = 0;
static uint16_t worksMore = 0;
static LoadState worksState = LOAD_LOADING;
static uint8_t selectedCat = 0;

static TaskRow tasks[MAX_TASKS];
static uint8_t taskCount = 0;
static LoadState tasksState = LOAD_LOADING;
static char selectedWorkId[40] = "";
static char selectedWorkTitle[44] = "";
static uint8_t selectedTask = 0;

static StepMode stepMode = STEP_NONE;
static char currentStep[256] = "";
static uint32_t stepStartedAt = 0;
static uint32_t sentAt = 0;

static ModalKind modal = MODAL_NONE;
static char modalError[96] = "";

static bool shownConnected = false;
static bool uiDirty = true;
static bool keepScroll = false;
static lv_coord_t savedScrollY = 0;
static Action pendingAction = ACT_NONE;
static uint32_t pendingArg = 0;

static uint16_t touchX;
static uint16_t touchY;

static lv_color_t color(uint32_t value) { return lv_color_hex(value); }

// ---- Session channel (JSON over BLE) -------------------------------------------------------------

static RpcKind pendingKind = RPC_NONE;
static uint32_t pendingId = 0;
static uint32_t pendingSince = 0;
static uint32_t nextRpcId = 1;
static bool helloSeen = false;
static bool needStatus = false;
static uint32_t connectedAt = 0;
static uint32_t nextStatusTry = 0;
static uint32_t pairExpiresAt = 0;
static uint32_t pairPollAt = 0;
static uint32_t lastMatrixFetch = 0;

static const uint32_t RPC_TIMEOUT_MS = 10000;
static const uint32_t MATRIX_REFRESH_MS = 30000;
static const uint32_t SENT_SCREEN_MS = 4000;

static void copyText(char *target, size_t size, const char *source) {
  strlcpy(target, source == nullptr ? "" : source, size);
}

// A request is chunked to the negotiated MTU (at least 20 bytes) and notified to the bridge.
static void sendRpcLine(const char *line, size_t length) {
  uint16_t mtu = 23;
  NimBLEServer *server = NimBLEDevice::getServer();
  if (server != nullptr) mtu = server->getPeerMTU(bleConnHandle);
  size_t chunk = mtu > 23 ? mtu - 3 : 20;
  if (chunk > 180) chunk = 180;
  for (size_t offset = 0; offset < length; offset += chunk) {
    size_t size = length - offset < chunk ? length - offset : chunk;
    rpcTxCharacteristic->setValue((const uint8_t *)line + offset, size);
    rpcTxCharacteristic->notify();
    delay(8);
  }
}

// Only one request is in flight; a new one replaces it and the old reply is ignored.
static bool startRpc(RpcKind kind, const char *type, const char *key1 = nullptr, const char *value1 = nullptr,
                     const char *key2 = nullptr, const char *value2 = nullptr) {
  if (!bleConnected) return false;
  JsonDocument doc;
  uint32_t id = nextRpcId++;
  doc["id"] = id;
  doc["t"] = type;
  if (key1 != nullptr) doc[key1] = value1;
  if (key2 != nullptr) doc[key2] = value2;
  char line[256];
  size_t length = serializeJson(doc, line, sizeof(line) - 1);
  line[length++] = '\n';
  pendingKind = kind;
  pendingId = id;
  pendingSince = millis();
  sendRpcLine(line, length);
  return true;
}

static bool popRxLine(std::string &line) {
  bool found = false;
  xSemaphoreTake(rxMutex, portMAX_DELAY);
  if (rxLineCount > 0) {
    line = rxLines[0];
    for (uint8_t index = 1; index < rxLineCount; index++) rxLines[index - 1] = rxLines[index];
    rxLineCount--;
    found = true;
  }
  xSemaphoreGive(rxMutex);
  return found;
}

// ---- UI building blocks -------------------------------------------------------------------------

static void queueAction(lv_event_t *event) {
  uint32_t packed = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
  pendingAction = (Action)(packed & 0xFF);
  pendingArg = packed >> 8;
}

static void *packAction(Action action, uint32_t arg = 0) {
  return (void *)(uintptr_t)((arg << 8) | (uint32_t)action);
}

// A bare container: no theme background, border, padding, scrolling or touch handling.
static lv_obj_t *makeBox(lv_obj_t *parent, lv_coord_t width, lv_coord_t height) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_set_size(box, width, height);
  lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_radius(box, 0, 0);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_set_style_shadow_width(box, 0, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
  return box;
}

static lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, uint32_t textColor,
                           const lv_font_t *font = FONT_SMALL) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, color(textColor), 0);
  lv_obj_set_style_text_font(label, font, 0);
  return label;
}

// A one-line label that ends in "..." instead of wrapping.
static lv_obj_t *makeClippedLabel(lv_obj_t *parent, const char *text, uint32_t textColor, lv_coord_t width,
                                  const lv_font_t *font = FONT_SMALL) {
  lv_obj_t *label = makeLabel(parent, text, textColor, font);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_set_size(label, width, lv_font_get_line_height(font));
  return label;
}

static lv_obj_t *makeButton(lv_obj_t *parent, lv_coord_t width, lv_coord_t height, uint32_t background,
                            uint32_t pressed, lv_coord_t radius, Action action, uint32_t arg = 0) {
  lv_obj_t *button = lv_btn_create(parent);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_radius(button, radius, 0);
  lv_obj_set_style_border_width(button, 0, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_pad_all(button, 0, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(button, color(background), 0);
  lv_obj_set_style_bg_color(button, color(pressed), LV_STATE_PRESSED);
  lv_obj_add_event_cb(button, queueAction, LV_EVENT_CLICKED, packAction(action, arg));
  return button;
}

// The rounded surface card the app uses for its boxes.
static lv_obj_t *makeCard(lv_coord_t y, lv_coord_t height) {
  lv_obj_t *card = lv_obj_create(screenRoot);
  lv_obj_set_pos(card, 12, y);
  lv_obj_set_size(card, 296, height);
  lv_obj_set_style_bg_color(card, color(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, color(COLOR_BORDER), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_pad_all(card, 10, 0);
  lv_obj_set_style_pad_row(card, 3, 0);
  lv_obj_set_style_shadow_width(card, 0, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  return card;
}

static void centerCardContent(lv_obj_t *card) {
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *makeCenteredLabel(lv_obj_t *parent, const char *text, uint32_t textColor,
                                   const lv_font_t *font = FONT_SMALL) {
  lv_obj_t *label = makeLabel(parent, text, textColor, font);
  lv_obj_set_width(label, LV_PCT(100));
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  return label;
}

// A scrolling column that fills the area below the top bar.
static lv_obj_t *makeList() {
  lv_obj_t *list = lv_obj_create(screenRoot);
  lv_obj_set_pos(list, 12, 35);
  lv_obj_set_size(list, 296, 130);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_all(list, 0, 0);
  lv_obj_set_style_pad_row(list, 6, 0);
  lv_obj_set_style_shadow_width(list, 0, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  listObj = list;
  return list;
}

static lv_obj_t *addChip(lv_obj_t *bar, const char *text, lv_coord_t rightOffset, lv_coord_t width, uint32_t chipColor,
                         Action action) {
  lv_obj_t *chip = action == ACT_NONE ? makeBox(bar, width, 20)
                                      : makeButton(bar, width, 20, COLOR_HERO, COLOR_HERO_BORDER, LV_RADIUS_CIRCLE, action);
  lv_obj_align(chip, LV_ALIGN_RIGHT_MID, rightOffset, 0);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_color(chip, color(chipColor), 0);
  lv_obj_set_style_border_width(chip, 1, 0);
  if (action != ACT_NONE) lv_obj_set_ext_click_area(chip, 6);
  lv_obj_t *label = makeClippedLabel(chip, text, chipColor, width - 8);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(label);
  return chip;
}

// Top bar: a back arrow (or the BandFlow mark), a title, the Bluetooth state and optional chips.
static void drawTopBar(const char *title, lv_coord_t titleWidth, bool back) {
  lv_obj_t *bar = makeBox(screenRoot, SCREEN_WIDTH, 30);
  lv_obj_set_style_bg_color(bar, color(COLOR_HERO), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(bar, color(COLOR_HERO_BORDER), 0);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(bar, 1, 0);

  if (back) {
    lv_obj_t *button = makeButton(bar, 34, 24, COLOR_ACCENT, COLOR_SUCCESS, 8, ACT_BACK);
    lv_obj_align(button, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_ext_click_area(button, 8);
    lv_obj_center(makeLabel(button, LV_SYMBOL_LEFT, COLOR_ACCENT_TEXT));
  } else {
    lv_obj_t *mark = makeBox(bar, 20, 20);
    lv_obj_align(mark, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mark, color(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_center(makeLabel(mark, "B", COLOR_ACCENT_TEXT));
  }
  lv_obj_align(makeClippedLabel(bar, title, COLOR_TITLE, titleWidth), LV_ALIGN_LEFT_MID, 46, 0);

  lv_obj_t *link = makeLabel(bar, shownConnected ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_WARNING,
                             shownConnected ? COLOR_ACCENT : COLOR_DANGER);
  lv_obj_align(link, LV_ALIGN_RIGHT_MID, -12, 0);

  if (screen == SCR_MATRIX) {
    addChip(bar, userName, -36, 84, COLOR_BODY, ACT_ACCOUNT_OPEN);
    if (stepMode == STEP_ACTIVE) addChip(bar, LV_SYMBOL_PLAY " Now", -126, 62, COLOR_SUCCESS, ACT_OPEN_STEP);
  } else if (screen == SCR_SUBTASKS && tasksState == LOAD_READY) {
    uint8_t done = 0;
    for (uint8_t index = 0; index < taskCount; index++) done += tasks[index].state == 'd';
    char progress[12];
    snprintf(progress, sizeof(progress), "%u/%u", done, taskCount);
    addChip(bar, progress, -36, 54, COLOR_ACCENT, ACT_NONE);
  } else if (screen == SCR_STEP && stepMode == STEP_ACTIVE) {
    lv_obj_t *chip = addChip(bar, "00:00", -36, 62, COLOR_ACCENT, ACT_NONE);
    timerLabel = lv_obj_get_child(chip, 0);
  }
}

// ---- Screens ---------------------------------------------------------------------------------------

static void drawConnecting() {
  drawTopBar("BandFlow", 180, false);
  lv_obj_t *card = makeCard(35, 129);
  centerCardContent(card);
  if (shownConnected && connectError[0]) {
    makeCenteredLabel(card, "Can't reach the server", COLOR_DANGER, FONT_STEP);
    makeCenteredLabel(card, connectError, COLOR_BODY);
    makeCenteredLabel(card, "Trying again...", COLOR_BODY);
    return;
  }
  makeCenteredLabel(card, shownConnected ? "Checking your account" : "Waiting for the bridge", COLOR_TITLE, FONT_STEP);
  makeCenteredLabel(card, shownConnected ? "One moment..." : "Start the bridge on the Raspberry Pi", COLOR_BODY);
}

static void drawPairing() {
  drawTopBar("Link your account", 180, false);
  lv_obj_t *card = makeCard(35, 129);
  centerCardContent(card);
  makeCenteredLabel(card, pairCode[0] ? "Your link code" : "Getting a code...", COLOR_BODY);
  if (pairCode[0]) {
    char spaced[12];
    snprintf(spaced, sizeof(spaced), "%.3s %.3s", pairCode, pairCode + 3);
    lv_obj_t *code = makeCenteredLabel(card, spaced, COLOR_ACCENT, FONT_CODE);
    lv_obj_set_style_text_letter_space(code, 2, 0);
  }
  const char *hintText = !shownConnected ? "Waiting for the bridge to connect"
                         : connectError[0] ? connectError
                                           : "In the app: Settings > Wristband, then enter it";
  lv_obj_t *hint = makeCenteredLabel(card, hintText, connectError[0] ? COLOR_DANGER : COLOR_BODY);
  lv_obj_set_style_pad_top(hint, 6, 0);
}

static void drawMatrix() {
  drawTopBar("Matrix", 60, false);
  // Columns are urgency and rows are importance, like the Eisenhower matrix in the app.
  lv_obj_t *urgent = makeCenteredLabel(screenRoot, "URGENT", COLOR_BODY);
  lv_obj_set_width(urgent, 146);
  lv_obj_set_pos(urgent, 9, 33);
  lv_obj_t *notUrgent = makeCenteredLabel(screenRoot, "NOT URGENT", COLOR_BODY);
  lv_obj_set_width(notUrgent, 146);
  lv_obj_set_pos(notUrgent, 165, 33);

  for (uint8_t index = 0; index < 4; index++) {
    uint8_t column = index % 2;
    uint8_t row = index / 2;
    lv_obj_t *cell = makeButton(screenRoot, 146, 56, COLOR_SURFACE, COLOR_SURFACE_PRESS, 12, ACT_OPEN_CAT, index);
    lv_obj_set_pos(cell, 9 + column * 156, 50 + row * 60);
    lv_obj_set_style_border_color(cell, color(CAT_COLORS[index]), 0);
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_t *title = makeClippedLabel(cell, CAT_TITLES[index], CAT_COLORS[index], 92);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 7);
    lv_obj_t *subtitle = makeLabel(cell, row == 0 ? "Important" : "Not important", COLOR_BODY);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 10, 29);

    lv_obj_t *count = makeBox(cell, 28, 28);
    lv_obj_align(count, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_set_style_radius(count, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(count, color(CAT_COLORS[index]), 0);
    lv_obj_set_style_bg_opa(count, LV_OPA_COVER, 0);
    char number[8];
    if (matrixLoaded) snprintf(number, sizeof(number), "%d", matrixCounts[index]);
    else snprintf(number, sizeof(number), "-");
    lv_obj_center(makeLabel(count, number, COLOR_ACCENT_TEXT));
  }
}

// Shown in place of a list while it loads, fails or is empty.
static void drawListMessage(const char *title, const char *hint, bool retry) {
  lv_obj_t *box = retry ? makeButton(screenRoot, 296, 130, COLOR_BG, COLOR_SURFACE, 12, ACT_RETRY) : makeBox(screenRoot, 296, 130);
  lv_obj_set_pos(box, 12, 35);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(box, 4, 0);
  makeCenteredLabel(box, title, COLOR_TITLE, FONT_STEP);
  if (hint != nullptr) makeCenteredLabel(box, hint, COLOR_BODY);
}

static uint32_t priorityColor(char priority) {
  if (priority == 'H') return COLOR_PRIORITY_HIGH;
  if (priority == 'M') return COLOR_PRIORITY_MED;
  return COLOR_PRIORITY_LOW;
}

static const char *priorityName(char priority) {
  if (priority == 'H') return "High";
  if (priority == 'M') return "Medium";
  return "Low";
}

// Like the task cards in the app: title, progress bar and priority / due line.
static void drawWorkCard(lv_obj_t *list, uint8_t index) {
  const WorkRow &work = works[index];
  lv_obj_t *card = makeButton(list, LV_PCT(100), 48, COLOR_SURFACE, COLOR_SURFACE_PRESS, 12, ACT_OPEN_WORK, index);
  lv_obj_set_style_border_color(card, color(COLOR_BORDER), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_flex_grow(card, 0);

  lv_obj_t *title = makeClippedLabel(card, work.title, COLOR_TITLE, 220);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 11, 6);
  char percent[8];
  snprintf(percent, sizeof(percent), "%u%%", work.progress);
  lv_obj_align(makeLabel(card, percent, COLOR_BODY), LV_ALIGN_TOP_RIGHT, -11, 6);

  lv_obj_t *bar = lv_bar_create(card);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(bar, 96, 6);
  lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 11, 32);
  lv_obj_set_style_bg_color(bar, color(COLOR_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, color(COLOR_ACCENT), LV_PART_INDICATOR);
  lv_bar_set_value(bar, work.progress, LV_ANIM_OFF);

  char meta[40];
  if (work.due[0]) snprintf(meta, sizeof(meta), "%s  Due %s", priorityName(work.priority), work.due + (strlen(work.due) > 5 ? 5 : 0));
  else snprintf(meta, sizeof(meta), "%s", priorityName(work.priority));
  lv_obj_t *metaLabel = makeClippedLabel(card, meta, priorityColor(work.priority), 160);
  lv_obj_align(metaLabel, LV_ALIGN_TOP_LEFT, 120, 25);
}

static void drawWorks() {
  drawTopBar(CAT_NAMES[selectedCat], 200, true);
  if (worksState == LOAD_LOADING) return drawListMessage("Loading...", nullptr, false);
  if (worksState == LOAD_ERROR) return drawListMessage("Could not load", "Tap to try again", true);
  if (workCount == 0) return drawListMessage("Nothing here", "No active work in this quadrant", false);
  lv_obj_t *list = makeList();
  for (uint8_t index = 0; index < workCount; index++) drawWorkCard(list, index);
  if (worksMore > 0) {
    char more[40];
    snprintf(more, sizeof(more), "+%u more in the app", (unsigned)worksMore);
    lv_obj_t *label = makeCenteredLabel(list, more, COLOR_BODY);
    lv_obj_set_style_pad_bottom(label, 4, 0);
  }
}

static void drawTaskRow(lv_obj_t *list, uint8_t index) {
  const TaskRow &task = tasks[index];
  bool done = task.state == 'd';
  bool active = task.state == 'a';
  lv_obj_t *row = makeButton(list, LV_PCT(100), 36, COLOR_SURFACE, COLOR_SURFACE_PRESS, 10, ACT_OPEN_TASK, index);
  lv_obj_set_style_border_color(row, color(active ? COLOR_ACCENT : COLOR_BORDER), 0);
  lv_obj_set_style_border_width(row, 1, 0);

  // Same three states as the checkbox in the app's subtask list.
  lv_obj_t *box = makeBox(row, 18, 18);
  lv_obj_align(box, LV_ALIGN_LEFT_MID, 9, 0);
  lv_obj_set_style_radius(box, 5, 0);
  lv_obj_set_style_border_width(box, 1, 0);
  lv_obj_set_style_border_color(box, color(done ? COLOR_SUCCESS : active ? COLOR_ACCENT : COLOR_BORDER), 0);
  if (done) {
    lv_obj_set_style_bg_color(box, color(COLOR_SUCCESS), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_center(makeLabel(box, LV_SYMBOL_OK, COLOR_ACCENT_TEXT));
  }

  lv_obj_t *text = makeClippedLabel(row, task.text, active ? COLOR_TITLE : COLOR_BODY, active ? 190 : 248);
  lv_obj_align(text, LV_ALIGN_LEFT_MID, 36, 0);
  if (done) lv_obj_set_style_text_decor(text, LV_TEXT_DECOR_STRIKETHROUGH, 0);
  if (active) {
    lv_obj_t *chip = makeBox(row, 56, 20);
    lv_obj_align(chip, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(chip, color(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_center(makeLabel(chip, "WATCH", COLOR_ACCENT));
  }
}

static void drawSubtasks() {
  drawTopBar(selectedWorkTitle, 130, true);
  if (tasksState == LOAD_LOADING) return drawListMessage("Loading...", nullptr, false);
  if (tasksState == LOAD_ERROR) return drawListMessage("Could not load", "Tap to try again", true);
  lv_obj_t *list = makeList();
  for (uint8_t index = 0; index < taskCount; index++) drawTaskRow(list, index);
}

static void drawStep() {
  if (stepMode == STEP_SENT) {
    drawTopBar("BandFlow", 180, true);
    lv_obj_t *card = makeCard(35, 129);
    centerCardContent(card);
    lv_obj_t *badge = makeBox(card, 40, 40);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, color(COLOR_SUCCESS), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_center(makeLabel(badge, LV_SYMBOL_OK, COLOR_ACCENT_TEXT, FONT_STEP));
    lv_obj_t *title = makeCenteredLabel(card, "Step done", COLOR_TITLE, FONT_STEP);
    lv_obj_set_style_pad_top(title, 6, 0);
    makeCenteredLabel(card, "Waiting for the next step...", COLOR_BODY);
    return;
  }

  drawTopBar("Current step", 110, true);
  // The step text can be long, so the card scrolls vertically when it does not fit.
  lv_obj_t *card = makeCard(35, 86);
  lv_obj_set_scroll_dir(card, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *caption = makeLabel(card, "ON THE WRISTBAND", COLOR_BODY);
  lv_obj_set_style_text_letter_space(caption, 1, 0);
  lv_obj_t *step = makeLabel(card, currentStep, COLOR_TITLE, FONT_STEP);
  lv_label_set_long_mode(step, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(step, LV_PCT(100));

  lv_obj_t *button = makeButton(screenRoot, 296, 38, COLOR_SUCCESS, COLOR_SUCCESS_PRESS, 14, ACT_DONE);
  lv_obj_set_pos(button, 12, 127);
  lv_obj_set_style_bg_color(button, color(COLOR_DISABLED), LV_STATE_DISABLED);
  // Without a link the DONE notification would go nowhere, so I block the button instead.
  lv_obj_t *label = makeLabel(button, shownConnected ? LV_SYMBOL_OK "  DONE" : "Not connected",
                              shownConnected ? COLOR_ACCENT_TEXT : COLOR_BODY, FONT_STEP);
  lv_obj_center(label);
  if (!shownConnected) lv_obj_add_state(button, LV_STATE_DISABLED);
}

// ---- Modal dialogs --------------------------------------------------------------------------------

static lv_obj_t *modalButton(lv_obj_t *card, const char *text, Action action, bool filled, uint32_t accent, lv_coord_t x,
                             lv_coord_t width) {
  lv_obj_t *button = makeButton(card, width, 34, filled ? accent : COLOR_SURFACE, filled ? COLOR_DANGER_PRESS : COLOR_SURFACE_PRESS, 10, action);
  lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, x, 0);
  if (!filled) {
    lv_obj_set_style_border_color(button, color(accent), 0);
    lv_obj_set_style_border_width(button, 1, 0);
  }
  lv_obj_center(makeLabel(button, text, filled ? COLOR_ACCENT_TEXT : accent));
  return button;
}

static void drawModal() {
  modalRoot = makeBox(screenRoot, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_pos(modalRoot, 0, 0);
  lv_obj_set_style_bg_color(modalRoot, color(0x000000), 0);
  lv_obj_set_style_bg_opa(modalRoot, LV_OPA_70, 0);
  lv_obj_add_flag(modalRoot, LV_OBJ_FLAG_CLICKABLE);  // swallows taps meant for the screen underneath

  const char *caption = "";
  uint32_t captionColor = COLOR_BODY;
  const char *body = "";
  const char *leftText = nullptr;
  Action leftAction = ACT_MODAL_CLOSE;
  const char *rightText = nullptr;
  Action rightAction = ACT_NONE;
  bool rightFilled = false;
  const TaskRow &task = tasks[selectedTask < MAX_TASKS ? selectedTask : 0];

  switch (modal) {
    case MODAL_TASK:
      caption = task.state == 'a' ? "TASK - ON THE WATCH" : task.state == 'd' ? "TASK - DONE" : "TASK";
      body = task.text;
      leftText = "Close";
      rightText = "Remove";
      rightAction = ACT_TASK_REMOVE_ASK;
      break;
    case MODAL_TASK_CONFIRM:
      caption = "REMOVE THIS TASK?";
      captionColor = COLOR_DANGER;
      body = task.text;
      leftText = "Cancel";
      rightText = "Remove";
      rightAction = ACT_TASK_REMOVE_YES;
      rightFilled = true;
      break;
    case MODAL_BUSY:
      caption = "WORKING";
      body = "Please wait...";
      break;
    case MODAL_ERROR:
      caption = "SOMETHING WENT WRONG";
      captionColor = COLOR_DANGER;
      body = modalError;
      leftText = "Close";
      break;
    case MODAL_ACCOUNT:
      caption = "SIGNED IN AS";
      body = userName;
      leftText = "Close";
      rightText = "Unlink";
      rightAction = ACT_ACCOUNT_UNLINK_ASK;
      break;
    case MODAL_ACCOUNT_CONFIRM:
      caption = "UNLINK THIS WATCH?";
      captionColor = COLOR_DANGER;
      body = "You will need a new code to link it again.";
      leftText = "Cancel";
      rightText = "Unlink";
      rightAction = ACT_ACCOUNT_UNLINK_YES;
      rightFilled = true;
      break;
    default:
      return;
  }

  lv_coord_t bodyHeight = lv_font_get_line_height(FONT_STEP) * 3;
  lv_obj_t *card = lv_obj_create(modalRoot);
  lv_obj_set_size(card, 280, 10 + 17 + 4 + bodyHeight + 8 + 34 + 10);
  lv_obj_center(card);
  lv_obj_set_style_bg_color(card, color(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, color(COLOR_BORDER), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_pad_all(card, 10, 0);
  lv_obj_set_style_shadow_width(card, 0, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *captionLabel = makeLabel(card, caption, captionColor);
  lv_obj_set_style_text_letter_space(captionLabel, 1, 0);
  lv_obj_align(captionLabel, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_t *bodyLabel = makeLabel(card, body, COLOR_TITLE, FONT_STEP);
  lv_label_set_long_mode(bodyLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_size(bodyLabel, 258, bodyHeight);
  lv_obj_align(bodyLabel, LV_ALIGN_TOP_LEFT, 0, 21);

  if (leftText != nullptr && rightText != nullptr) {
    modalButton(card, leftText, leftAction, false, COLOR_BODY, 0, 124);
    modalButton(card, rightText, rightAction, rightFilled, COLOR_DANGER, 134, 124);
  } else if (leftText != nullptr) {
    modalButton(card, leftText, leftAction, false, COLOR_BODY, 0, 258);
  }
}

static void showModal(ModalKind kind) {
  if (modalRoot != nullptr) {
    lv_obj_del(modalRoot);
    modalRoot = nullptr;
  }
  modal = kind;
  if (kind != MODAL_NONE) drawModal();
}

// ---- Rendering -----------------------------------------------------------------------------------------

static void renderUi() {
  // A data refresh keeps the list where the user scrolled it; changing screen starts at the top.
  if (keepScroll && listObj != nullptr) savedScrollY = lv_obj_get_scroll_y(listObj);
  else savedScrollY = 0;
  lv_obj_clean(screenRoot);
  modalRoot = nullptr;
  listObj = nullptr;
  timerLabel = nullptr;

  switch (screen) {
    case SCR_CONNECTING: drawConnecting(); break;
    case SCR_PAIRING: drawPairing(); break;
    case SCR_MATRIX: drawMatrix(); break;
    case SCR_WORKS: drawWorks(); break;
    case SCR_SUBTASKS: drawSubtasks(); break;
    case SCR_STEP: drawStep(); break;
  }
  if (listObj != nullptr && savedScrollY > 0) {
    lv_obj_update_layout(listObj);
    lv_obj_scroll_to_y(listObj, savedScrollY, LV_ANIM_OFF);
  }
  if (modal != MODAL_NONE) drawModal();
  uiDirty = false;
  keepScroll = false;

  lv_mem_monitor_t monitor;
  lv_mem_monitor(&monitor);
  Serial.printf("UI drawn, LVGL memory free: %u bytes\n", (unsigned)monitor.free_size);
}

static void updateStepTimer(lv_timer_t *) {
  if (timerLabel == nullptr) return;
  uint32_t seconds = (millis() - stepStartedAt) / 1000;
  char text[16];
  snprintf(text, sizeof(text), "%02lu:%02lu", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
  lv_label_set_text(timerLabel, text);
}

// ---- Display and touch setup ---------------------------------------------------------------------------

static void displayFlush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colorData) {
  uint32_t width = area->x2 - area->x1 + 1;
  uint32_t height = area->y2 - area->y1 + 1;
#if (LV_COLOR_16_SWAP != 0)
  display->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&colorData->full, width, height);
#else
  display->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&colorData->full, width, height);
#endif
  lv_disp_flush_ready(driver);
}

static void readTouch(lv_indev_drv_t *, lv_indev_data_t *data) {
  if (CHSC6413_Scan(&touchX, &touchY) == 1) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = touchX;
    data->point.y = touchY;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

static void setupDisplay() {
  display->begin(80000000);
  display->invertDisplay(true);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  CHSC6413_init();

  lv_init();
  displayBuffer1 = (lv_color_t *)heap_caps_malloc(
    sizeof(lv_color_t) * SCREEN_WIDTH * SCREEN_HEIGHT / 8,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
  );
  displayBuffer2 = (lv_color_t *)heap_caps_malloc(
    sizeof(lv_color_t) * SCREEN_WIDTH * SCREEN_HEIGHT / 8,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
  );
  if (displayBuffer1 == nullptr || displayBuffer2 == nullptr) {
    Serial.println("Display buffer allocation failed");
    while (true) delay(1000);
  }

  lv_disp_draw_buf_init(&displayBuffer, displayBuffer1, displayBuffer2, SCREEN_WIDTH * SCREEN_HEIGHT / 8);
  lv_disp_drv_init(&displayDriver);
  displayDriver.hor_res = SCREEN_WIDTH;
  displayDriver.ver_res = SCREEN_HEIGHT;
  displayDriver.flush_cb = displayFlush;
  displayDriver.draw_buf = &displayBuffer;
  lv_disp_drv_register(&displayDriver);

  static lv_indev_drv_t inputDriver;
  lv_indev_drv_init(&inputDriver);
  inputDriver.type = LV_INDEV_TYPE_POINTER;
  inputDriver.read_cb = readTouch;
  lv_indev_drv_register(&inputDriver);

  lv_obj_t *root = lv_scr_act();
  lv_obj_set_style_bg_color(root, color(COLOR_BG), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

  screenRoot = makeBox(root, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_pos(screenRoot, 0, 0);
  lv_timer_create(updateStepTimer, 1000, nullptr);
}

// ---- Navigation and data loading ------------------------------------------------------------------------

static void navigate(Screen next);

static void requestMatrix() {
  lastMatrixFetch = millis();
  startRpc(RPC_MATRIX, "matrix");
}

static void requestWorks() {
  worksState = LOAD_LOADING;
  if (!startRpc(RPC_WORKS, "works", "cat", CAT_KEYS[selectedCat])) worksState = LOAD_ERROR;
}

static void requestTasks() {
  tasksState = LOAD_LOADING;
  if (!startRpc(RPC_SUBTASKS, "subtasks", "work", selectedWorkId)) tasksState = LOAD_ERROR;
}

static void goHome() {
  if (linkState == LINK_PAIRED) navigate(SCR_MATRIX);
  else navigate(linkState == LINK_UNPAIRED ? SCR_PAIRING : SCR_CONNECTING);
}

static void navigate(Screen next) {
  screen = next;
  modal = MODAL_NONE;
  keepScroll = false;
  uiDirty = true;
  if (next == SCR_MATRIX) requestMatrix();
  else if (next == SCR_WORKS) requestWorks();
  else if (next == SCR_SUBTASKS) requestTasks();
}

static void setUnlinked() {
  linkState = LINK_UNPAIRED;
  userName[0] = '\0';
  pairCode[0] = '\0';
  pairExpiresAt = 0;  // asks for a fresh code straight away
  matrixLoaded = false;
  if (screen != SCR_STEP || stepMode != STEP_ACTIVE) navigate(SCR_PAIRING);
  else modal = MODAL_NONE;
}

static void onLinked(const char *name) {
  bool nameChanged = strcmp(userName, name) != 0;
  copyText(userName, sizeof(userName), name);
  pairCode[0] = '\0';
  bool first = linkState != LINK_PAIRED;
  linkState = LINK_PAIRED;
  Serial.printf("Linked to %s\n", userName);
  if (first && (screen == SCR_CONNECTING || screen == SCR_PAIRING)) {
    if (stepMode == STEP_ACTIVE) navigate(SCR_STEP);
    else goHome();
  } else if (nameChanged) {
    uiDirty = true;
  }
}

// I tell the Raspberry Pi the step is finished; it then sends the next ready subtask.
static void sendDone() {
  statusCharacteristic->setValue("DONE");
  statusCharacteristic->notify();
  Serial.println("Sent DONE to the Pi");
  stepMode = STEP_SENT;
  sentAt = millis();
  uiDirty = true;
}

// ---- Replies from the bridge ------------------------------------------------------------------------------

static void applyTasks(JsonDocument &doc) {
  const char *title = doc["work"]["title"] | "";
  if (title[0]) copyText(selectedWorkTitle, sizeof(selectedWorkTitle), title);
  taskCount = 0;
  for (JsonObject item : doc["subtasks"].as<JsonArray>()) {
    if (taskCount >= MAX_TASKS) break;
    TaskRow &row = tasks[taskCount++];
    copyText(row.id, sizeof(row.id), item["id"] | "");
    copyText(row.text, sizeof(row.text), item["d"] | "");
    const char *state = item["s"] | "p";
    row.state = state[0];
  }
  tasksState = LOAD_READY;
}

static void onRpcError(RpcKind kind, const char *error) {
  Serial.printf("Request failed: %s\n", error);
  if (strcmp(error, "not_linked") == 0) {
    setUnlinked();
    return;
  }
  switch (kind) {
    case RPC_WORKS:
      worksState = LOAD_ERROR;
      keepScroll = false;
      uiDirty = true;
      break;
    case RPC_SUBTASKS:
      tasksState = LOAD_ERROR;
      uiDirty = true;
      break;
    case RPC_REMOVE:
    case RPC_UNLINK:
      copyText(modalError, sizeof(modalError), error);
      showModal(MODAL_ERROR);
      break;
    case RPC_STATUS:
    case RPC_PAIR:
    case RPC_LINK_STATUS:
      // runLogic() retries these, so the screen explains why it is still waiting.
      copyText(connectError, sizeof(connectError), error);
      if (screen == SCR_CONNECTING || screen == SCR_PAIRING) uiDirty = true;
      break;
    default:
      break;  // the matrix is retried by runLogic()
  }
}

static void handleReply(JsonDocument &doc, RpcKind kind) {
  if (connectError[0] != ' ' && (kind == RPC_STATUS || kind == RPC_PAIR || kind == RPC_LINK_STATUS)) {
    connectError[0] = ' ';
    uiDirty = true;
  }
  switch (kind) {
    case RPC_STATUS:
    case RPC_PAIR:
    case RPC_LINK_STATUS:
      if (doc["linked"] | false) {
        onLinked(doc["name"] | "");
      } else if (kind == RPC_STATUS) {
        if (linkState != LINK_UNPAIRED) setUnlinked();
      } else if (kind == RPC_PAIR) {
        copyText(pairCode, sizeof(pairCode), doc["code"] | "");
        uint32_t ttl = doc["ttl"] | 300;
        pairExpiresAt = millis() + (ttl > 10 ? ttl - 5 : ttl) * 1000UL;
        pairPollAt = millis() + 2000;
        uiDirty = true;
      }
      break;
    case RPC_MATRIX: {
      int counts[4];
      for (uint8_t index = 0; index < 4; index++) counts[index] = doc["counts"][CAT_KEYS[index]] | 0;
      bool changed = !matrixLoaded || memcmp(counts, matrixCounts, sizeof(counts)) != 0;
      memcpy(matrixCounts, counts, sizeof(counts));
      matrixLoaded = true;
      if (changed && screen == SCR_MATRIX) uiDirty = true;
      break;
    }
    case RPC_WORKS:
      workCount = 0;
      for (JsonObject item : doc["works"].as<JsonArray>()) {
        if (workCount >= MAX_WORKS) break;
        WorkRow &row = works[workCount++];
        copyText(row.id, sizeof(row.id), item["id"] | "");
        copyText(row.title, sizeof(row.title), item["title"] | "");
        copyText(row.due, sizeof(row.due), item["due"] | "");
        const char *priority = item["pr"] | "M";
        row.priority = priority[0];
        row.progress = item["p"] | 0;
      }
      worksMore = doc["more"] | 0;
      worksState = LOAD_READY;
      if (screen == SCR_WORKS) uiDirty = true;
      break;
    case RPC_SUBTASKS:
      applyTasks(doc);
      if (screen == SCR_SUBTASKS) uiDirty = true;
      break;
    case RPC_REMOVE:
      applyTasks(doc);
      // If the removed task was the one on the watch and nothing replaced it, the step screen is stale.
      if ((doc["clear_step"] | false) && stepMode == STEP_ACTIVE) stepMode = STEP_NONE;
      keepScroll = true;
      showModal(MODAL_NONE);
      uiDirty = true;
      break;
    case RPC_UNLINK:
      stepMode = STEP_NONE;
      setUnlinked();
      break;
    default:
      break;
  }
}

static void handleMessage(const std::string &line) {
  JsonDocument doc;
  if (deserializeJson(doc, line.c_str()) != DeserializationError::Ok) {
    Serial.println("Ignoring a malformed reply from the bridge");
    return;
  }
  const char *type = doc["t"] | "";
  if (strcmp(type, "hello") == 0) {
    // The bridge just subscribed to us, so this is the moment to ask who is linked.
    helloSeen = true;
    needStatus = true;
    return;
  }
  uint32_t id = doc["id"] | 0;
  if (id == 0 || id != pendingId) return;  // a reply to a request that was replaced
  RpcKind kind = pendingKind;
  pendingKind = RPC_NONE;
  pendingId = 0;
  if (!(doc["ok"] | false)) {
    onRpcError(kind, doc["error"] | "Request failed");
    return;
  }
  handleReply(doc, kind);
}

// ---- Main loop pieces -------------------------------------------------------------------------------------------

// I pull BLE changes into the UI from the main loop, since LVGL is not thread safe.
static void syncUi() {
  char taskCopy[sizeof(pendingTask)];
  bool changed;
  bool connected;
  portENTER_CRITICAL(&taskMux);
  changed = taskChanged;
  connected = bleConnected;
  if (changed) {
    memcpy(taskCopy, pendingTask, sizeof(taskCopy));
    taskCopy[sizeof(taskCopy) - 1] = '\0';
    taskChanged = false;
  }
  portEXIT_CRITICAL(&taskMux);

  if (connected != shownConnected) {
    shownConnected = connected;
    uiDirty = true;
    if (connected) {
      connectedAt = millis();
      helloSeen = false;
    } else {
      // Nothing can answer any more, so stop waiting on it.
      pendingKind = RPC_NONE;
      pendingId = 0;
      if (worksState == LOAD_LOADING) worksState = LOAD_ERROR;
      if (tasksState == LOAD_LOADING) tasksState = LOAD_ERROR;
      if (modal == MODAL_BUSY) {
        copyText(modalError, sizeof(modalError), "Lost the connection to the bridge.");
        modal = MODAL_ERROR;
      }
    }
  }

  if (changed) {
    copyText(currentStep, sizeof(currentStep), taskCopy);
    stepStartedAt = millis();
    stepMode = STEP_ACTIVE;
    Serial.print("Display updated: ");
    Serial.println(currentStep);
    navigate(SCR_STEP);
  }
}

static void drainReplies() {
  std::string line;
  while (popRxLine(line)) handleMessage(line);
}

static void runLogic() {
  uint32_t now = millis();

  if (pendingKind != RPC_NONE && now - pendingSince > RPC_TIMEOUT_MS) {
    RpcKind kind = pendingKind;
    pendingKind = RPC_NONE;
    pendingId = 0;
    onRpcError(kind, "No answer from the bridge.");
  }

  if (bleConnected && pendingKind == RPC_NONE) {
    bool waitingForAccount = linkState == LINK_UNKNOWN && now - connectedAt > 2500 && now >= nextStatusTry;
    if (needStatus || waitingForAccount) {
      needStatus = false;
      nextStatusTry = now + 4000;
      startRpc(RPC_STATUS, "status");
    } else if (linkState == LINK_UNPAIRED) {
      // Keep the code on screen fresh, and notice as soon as the app has linked it.
      if ((int32_t)(now - pairExpiresAt) >= 0) startRpc(RPC_PAIR, "pair");
      else if ((int32_t)(now - pairPollAt) >= 0) {
        pairPollAt = now + 2000;
        startRpc(RPC_LINK_STATUS, "link_status");
      }
    } else if (linkState == LINK_PAIRED && screen == SCR_MATRIX && now - lastMatrixFetch > MATRIX_REFRESH_MS) {
      requestMatrix();
    }
  }

  // After DONE the next step normally arrives within a second; if none does, go back to the matrix.
  if (screen == SCR_STEP && stepMode == STEP_SENT && now - sentAt > SENT_SCREEN_MS) {
    stepMode = STEP_NONE;
    goHome();
  }
}

static void runAction() {
  Action action = pendingAction;
  uint32_t arg = pendingArg;
  pendingAction = ACT_NONE;
  if (action == ACT_NONE) return;

  switch (action) {
    case ACT_BACK:
      if (screen == SCR_WORKS) navigate(SCR_MATRIX);
      else if (screen == SCR_SUBTASKS) navigate(SCR_WORKS);
      else goHome();
      break;
    case ACT_OPEN_CAT:
      selectedCat = arg < 4 ? arg : 0;
      navigate(SCR_WORKS);
      break;
    case ACT_OPEN_WORK:
      if (arg >= workCount) break;
      copyText(selectedWorkId, sizeof(selectedWorkId), works[arg].id);
      copyText(selectedWorkTitle, sizeof(selectedWorkTitle), works[arg].title);
      taskCount = 0;
      navigate(SCR_SUBTASKS);
      break;
    case ACT_OPEN_TASK:
      if (arg >= taskCount) break;
      selectedTask = arg;
      showModal(MODAL_TASK);
      break;
    case ACT_OPEN_STEP:
      if (stepMode == STEP_ACTIVE) navigate(SCR_STEP);
      break;
    case ACT_DONE:
      if (screen == SCR_STEP && stepMode == STEP_ACTIVE && shownConnected) sendDone();
      break;
    case ACT_RETRY:
      if (screen == SCR_WORKS) requestWorks();
      else if (screen == SCR_SUBTASKS) requestTasks();
      uiDirty = true;
      break;
    case ACT_MODAL_CLOSE:
      showModal(MODAL_NONE);
      break;
    case ACT_TASK_REMOVE_ASK:
      showModal(MODAL_TASK_CONFIRM);
      break;
    case ACT_TASK_REMOVE_YES:
      if (!startRpc(RPC_REMOVE, "remove", "work", selectedWorkId, "sub", tasks[selectedTask].id)) {
        copyText(modalError, sizeof(modalError), "Not connected to the bridge.");
        showModal(MODAL_ERROR);
      } else {
        showModal(MODAL_BUSY);
      }
      break;
    case ACT_ACCOUNT_OPEN:
      showModal(MODAL_ACCOUNT);
      break;
    case ACT_ACCOUNT_UNLINK_ASK:
      showModal(MODAL_ACCOUNT_CONFIRM);
      break;
    case ACT_ACCOUNT_UNLINK_YES:
      if (!startRpc(RPC_UNLINK, "unlink")) {
        copyText(modalError, sizeof(modalError), "Not connected to the bridge.");
        showModal(MODAL_ERROR);
      } else {
        showModal(MODAL_BUSY);
      }
      break;
    default:
      break;
  }
}

// ---- BLE ---------------------------------------------------------------------------------------------------------

// I copy each task received from the Raspberry Pi into the display buffer.
class TaskCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override {
    std::string value = characteristic->getValue();
    Serial.print("Received subtask from Pi: ");
    Serial.println(value.c_str());
    if (value.empty()) return;
    portENTER_CRITICAL(&taskMux);
    strncpy(pendingTask, value.c_str(), sizeof(pendingTask) - 1);
    pendingTask[sizeof(pendingTask) - 1] = '\0';
    taskChanged = true;
    portEXIT_CRITICAL(&taskMux);
  }
};

// Replies arrive in chunks; I collect them until the newline and queue each finished line for the main loop.
class RpcRxCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override {
    std::string chunk = characteristic->getValue();
    xSemaphoreTake(rxMutex, portMAX_DELAY);
    rxAccum += chunk;
    if (rxAccum.size() > 8192) rxAccum.clear();
    size_t newline;
    while ((newline = rxAccum.find('\n')) != std::string::npos) {
      if (rxLineCount < 4) rxLines[rxLineCount++] = rxAccum.substr(0, newline);
      else Serial.println("Dropping a reply: queue full");
      rxAccum.erase(0, newline + 1);
    }
    xSemaphoreGive(rxMutex);
  }
};

class ServerCallback : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override {
    portENTER_CRITICAL(&taskMux);
    bleConnected = true;
    bleConnHandle = connInfo.getConnHandle();
    portEXIT_CRITICAL(&taskMux);
    Serial.println("Pi connected");
  }

  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override {
    portENTER_CRITICAL(&taskMux);
    bleConnected = false;
    portEXIT_CRITICAL(&taskMux);
    xSemaphoreTake(rxMutex, portMAX_DELAY);
    rxAccum.clear();
    rxLineCount = 0;
    xSemaphoreGive(rxMutex);
    Serial.println("Pi disconnected, advertising again");
    NimBLEDevice::startAdvertising();
  }
};

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Starting BandFlow wristband BLE...");

  rxMutex = xSemaphoreCreateMutex();
  setupDisplay();
  renderUi();

  NimBLEDevice::init("BandFlow-Wristband");
  NimBLEDevice::setMTU(247);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallback());
  NimBLEService *service = server->createService(SERVICE_UUID);

  // The Raspberry Pi writes the current subtask here.
  taskCharacteristic = service->createCharacteristic(
    TASK_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE
  );
  taskCharacteristic->setCallbacks(new TaskCallback());

  // The wristband notifies "DONE" here when the wearer finishes the current subtask.
  statusCharacteristic = service->createCharacteristic(
    STATUS_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  statusCharacteristic->setValue("READY");

  // Session channel: replies come in on RPC_RX, requests go out as notifications on RPC_TX.
  NimBLECharacteristic *rpcRx = service->createCharacteristic(RPC_RX_CHAR_UUID, NIMBLE_PROPERTY::WRITE);
  rpcRx->setCallbacks(new RpcRxCallback());
  rpcTxCharacteristic = service->createCharacteristic(RPC_TX_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);

  service->start();

  // I advertise the service so the Raspberry Pi can discover the band.
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->start();

  Serial.println("BLE advertising started. Look for 'BandFlow-Wristband' in nRF Connect.");
}

void loop() {
  static uint32_t lastTick = millis();
  uint32_t now = millis();
  lv_tick_inc(now - lastTick);
  lastTick = now;

  syncUi();
  drainReplies();
  runLogic();
  runAction();
  if (uiDirty) renderUi();
  lv_timer_handler();
  delay(5);
}
//sergei gomo
