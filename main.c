#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"

#define STEPS_IN_EACH_REVOLUTION 4096
#define DISPENSE_INTERVAL_MS 30000
#define MAX_DISPENSE_COUNT 7
#define CALIBRATION_TIMEOUT_MS 3000
#define ERR_BLINK_DELAY_MS 500

/* Pin Definitions */
const uint led_pin = 22;
const uint button1_pin = 7;

/* Motor Pins Definition (ULN2003 Driver to 28BYJ-48 Stepper) */
const uint Stepper_coilD = 2;
const uint Stepper_coilC = 3;
const uint Stepper_coilB = 6;
const uint Stepper_coilA = 13;

/* Sensor Pins */
const uint opto_pin = 28;
const uint piezo_pin = 27;

/* State Machine Definition */
enum State {
    WAITING_TO_CALIBRATE,
    CALIBRATING,
    WAITING_TO_DISPENSE,
    DISPENSING,
    DISPENSED,
    ERROR
};

enum State current_state = WAITING_TO_CALIBRATE;

int dispense_count = 0;
volatile bool is_calibrated = false;

static volatile uint stepIndex = 0;
static volatile uint revIndex = 0;
static uint16_t adc_value = 0;
int prevValue = 0;
static uint calib_index = 0;
static volatile bool FallingEdge = false;
static volatile uint fill = 0;

/* Function prototypes */
void calibrate_motor(void);
void run_motor(int steps);
void opto_falling_edge(uint gpio, uint32_t events);
void reset_dispenser(void);
void dispense_pills(void);
void check_piezo_sensor(void);
void handle_led_state(void);
void control_led(bool state);

/* 8-step half-stepping sequence */
static const uint HalfStep_Sequence[8][4] = {
    {1, 0, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 0, 0},
    {0, 1, 1, 0},
    {0, 0, 1, 0},
    {0, 0, 1, 1},
    {0, 0, 0, 1},
    {1, 0, 0, 1}
};

/*----------------- Main Section ------------------------------------ */
int main() {
    stdio_init_all();

    // Initialize LED pin
    gpio_init(led_pin);
    gpio_set_dir(led_pin, GPIO_OUT);

    // Initialize button pin
    gpio_init(button1_pin);
    gpio_set_dir(button1_pin, GPIO_IN);
    gpio_pull_up(button1_pin);

    // Initialize motor driver outputs
    gpio_init(Stepper_coilD);
    gpio_set_dir(Stepper_coilD, GPIO_OUT);
    gpio_init(Stepper_coilC);
    gpio_set_dir(Stepper_coilC, GPIO_OUT);
    gpio_init(Stepper_coilB);
    gpio_set_dir(Stepper_coilB, GPIO_OUT);
    gpio_init(Stepper_coilA);
    gpio_set_dir(Stepper_coilA, GPIO_OUT);

    // Initialize opto-fork sensor with falling-edge interrupt
    gpio_init(opto_pin);
    gpio_set_dir(opto_pin, GPIO_IN);
    gpio_pull_up(opto_pin);
    gpio_set_irq_enabled_with_callback(opto_pin, GPIO_IRQ_EDGE_FALL, true, &opto_falling_edge);

    // Initialize piezo sensor
    gpio_init(piezo_pin);
    gpio_set_dir(piezo_pin, GPIO_IN);
    gpio_pull_up(piezo_pin);

    while (true) {
        switch (current_state) {
            case WAITING_TO_CALIBRATE:
                handle_led_state();
                // Waiting for button press to start calibration
                if (gpio_get(button1_pin) == 0) {
                    printf("Button is pressed\n");
                    printf("Calibration will start \n");
                    current_state = CALIBRATING;
                    calibrate_motor();
                }
                break;

            case CALIBRATING:
                // Waiting for calibration to complete
                if (is_calibrated) {
                    printf("Calibration completed \n");
                    control_led(true);
                    current_state = WAITING_TO_DISPENSE;
                }
                break;

            case WAITING_TO_DISPENSE:
                // Waiting for another button press to begin dispensing
                control_led(true);
                if (gpio_get(button1_pin) == 0) {
                    current_state = DISPENSING;
                }
                break;

            case DISPENSING:
                printf("Starting dispensing cycle\n");
                control_led(true);

                for (int i = 1; i <= MAX_DISPENSE_COUNT; i++) {
                    dispense_pills();
                    printf("Dispensed dose %d \n", i);

                    if (i == MAX_DISPENSE_COUNT) {
                        printf("Dispensing round completed\n");
                        current_state = DISPENSED;
                        break;
                    }
                    sleep_ms(DISPENSE_INTERVAL_MS);
                }
                break;

            case DISPENSED:
                control_led(true);
                printf("Dispenser will now reset \n");
                reset_dispenser();
                break;

            case ERROR:
                handle_led_state();
                break;
        }
        sleep_ms(100);
    }

    return 0;
}

void calibrate_motor(void) {
    is_calibrated = false;
    FallingEdge = false;
    while (FallingEdge == false) {
        run_motor(1);
    }
    FallingEdge = false;
    while (FallingEdge == false) {
        run_motor(1);
    }

    is_calibrated = true;
    printf("Calibration is completed \n");
    printf("Number of steps per revolution: %u\n", calib_index);
}

void run_motor(int steps) {
    for (int i = 0; i < steps; i++) {
        gpio_put(Stepper_coilD, HalfStep_Sequence[stepIndex][0]);
        gpio_put(Stepper_coilC, HalfStep_Sequence[stepIndex][1]);
        gpio_put(Stepper_coilB, HalfStep_Sequence[stepIndex][2]);
        gpio_put(Stepper_coilA, HalfStep_Sequence[stepIndex++][3]);
        revIndex++;
        if (stepIndex >= 8) {
            stepIndex = 0;
        }
        sleep_ms(4);
    }
}

void reset_dispenser(void) {
    dispense_count = 0;
    printf("..dispense count %d\n", dispense_count);
    is_calibrated = false;
    current_state = WAITING_TO_CALIBRATE;
}

void dispense_pills(void) {
    int runTime = 1;
    if (is_calibrated == true) {
        uint steps_to_run = (calib_index > 0) ? (runTime * calib_index / 8) : (STEPS_IN_EACH_REVOLUTION / 8);
        run_motor(steps_to_run);
        check_piezo_sensor();
    } else {
        printf("Run motor fail: motor not calibrated.\n");
    }
}

void check_piezo_sensor(void) {
    static bool pill_dispensed = false;
    adc_value = gpio_get(piezo_pin);
    if (adc_value != prevValue) {
        if (!adc_value) {
            printf("Pill impact detected on piezo sensor!\n");
            pill_dispensed = true;
        }
    }
    if (pill_dispensed) {
        dispense_count++;
        pill_dispensed = false;
        if (dispense_count >= MAX_DISPENSE_COUNT) {
            current_state = DISPENSED;
        }
    }
    prevValue = adc_value;
}

void opto_falling_edge(uint gpio, uint32_t events) {
    if (current_state == CALIBRATING) {
        FallingEdge = true;
        if (is_calibrated == false) {
            calib_index = revIndex;
        }
        revIndex = 0;
        is_calibrated = true;
    } else if (current_state == DISPENSING) {
        printf("Amount dispensed: %d\n", dispense_count);
        dispense_count++;
        if (dispense_count >= MAX_DISPENSE_COUNT) {
            printf("Dispense completed\n");
            current_state = DISPENSED;
        }
    }
}

void control_led(bool state) {
    gpio_put(led_pin, state);
}

void handle_led_state(void) {
    static int error_blink_count = 0;

    switch (current_state) {
        case WAITING_TO_CALIBRATE:
            // LED blinks at 1 Hz while waiting for user start command
            if (time_us_64() % 1000000 < 500000) {
                control_led(true);
            } else {
                control_led(false);
            }
            break;

        case CALIBRATING:
        case DISPENSING:
            control_led(true);
            break;

        case WAITING_TO_DISPENSE:
            control_led(false);
            break;

        case DISPENSED:
            reset_dispenser();
            break;

        case ERROR:
            // LED blinks 5 times upon an error condition
            if (error_blink_count < 5) {
                if (time_us_64() % 1000000 < 500000) {
                    control_led(true);
                } else {
                    control_led(false);
                }
            } else {
                control_led(true);
            }
            break;
    }
}

