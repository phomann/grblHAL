/*

  odometer.c - axis odometers including run time + spindle run time

  Part of GrblHAL

  Copyright (c) 2020 Terje Io

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifdef ARDUINO
#include "../driver.h"
#else
#include "driver.h"
#endif

#if ODOMETER_ENABLE

#include <string.h>
#include <stdio.h>

#ifdef ARDUINO
#include "../grbl/protocol.h"
#include "../grbl/nvs_buffer.h"
#else
#include "grbl/protocol.h"
#include "grbl/nvs_buffer.h"
#endif

typedef struct {
    uint64_t motors;
    uint64_t spindle;
    float distance[N_AXIS];
} odometer_data_t;

static uint32_t steps[N_AXIS] = {0};
static bool odometer_changed = false;
static uint32_t odometers_address, odometers_address_prv;
static odometer_data_t odometers, odometers_prv;
static nvs_io_t nvs;
static stepper_pulse_start_ptr stepper_pulse_start;
static on_unknown_sys_command_ptr on_unknown_sys_command;
static on_state_change_ptr on_state_change;
static spindle_set_state_ptr spindle_set_state_;
static settings_changed_ptr settings_changed;
static on_report_options_ptr on_report_options;

static void stepperPulseStart (stepper_t *stepper)
{
    odometer_changed = true;

    if(stepper->step_outbits.x)
        steps[X_AXIS]++;

    if(stepper->step_outbits.y)
        steps[Y_AXIS]++;

    if(stepper->step_outbits.z)
        steps[Z_AXIS]++;

#ifdef A_AXIS
    if(stepper->step_outbits.a)
        steps[A_AXIS]++;
#endif

#ifdef B_AXIS
    if(stepper->step_outbits.b)
        steps[B_AXIS]++;
#endif

#ifdef C_AXIS
    if(stepper->step_outbits.b)
        steps[C_AXIS]++;
#endif

    stepper_pulse_start(stepper);
}

void onStateChanged (uint_fast16_t state)
{
    static uint32_t ms = 0;

    if(state & (STATE_CYCLE|STATE_JOG|STATE_HOMING))
        ms = hal.get_elapsed_ticks();

    else if(odometer_changed) {

        uint_fast8_t idx = N_AXIS;

        odometer_changed = false;
        odometers.motors += (hal.get_elapsed_ticks() - ms);

        do {
            if(steps[--idx]) {
                odometers.distance[idx] += (float)steps[idx] / settings.axis[idx].steps_per_mm;
                steps[idx] = 0;
            }
        } while(idx);

        nvs.memcpy_to_nvs(odometers_address, (uint8_t *)&odometers, sizeof(odometer_data_t), true);
    }

    if(on_state_change)
        on_state_change(state);
}

// Called by foreground process.
static void odometers_write (uint_fast16_t state)
{
    nvs.memcpy_to_nvs(odometers_address, (uint8_t *)&odometers, sizeof(odometer_data_t), true);
}

ISR_CODE static void onSpindleSetState (spindle_state_t state, float rpm)
{
    static uint32_t ms = 0;

    spindle_set_state_(state, rpm);

    if(state.on)
        ms = hal.get_elapsed_ticks();
    else if(ms) {
        odometers.spindle += (hal.get_elapsed_ticks() - ms);
        ms = 0;
        // Write odometer data in foreground process.
        protocol_enqueue_rt_command(odometers_write);
    }
}

// Reclaim entry points that may have been changed on settings change.
static void onSettingsChanged (settings_t *settings)
{
    settings_changed(settings);

    if(hal.spindle.set_state != onSpindleSetState) {
        spindle_set_state_ = hal.spindle.set_state;
        hal.spindle.set_state = onSpindleSetState;
    }

    if(hal.stepper.pulse_start != stepperPulseStart) {
        stepper_pulse_start = hal.stepper.pulse_start;
        hal.stepper.pulse_start = stepperPulseStart;
    }
}

static void odometer_data_reset (bool backup)
{
    if(backup) {
        memcpy(&odometers_prv, &odometers, sizeof(odometer_data_t));
        nvs.memcpy_to_nvs(odometers_address_prv, (uint8_t *)&odometers_prv, sizeof(odometer_data_t), true);
    }
    memset(&odometers, 0, sizeof(odometer_data_t));
    nvs.memcpy_to_nvs(odometers_address, (uint8_t *)&odometers, sizeof(odometer_data_t), true);
}

static void odometers_report (odometer_data_t *odometers)
{
    char buf[40];
    uint_fast8_t idx;
    uint32_t hr = odometers->spindle / 3600000, min = (odometers->spindle / 60000) % 60;

    sprintf(buf, "SPINDLEHRS %ld:%.2ld", hr, min);
    report_message(buf, Message_Plain);

    hr = odometers->motors / 3600000;
    min = (odometers->motors / 60000) % 60;

    sprintf(buf, "MOTORHRS %ld:%.2ld", hr, min);
    report_message(buf, Message_Plain);

    for(idx = 0 ; idx < N_AXIS ; idx++) {
        sprintf(buf, "ODOMETER%s %s", axis_letter[idx], ftoa(odometers->distance[idx] / 1000.0f, 1)); // meters
        report_message(buf, Message_Plain);
    }
}

static status_code_t commandExecute (uint_fast16_t state, char *line, char *lcline)
{
    status_code_t retval = Status_Unhandled;

    if(line[1] == 'O') {

        if(!strcmp(&line[1], "ODOMETERS")) {
            odometers_report(&odometers);
            retval = Status_OK;
        }

        if(!strcmp(&line[1], "ODOMETERS=PREV")) {
            if(nvs.memcpy_from_nvs((uint8_t *)&odometers_prv, odometers_address_prv, sizeof(odometer_data_t), true) == NVS_TransferResult_OK)
                odometers_report(&odometers_prv);
            else
                report_message("Previous odometer values not available", Message_Warning);
            retval = Status_OK;
        }

        if(!strcmp(&line[1], "ODOMETERS=RST")) {
            odometer_data_reset(true);
            retval = Status_OK;
        }
    }

    return retval == Status_Unhandled && on_unknown_sys_command ? on_unknown_sys_command(state, line, lcline) : retval;
}

static void onReportOptions (void)
{
    on_report_options();
    hal.stream.write("[PLUGIN:ODOMETERS v0.01]"  ASCII_EOL);
}

static void odometer_warning1 (uint_fast16_t state)
{
    report_message("EEPROM or FRAM is required for odometers!", Message_Warning);
}

static void odometer_warning2 (uint_fast16_t state)
{
    report_message("Not enough NVS storage for odometers!", Message_Warning);
}

void odometer_init()
{
    memcpy(&nvs, nvs_buffer_get_physical(), sizeof(nvs_io_t));

    if(!(nvs.type == NVS_EEPROM || nvs.type == NVS_FRAM))
        protocol_enqueue_rt_command(odometer_warning1);
    else if(NVS_SIZE - GRBL_NVS_SIZE - hal.nvs.driver_area.size < (sizeof(odometer_data_t) * 2 + 2))
        protocol_enqueue_rt_command(odometer_warning2);
    else {

        odometers_address = NVS_SIZE - (sizeof(odometer_data_t) + 1);
        odometers_address_prv = odometers_address - (sizeof(odometer_data_t) + 1);

        if(nvs.memcpy_from_nvs((uint8_t *)&odometers, odometers_address, sizeof(odometer_data_t), true) != NVS_TransferResult_OK)
            odometer_data_reset(false);

        on_unknown_sys_command = grbl.on_unknown_sys_command;
        grbl.on_unknown_sys_command = commandExecute;

        on_state_change = grbl.on_state_change;
        grbl.on_state_change = onStateChanged;

        on_report_options = grbl.on_report_options;
        grbl.on_report_options = onReportOptions;

        settings_changed = hal.settings_changed;
        hal.settings_changed = onSettingsChanged;

        spindle_set_state_ = hal.spindle.set_state;
        hal.spindle.set_state = onSpindleSetState;

        stepper_pulse_start = hal.stepper.pulse_start;
        hal.stepper.pulse_start = stepperPulseStart;
    }
}

#endif
