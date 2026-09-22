/*
  BandFlow — ESP32-S3 wristband BLE firmware (step 1: no display yet)

  What this does:
  - Advertises itself over BLE as "BandFlow-Wristband"
  - Creates ONE service with TWO characteristics:
      TASK_CHAR   — the Raspberry Pi WRITES to this. This is how the Pi
                    sends the current subtask text to the band.
      STATUS_CHAR — the band WRITES to this (Pi reads/subscribes). This is
                    how the band reports things back, e.g. "DONE" when the
                    user taps a button.
  - For now, instead of a touchscreen button, it fakes a "DONE" report
    every 15 seconds so you can test the round-trip without any display
    wiring yet. Swap that timer for a real touch event once the screen
    is wired in.

  Setup required in Arduino IDE before flashing:
  1. File > Preferences > Additional Board URLs, add:
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-storage/package_esp32_index.json
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
  2. Tools > Board > Boards Manager > search "esp32" > install "esp32 by Espressif Systems"
  3. Tools > Board > select your specific ESP32-S3 board
  4. Sketch > Include Library > Manage Libraries > search "NimBLE-Arduino" > install
  5. Select the correct Port (USB) before uploading

  How to test after flashing (no Pi needed yet):
  - Install "nRF Connect" (free) on your phone
  - Scan for BLE devices, find "BandFlow-Wristband", connect
  - You should see one service with two characteristics
  - Try writing text to TASK_CHAR from the app — watch Serial Monitor
    on your laptop print what it received
  - Watch STATUS_CHAR update every 15 seconds with "DONE"
*/

#include <NimBLEDevice.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 170
#define TFT_BL        38

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
static lv_obj_t *taskLabel;
static portMUX_TYPE taskMux = portMUX_INITIALIZER_UNLOCKED;
static char pendingTask[256] = "Waiting for a task...";
static volatile bool taskChanged = true;

static void displayFlush(lv_disp_drv_t *driver, const lv_area_t *area,
                         lv_color_t *color) {
  uint32_t width = area->x2 - area->x1 + 1;
  uint32_t height = area->y2 - area->y1 + 1;
  display->draw16bitRGBBitmap(area->x1, area->y1,
                              (uint16_t *)&color->full, width, height);
  lv_disp_flush_ready(driver);
}

static void setupDisplay() {
  display->begin(80000000);
  display->invertDisplay(true);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

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
    return;
  }

  lv_disp_draw_buf_init(&displayBuffer, displayBuffer1, displayBuffer2,
                        SCREEN_WIDTH * SCREEN_HEIGHT / 8);
  lv_disp_drv_init(&displayDriver);
  displayDriver.hor_res = SCREEN_WIDTH;
  displayDriver.ver_res = SCREEN_HEIGHT;
  displayDriver.flush_cb = displayFlush;
  displayDriver.draw_buf = &displayBuffer;
  lv_disp_drv_register(&displayDriver);

  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  lv_obj_t *heading = lv_label_create(screen);
  lv_label_set_text(heading, "CURRENT TASK");
  lv_obj_set_style_text_color(heading, lv_color_hex(0x55D6BE), 0);
  lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 20);

  taskLabel = lv_label_create(screen);
  lv_label_set_text(taskLabel, pendingTask);
  lv_label_set_long_mode(taskLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(taskLabel, 285);
  lv_obj_set_style_text_color(taskLabel, lv_color_white(), 0);
  lv_obj_set_style_text_align(taskLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(taskLabel, LV_ALIGN_CENTER, 0, 5);
}

static void updateTaskDisplay() {
  char taskCopy[sizeof(pendingTask)];
  bool changed;
  portENTER_CRITICAL(&taskMux);
  changed = taskChanged;
  if (changed) {
    memcpy(taskCopy, pendingTask, sizeof(taskCopy));
    taskCopy[sizeof(taskCopy) - 1] = '\0';
    taskChanged = false;
  }
  portEXIT_CRITICAL(&taskMux);

  if (changed && taskLabel != nullptr) {
    lv_label_set_text(taskLabel, taskCopy);
    lv_obj_invalidate(taskLabel);
    Serial.print("Display updated: ");
    Serial.println(taskCopy);
  }
}

// These are just random unique IDs (UUIDs) that identify our service
// and characteristics. They must match EXACTLY on the Raspberry Pi side
// later, so don't change them once the Pi code is written against them.
#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define TASK_CHAR_UUID       "12345678-1234-1234-1234-1234567890ac"
#define STATUS_CHAR_UUID     "12345678-1234-1234-1234-1234567890ad"

NimBLECharacteristic *taskCharacteristic;
NimBLECharacteristic *statusCharacteristic;

unsigned long lastFakeStatusUpdate = 0;
const unsigned long FAKE_STATUS_INTERVAL_MS = 15000; // every 15 seconds

// Called automatically whenever the Pi writes to TASK_CHAR
class TaskCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override {
    std::string value = characteristic->getValue();
    Serial.print("Received subtask from Pi: ");
    Serial.println(value.c_str());
    portENTER_CRITICAL(&taskMux);
    strncpy(pendingTask, value.c_str(), sizeof(pendingTask) - 1);
    pendingTask[sizeof(pendingTask) - 1] = '\0';
    taskChanged = true;
    portEXIT_CRITICAL(&taskMux);
  }
};

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Starting BandFlow wristband BLE...");

  setupDisplay();

  NimBLEDevice::init("BandFlow-Wristband");

  NimBLEServer *server = NimBLEDevice::createServer();
  NimBLEService *service = server->createService(SERVICE_UUID);

  // Pi -> band: Pi writes subtask text here
  taskCharacteristic = service->createCharacteristic(
    TASK_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE
  );
  taskCharacteristic->setCallbacks(new TaskCallback());

  // Band -> Pi: band writes status updates here, Pi reads/subscribes
  statusCharacteristic = service->createCharacteristic(
    STATUS_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  statusCharacteristic->setValue("READY");

  service->start();

  // Make the device discoverable
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->start();

  Serial.println("BLE advertising started. Look for 'BandFlow-Wristband' in nRF Connect.");
}

void loop() {
  updateTaskDisplay();
  lv_tick_inc(5);
  lv_timer_handler();
  delay(5);

  // Fake a "user tapped done" event every 15 seconds, just so you have
  // something to test the Pi <-> band round trip with before the real
  // touchscreen button exists.
  unsigned long now = millis();
  if (now - lastFakeStatusUpdate > FAKE_STATUS_INTERVAL_MS) {
    lastFakeStatusUpdate = now;
    statusCharacteristic->setValue("The higher I get the lower I'll sink, I can't drown my demons they now know how to swim");
    statusCharacteristic->notify();
    Serial.println("Sent fake status update: The higher I get the lower I'll sink, I can't drown my demons they know how to swim");
  }
}
