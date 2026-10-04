const int micPin = A0;
const unsigned long sampleRate = 8000;
const unsigned long samplePeriod = 1000000UL / sampleRate;
const int oversample = 4;  // 4 x 10-bit reads summed -> 12-bit result (0..4092)

unsigned long nextSample;

void setup() {
  Serial.begin(250000);
  // ADC clock 16 MHz / 16 = 1 MHz: ~13 us per read, so 4 reads fit in one 125 us sample
  ADCSRA = (ADCSRA & ~0x07) | 0x04;
  nextSample = micros();
}

void loop() {
  unsigned int sum = 0;
  for (int i = 0; i < oversample; i++) {
    sum += analogRead(micPin);
  }

  // Frame: first byte has bit 7 set (header), second byte has bit 7 clear,
  // so the receiver can always find frame boundaries after a dropped byte.
  Serial.write(0x80 | (sum >> 6));
  Serial.write(sum & 0x3F);

  // Fixed-schedule timing so the rate doesn't drift with loop overhead
  nextSample += samplePeriod;
  while ((long)(micros() - nextSample) < 0) {
  }
}
