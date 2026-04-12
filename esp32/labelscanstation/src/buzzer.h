#pragma once

// Initialize buzzer PWM on BUZZER_GPIO.
void buzzer_init();

// Good beep: 2kHz for 50ms.
void buzzer_beep_good();

// Bad buzz: 500Hz for 100ms.
void buzzer_beep_bad();

// Sad beep: known card but can't print (descending tone).
void buzzer_beep_sad();
