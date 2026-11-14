#ifndef PWM_H
#define PWM_H

#include <stdint.h>

// Press-machine simulation: continuous PWM toggling for the given duration (ms)
void PWM_test_run_press(void *arg);

// CNC simulation: drives the pin HIGH for a full 60 seconds, then LOW. One-shot.
void PWM_test_run_cnc(void);

#endif
