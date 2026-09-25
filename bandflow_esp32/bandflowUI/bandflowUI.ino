// I keep this touchscreen prototype independent from lvgl_v8_port.ino.

#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include "touch.h"

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 170
#define TFT_BL 38

#define COLOR_BG 0x11194A
#define COLOR_PANEL 0x2A225E
#define COLOR_PRIMARY 0x7650F5
#define COLOR_SECONDARY 0x4F6CF6
#define COLOR_SUCCESS 0x32D98A
#define COLOR_TEXT 0xF5F3FF
#define COLOR_MUTED 0x9A95C1

Arduino_DataBus *displayBus = new Arduino_ESP32SPI(
	11, 10, 12, 13, GFX_NOT_DEFINED
);
Arduino_GFX *display = new Arduino_GC9A01(
	displayBus, 1, 1, true, 170, 320, 35, 0, 35, 0
);

static lv_disp_draw_buf_t drawBuffer;
static lv_disp_drv_t displayDriver;
static lv_color_t *drawBuffer1;
static lv_color_t *drawBuffer2;
static lv_obj_t *screenRoot;
static lv_obj_t *timerLabel;
static lv_timer_t *completionTimer;
static lv_timer_t *activeTimer;
static uint16_t touchX;
static uint16_t touchY;
static bool touchWasDown = false;
static uint16_t touchStartX = 0;
static uint16_t touchStartY = 0;
static bool swipeBackRequested = false;

enum ScreenId {
	SCREEN_TASK_LIST,
	SCREEN_PRIORITY_MATRIX,
	SCREEN_SUBTASK_DETAILS,
	SCREEN_ACTIVE_SUBTASK,
	SCREEN_TASK_COMPLETE
};

struct MockSubtask {
	const char *text;
	bool done;
};

struct MockTask {
	const char *name;
	uint8_t progress;
	const char *quadrant;
	MockSubtask subtasks[3];
};

static MockTask mockTasks[] = {
	{"Prepare site report", 66, "Do First", {
		{"Collect site measurements", true},
		{"Write the safety summary", false},
		{"Send report to the team", false}
	}},
	{"Order replacement parts", 25, "Schedule", {
		{"Check the parts list", true},
		{"Confirm supplier stock", false},
		{"Place the order", false}
	}},
	{"Review equipment photos", 0, "Delegate", {
		{"Collect the latest photos", false},
		{"Mark issues for review", false},
		{"Share the review notes", false}
	}},
	{"Archive old documents", 100, "Drop", {
		{"Select expired documents", true},
		{"Move them to archive", true},
		{"Confirm the archive", true}
	}}
};

static const uint8_t taskCount = sizeof(mockTasks) / sizeof(mockTasks[0]);
static uint8_t selectedTask = 0;
static uint8_t selectedSubtask = 1;
static ScreenId currentScreen = SCREEN_TASK_LIST;
static uint32_t activeStartedAt = 0;

static lv_color_t color(uint32_t value) { return lv_color_hex(value); }

static void flushDisplay(lv_disp_drv_t *driver, const lv_area_t *area,
												 lv_color_t *colorData) {
	uint32_t width = area->x2 - area->x1 + 1;
	uint32_t height = area->y2 - area->y1 + 1;
#if (LV_COLOR_16_SWAP != 0)
	display->draw16bitBeRGBBitmap(area->x1, area->y1,
																(uint16_t *)&colorData->full, width, height);
#else
	display->draw16bitRGBBitmap(area->x1, area->y1,
															(uint16_t *)&colorData->full, width, height);
#endif
	lv_disp_flush_ready(driver);
}

static void readTouch(lv_indev_drv_t *, lv_indev_data_t *data) {
	if (CHSC6413_Scan(&touchX, &touchY) == 1) {
		data->state = LV_INDEV_STATE_PR;
		data->point.x = touchX;
		data->point.y = touchY;
		if (!touchWasDown) {
			touchWasDown = true;
			touchStartX = touchX;
			touchStartY = touchY;
			Serial.print("Touch down x=");
			Serial.print(touchX);
			Serial.print(" y=");
			Serial.println(touchY);
		}
	} else {
		data->state = LV_INDEV_STATE_REL;
		if (touchWasDown) {
			uint16_t endX = touchX;
			uint16_t endY = touchY;
			if (endX > touchStartX + 60 &&
					abs((int)endY - (int)touchStartY) < 45) {
				swipeBackRequested = true;
			}
			touchWasDown = false;
		}
	}
}

static lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, lv_coord_t x,
													 lv_coord_t y, lv_coord_t width, lv_coord_t height,
													 uint32_t textColor, uint8_t fontSize = 14) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, text);
	lv_obj_set_pos(label, x, y);
	lv_obj_set_size(label, width, height);
	lv_obj_set_style_text_color(label, color(textColor), 0);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	return label;
}

static lv_obj_t *makeButton(lv_obj_t *parent, const char *text, lv_coord_t x,
														lv_coord_t y, lv_coord_t width, lv_coord_t height,
														uint32_t background, lv_event_cb_t callback,
														void *userData = nullptr) {
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_set_pos(button, x, y);
	lv_obj_set_size(button, width, height);
	lv_obj_set_style_bg_color(button, color(background), 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(button, color(COLOR_SECONDARY), LV_STATE_PRESSED);
	lv_obj_set_style_radius(button, 8, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_t *label = makeLabel(button, text, 4, 2, width - 8, height - 4, COLOR_TEXT, 14);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, userData);
	return button;
}

static void styleRoot() {
	screenRoot = lv_obj_create(lv_scr_act());
	lv_obj_set_size(screenRoot, SCREEN_WIDTH, SCREEN_HEIGHT);
	lv_obj_set_pos(screenRoot, 0, 0);
	lv_obj_set_style_bg_color(screenRoot, color(COLOR_BG), 0);
	lv_obj_set_style_bg_opa(screenRoot, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(screenRoot, 0, 0);
	lv_obj_set_style_radius(screenRoot, 0, 0);
	lv_obj_set_style_pad_all(screenRoot, 0, 0);
	lv_obj_clear_flag(screenRoot, LV_OBJ_FLAG_SCROLLABLE);
}

static void goTo(ScreenId nextScreen);

static void onTaskCard(lv_event_t *event) {
	selectedTask = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
	selectedSubtask = 0;
	while (selectedSubtask < 3 && mockTasks[selectedTask].subtasks[selectedSubtask].done) {
		selectedSubtask++;
	}
	if (selectedSubtask >= 3) selectedSubtask = 2;
	goTo(SCREEN_SUBTASK_DETAILS);
}

static void onMatrixQuadrant(lv_event_t *event) {
	selectedTask = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
	selectedSubtask = 0;
	goTo(SCREEN_SUBTASK_DETAILS);
}

static void onOpenMatrix(lv_event_t *) { goTo(SCREEN_PRIORITY_MATRIX); }

static void onStartSubtask(lv_event_t *) {
	activeStartedAt = millis();
	goTo(SCREEN_ACTIVE_SUBTASK);
}

static void onNotYet(lv_event_t *) { }

static void goBack() {
	if (currentScreen == SCREEN_ACTIVE_SUBTASK) goTo(SCREEN_SUBTASK_DETAILS);
	else if (currentScreen == SCREEN_SUBTASK_DETAILS ||
			 currentScreen == SCREEN_PRIORITY_MATRIX) goTo(SCREEN_TASK_LIST);
	else if (currentScreen == SCREEN_TASK_COMPLETE) goTo(SCREEN_TASK_LIST);
}

static void onDone(lv_event_t *) {
	mockTasks[selectedTask].subtasks[selectedSubtask].done = true;
	mockTasks[selectedTask].progress = (uint8_t)((selectedSubtask + 1) * 33);
	if (mockTasks[selectedTask].progress > 100) mockTasks[selectedTask].progress = 100;
	if (selectedSubtask < 2) {
		selectedSubtask++;
		activeStartedAt = millis();
		goTo(SCREEN_ACTIVE_SUBTASK);
	} else {
		mockTasks[selectedTask].progress = 100;
		goTo(SCREEN_TASK_COMPLETE);
	}
}

static void returnToTaskList(lv_timer_t *) { goTo(SCREEN_TASK_LIST); }

static void updateActiveTimer(lv_timer_t *) {
	if (timerLabel == nullptr) return;
	uint32_t seconds = (millis() - activeStartedAt) / 1000;
	char timeText[16];
	snprintf(timeText, sizeof(timeText), "%02lu:%02lu", seconds / 60, seconds % 60);
	lv_label_set_text(timerLabel, timeText);
}

static void drawHeader(const char *title, bool showMatrixButton) {
	makeLabel(screenRoot, title, 12, 7, 190, 22, COLOR_TEXT, 16);
	if (showMatrixButton) {
		makeButton(screenRoot, "MATRIX", 235, 6, 73, 23, COLOR_PRIMARY, onOpenMatrix);
	}
}

static void drawTaskList() {
	drawHeader("MY TASKS", true);
	lv_obj_t *list = lv_obj_create(screenRoot);
	lv_obj_set_pos(list, 8, 31);
	lv_obj_set_size(list, 304, 132);
	lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_pad_all(list, 0, 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);

	for (uint8_t index = 0; index < taskCount; index++) {
		lv_obj_t *card = lv_btn_create(list);
		lv_obj_set_pos(card, 0, index * 48);
		lv_obj_set_size(card, 296, 43);
		lv_obj_set_style_bg_color(card, color(COLOR_PANEL), 0);
		lv_obj_set_style_bg_color(card, color(COLOR_SECONDARY), LV_STATE_PRESSED);
		lv_obj_set_style_radius(card, 7, 0);
		lv_obj_set_style_border_width(card, 0, 0);
		lv_obj_add_event_cb(card, onTaskCard, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
		makeLabel(card, mockTasks[index].name, 12, 5, 205, 18, COLOR_TEXT, 14);

		char progressText[8];
		snprintf(progressText, sizeof(progressText), "%u%%", mockTasks[index].progress);
		makeLabel(card, progressText, 245, 5, 42, 16, COLOR_MUTED, 12);
		lv_obj_t *bar = lv_bar_create(card);
		lv_obj_set_pos(bar, 12, 29);
		lv_obj_set_size(bar, 273, 7);
		lv_obj_set_style_bg_color(bar, color(0x423A76), LV_PART_MAIN);
		lv_obj_set_style_bg_color(bar, color(COLOR_SECONDARY), LV_PART_INDICATOR);
		lv_bar_set_value(bar, mockTasks[index].progress, LV_ANIM_OFF);
	}
}

static void drawPriorityMatrix() {
	makeButton(screenRoot, "TASKS", 12, 6, 67, 23, COLOR_PRIMARY, [](lv_event_t *) { goTo(SCREEN_TASK_LIST); });
	makeLabel(screenRoot, "PRIORITY MATRIX", 91, 8, 150, 20, COLOR_TEXT, 14);
	const char *titles[] = {"DO FIRST", "SCHEDULE", "DELEGATE", "DROP"};
	const uint32_t borders[] = {COLOR_PRIMARY, COLOR_SECONDARY, COLOR_SUCCESS, COLOR_MUTED};
	const uint8_t taskIndexes[] = {0, 1, 2, 3};
	for (uint8_t index = 0; index < 4; index++) {
		uint8_t column = index % 2;
		uint8_t row = index / 2;
		lv_obj_t *quadrant = lv_btn_create(screenRoot);
		lv_obj_set_pos(quadrant, 9 + column * 153, 34 + row * 61);
		lv_obj_set_size(quadrant, 146, 55);
		lv_obj_set_style_bg_color(quadrant, color(COLOR_PANEL), 0);
		lv_obj_set_style_bg_color(quadrant, color(COLOR_SECONDARY), LV_STATE_PRESSED);
		lv_obj_set_style_border_color(quadrant, color(borders[index]), 0);
		lv_obj_set_style_border_width(quadrant, 1, 0);
		lv_obj_set_style_radius(quadrant, 7, 0);
		lv_obj_add_event_cb(quadrant, onMatrixQuadrant, LV_EVENT_CLICKED, (void *)(uintptr_t)taskIndexes[index]);
		makeLabel(quadrant, titles[index], 7, 4, 132, 15, borders[index], 12);
		makeLabel(quadrant, mockTasks[taskIndexes[index]].name, 7, 22, 132, 26, COLOR_TEXT, 12);
	}
}

static void drawSubtaskDetails() {
	makeButton(screenRoot, "BACK", 10, 6, 58, 23, COLOR_PANEL, [](lv_event_t *) { goTo(SCREEN_TASK_LIST); });
	makeLabel(screenRoot, mockTasks[selectedTask].name, 78, 8, 225, 20, COLOR_TEXT, 14);
	lv_obj_t *checklist = lv_obj_create(screenRoot);
	lv_obj_set_pos(checklist, 8, 31);
	lv_obj_set_size(checklist, 304, 101);
	lv_obj_set_style_bg_opa(checklist, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(checklist, 0, 0);
	lv_obj_set_style_pad_all(checklist, 0, 0);
	lv_obj_set_scroll_dir(checklist, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(checklist, LV_SCROLLBAR_MODE_AUTO);
	for (uint8_t index = 0; index < 3; index++) {
		lv_obj_t *check = lv_checkbox_create(checklist);
		lv_checkbox_set_text(check, mockTasks[selectedTask].subtasks[index].text);
		lv_obj_set_pos(check, 4, index * 36);
		lv_obj_set_size(check, 290, 32);
		lv_obj_set_style_pad_column(check, 9, 0);
		lv_obj_set_style_bg_color(check, color(COLOR_PANEL), 0);
		lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(check, 6, 0);
		lv_obj_set_style_pad_left(check, 8, 0);
		lv_obj_set_style_pad_right(check, 5, 0);
		lv_obj_set_style_border_width(check, index == selectedSubtask ? 1 : 0, 0);
		lv_obj_set_style_border_color(check, color(COLOR_SECONDARY), 0);
		lv_obj_set_style_bg_color(check, color(COLOR_SUCCESS), LV_PART_INDICATOR | LV_STATE_CHECKED);
		lv_obj_set_style_width(check, 25, LV_PART_INDICATOR);
		lv_obj_set_style_height(check, 25, LV_PART_INDICATOR);
		lv_obj_set_style_radius(check, 4, LV_PART_INDICATOR);
		lv_obj_set_style_text_color(check, index == selectedSubtask ? color(COLOR_TEXT) : color(COLOR_MUTED), 0);
		if (mockTasks[selectedTask].subtasks[index].done) lv_obj_add_state(check, LV_STATE_CHECKED);
	}
	makeButton(screenRoot, "Start", 65, 138, 190, 28, COLOR_PRIMARY, onStartSubtask);
}

static void drawActiveSubtask() {
	makeLabel(screenRoot, "ACTIVE SUBTASK", 0, 8, 320, 18, COLOR_MUTED, 12);
	lv_obj_set_style_text_align(lv_obj_get_child(screenRoot, 0), LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_t *taskText = makeLabel(screenRoot, mockTasks[selectedTask].subtasks[selectedSubtask].text,
																 22, 31, 276, 42, COLOR_TEXT, 16);
	lv_obj_set_style_text_align(taskText, LV_TEXT_ALIGN_CENTER, 0);
	timerLabel = makeLabel(screenRoot, "00:00", 0, 76, 320, 22, COLOR_SECONDARY, 16);
	lv_obj_set_style_text_align(timerLabel, LV_TEXT_ALIGN_CENTER, 0);
	makeButton(screenRoot, "Not yet", 8, 108, 150, 54, COLOR_PANEL, onNotYet);
	makeButton(screenRoot, "Done", 162, 108, 150, 54, COLOR_SUCCESS, onDone);
	activeTimer = lv_timer_create(updateActiveTimer, 1000, nullptr);
}

static void drawTaskComplete() {
	lv_obj_t *check = lv_label_create(screenRoot);
	lv_label_set_text(check, LV_SYMBOL_OK);
	lv_obj_set_style_text_color(check, color(COLOR_SUCCESS), 0);
	lv_obj_set_style_text_font(check, &lv_font_montserrat_14, 0);
	lv_obj_align(check, LV_ALIGN_TOP_MID, 0, 21);
	lv_obj_t *title = makeLabel(screenRoot, "TASK COMPLETE", 0, 57, 320, 20, COLOR_TEXT, 16);
	lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_t *name = makeLabel(screenRoot, mockTasks[selectedTask].name, 0, 83, 320, 18, COLOR_MUTED, 14);
	lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_t *hint = makeLabel(screenRoot, "Returning to task list...", 0, 126, 320, 18, COLOR_MUTED, 12);
	lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
	completionTimer = lv_timer_create(returnToTaskList, 2000, nullptr);
	lv_timer_set_repeat_count(completionTimer, 1);
}

static void goTo(ScreenId nextScreen) {
	if (activeTimer != nullptr) {
		lv_timer_del(activeTimer);
		activeTimer = nullptr;
	}
	if (completionTimer != nullptr) {
		lv_timer_del(completionTimer);
		completionTimer = nullptr;
	}
	if (screenRoot != nullptr) lv_obj_del(screenRoot);
	currentScreen = nextScreen;
	styleRoot();
	if (nextScreen == SCREEN_TASK_LIST) drawTaskList();
	else if (nextScreen == SCREEN_PRIORITY_MATRIX) drawPriorityMatrix();
	else if (nextScreen == SCREEN_SUBTASK_DETAILS) drawSubtaskDetails();
	else if (nextScreen == SCREEN_ACTIVE_SUBTASK) drawActiveSubtask();
	else drawTaskComplete();
}

static void setupDisplay() {
	display->begin(80000000);
	display->invertDisplay(true);
	pinMode(TFT_BL, OUTPUT);
	digitalWrite(TFT_BL, HIGH);
	CHSC6413_init();
	lv_init();

	drawBuffer1 = (lv_color_t *)heap_caps_malloc(
		sizeof(lv_color_t) * SCREEN_WIDTH * SCREEN_HEIGHT / 8,
		MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
	);
	drawBuffer2 = (lv_color_t *)heap_caps_malloc(
		sizeof(lv_color_t) * SCREEN_WIDTH * SCREEN_HEIGHT / 8,
		MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
	);
	if (drawBuffer1 == nullptr || drawBuffer2 == nullptr) {
		Serial.println("LVGL buffer allocation failed");
		while (true) delay(1000);
	}
	lv_disp_draw_buf_init(&drawBuffer, drawBuffer1, drawBuffer2,
												SCREEN_WIDTH * SCREEN_HEIGHT / 8);
	lv_disp_drv_init(&displayDriver);
	displayDriver.hor_res = SCREEN_WIDTH;
	displayDriver.ver_res = SCREEN_HEIGHT;
	displayDriver.flush_cb = flushDisplay;
	displayDriver.draw_buf = &drawBuffer;
	lv_disp_drv_register(&displayDriver);

	static lv_indev_drv_t inputDriver;
	lv_indev_drv_init(&inputDriver);
	inputDriver.type = LV_INDEV_TYPE_POINTER;
	inputDriver.read_cb = readTouch;
	lv_indev_drv_register(&inputDriver);
}

void setup() {
	Serial.begin(115200);
	delay(1000);
	Serial.println("Starting BandFlow UI...");
	setupDisplay();
	goTo(SCREEN_TASK_LIST);
	Serial.println("BandFlow UI ready.");
}

void loop() {
	lv_tick_inc(5);
	lv_timer_handler();
	if (swipeBackRequested) {
		swipeBackRequested = false;
		goBack();
	}
	delay(5);
}
