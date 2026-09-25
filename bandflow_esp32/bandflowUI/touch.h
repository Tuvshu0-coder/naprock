/*******************************************************************************
 * CHSC6413 touch driver for the UEDX17320019E-WB-A display.
 ******************************************************************************/

#ifndef BANDFLOW_TOUCH_H
#define BANDFLOW_TOUCH_H

#include <Wire.h>
#include "Arduino.h"

#define TOUCH_SDA 9
#define TOUCH_SCL 46
#define TOUCH_INT 8
#define TOUCH_RST 3
#define TOUCH_WIDTH 320
#define TOUCH_HEIGHT 170
#define CHSC6413_ADDR (uint8_t)(0x2E)

#define X_MIRRORING 0
#define Y_MIRRORING 1

static void touchReset() {
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  delay(50);
  pinMode(TOUCH_INT, INPUT);
}

static uint8_t readTouchRegisters32(uint32_t reg, uint8_t *buffer, uint8_t size) {
  Wire.beginTransmission(CHSC6413_ADDR);
  Wire.write((uint8_t)(reg >> 24));
  Wire.write((uint8_t)(reg >> 16));
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)reg);
  if (Wire.endTransmission() != 0) return 0;

  uint8_t received = Wire.requestFrom(CHSC6413_ADDR, size);
  if (received != size) return 0;
  for (uint8_t index = 0; index < size; index++) buffer[index] = Wire.read();
  return 1;
}

static void CHSC6413_init() {
  Wire.setPins(TOUCH_SDA, TOUCH_SCL);
  Wire.begin();
  touchReset();
}

static uint8_t CHSC6413_Scan(uint16_t *x, uint16_t *y) {
  uint8_t data[3] = {0};
  if (!readTouchRegisters32(0x2000002C, data, sizeof(data))) return 0;

  uint8_t handState = data[0] & 0x30;
  if (handState != 0x00 && handState != 0x20) return 0;

  uint16_t controllerX = (uint16_t)(((data[0] & 0x40) << 2) | data[1]);
  uint16_t controllerY = (uint16_t)(((data[0] & 0x80) << 1) | data[2]);
  uint16_t screenX = controllerY;
  uint16_t screenY = controllerX;

  *x = X_MIRRORING ? (TOUCH_WIDTH - 1 - screenX) : screenX;
  *y = Y_MIRRORING ? (TOUCH_HEIGHT - 1 - screenY) : screenY;
  if (*x >= TOUCH_WIDTH || *y >= TOUCH_HEIGHT) return 0;
  return 1;
}

#endif
