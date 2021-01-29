/*
  report.c - reporting and messaging methods

  Part of grblHAL

  Copyright (c) 2017-2021 Terje Io
  Copyright (c) 2012-2016 Sungeun K. Jeon for Gnea Research LLC

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

/*
  This file functions as the primary feedback interface for Grbl. Any outgoing data, such
  as the protocol status messages, feedback messages, and status reports, are stored here.
  For the most part, these functions primarily are called from protocol.c methods. If a
  different style feedback is desired (i.e. JSON), then a user can change these following
  methods to accomodate their needs.
*/

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "hal.h"
#include "report.h"
#include "nvs_buffer.h"
#include "state_machine.h"

#ifdef ENABLE_SPINDLE_LINEARIZATION
#include <stdio.h>
#endif

#ifndef REPORT_OVERRIDE_REFRESH_BUSY_COUNT
#define REPORT_OVERRIDE_REFRESH_BUSY_COUNT 20   // (1-255)
#endif
#ifndef REPORT_OVERRIDE_REFRESH_IDLE_COUNT
#define REPORT_OVERRIDE_REFRESH_IDLE_COUNT 10   // (1-255) Must be less than or equal to the busy count
#endif
#ifndef REPORT_WCO_REFRESH_BUSY_COUNT
#define REPORT_WCO_REFRESH_BUSY_COUNT 30        // (2-255)
#endif
#ifndef REPORT_WCO_REFRESH_IDLE_COUNT
#define REPORT_WCO_REFRESH_IDLE_COUNT 10        // (2-255) Must be less than or equal to the busy count
#endif

// Compile-time sanity check of defines

#if (REPORT_WCO_REFRESH_BUSY_COUNT < REPORT_WCO_REFRESH_IDLE_COUNT)
  #error "WCO busy refresh is less than idle refresh."
#endif
#if (REPORT_OVERRIDE_REFRESH_BUSY_COUNT < REPORT_OVERRIDE_REFRESH_IDLE_COUNT)
  #error "Override busy refresh is less than idle refresh."
#endif
#if (REPORT_WCO_REFRESH_IDLE_COUNT < 2)
  #error "WCO refresh must be greater than one."
#endif
#if (REPORT_OVERRIDE_REFRESH_IDLE_COUNT < 1)
  #error "Override refresh must be greater than zero."
#endif

static char buf[(STRLEN_COORDVALUE + 1) * N_AXIS];
static char *(*get_axis_values)(float *axis_values);
static char *(*get_axis_value)(float value);
static char *(*get_rate_value)(float value);
static uint8_t override_counter = 0; // Tracks when to add override data to status reports.
static uint8_t wco_counter = 0;      // Tracks when to add work coordinate offset data to status reports.
static const char vbar[2] = { '|', '\0' };

static bool report_setting (const setting_detail_t *setting, uint_fast16_t offset, void *data);

static const report_t report_fns = {
    .setting = report_setting,
    .status_message = report_status_message,
    .feedback_message = report_feedback_message
};

// Append a number of strings to the static buffer
// NOTE: do NOT use for several int/float conversions as these share the same underlying buffer!
static char *appendbuf (int argc, ...)
{
    char c, *s = buf, *arg;

    va_list list;
    va_start(list, argc);

    while(argc--) {
        arg = va_arg(list, char *);
        do {
            c = *s++ = *arg++;
        } while(c);
        s--;
    }

    va_end(list);

    return buf;
}

static char *map_coord_system (coord_system_id_t id)
{
    uint8_t g5x = id + 54;

    strcpy(buf, uitoa((uint32_t)(g5x > 59 ? 59 : g5x)));
    if(g5x > 59) {
        strcat(buf, ".");
        strcat(buf, uitoa((uint32_t)(g5x - 59)));
    }

    return buf;
}

// Convert axis position values to null terminated string (mm).
static char *get_axis_values_mm (float *axis_values)
{
    uint_fast32_t idx;

    buf[0] = '\0';

    for (idx = 0; idx < N_AXIS; idx++) {
        if(idx == X_AXIS && gc_state.modal.diameter_mode)
            strcat(buf, ftoa(axis_values[idx] * 2.0f, N_DECIMAL_COORDVALUE_MM));
        else
            strcat(buf, ftoa(axis_values[idx], N_DECIMAL_COORDVALUE_MM));
        if (idx < (N_AXIS - 1))
            strcat(buf, ",");
    }

    return buf;
}

// Convert axis position values to null terminated string (inch).
static char *get_axis_values_inches (float *axis_values)
{
    uint_fast32_t idx;

    buf[0] = '\0';

    for (idx = 0; idx < N_AXIS; idx++) {
        if(idx == X_AXIS && gc_state.modal.diameter_mode)
            strcat(buf, ftoa(axis_values[idx] * INCH_PER_MM * 2.0f, N_DECIMAL_COORDVALUE_INCH));
        else
            strcat(buf, ftoa(axis_values[idx] * INCH_PER_MM, N_DECIMAL_COORDVALUE_INCH));
        if (idx < (N_AXIS - 1))
            strcat(buf, ",");
    }

    return buf;
}

// Convert rate value to null terminated string (mm).
static char *get_axis_value_mm (float value)
{
    return strcpy(buf, ftoa(value, N_DECIMAL_COORDVALUE_MM));
}

// Convert rate value to null terminated string (mm).
static char *get_axis_value_inches (float value)
{
    return strcpy(buf, ftoa(value * INCH_PER_MM, N_DECIMAL_COORDVALUE_INCH));
}

// Convert rate value to null terminated string (mm).
static char *get_rate_value_mm (float value)
{
    return uitoa((uint32_t)value);
}

// Convert rate value to null terminated string (mm).
static char *get_rate_value_inch (float value)
{
    return uitoa((uint32_t)(value * INCH_PER_MM));
}

// Convert axes signals bits to string representation
// NOTE: returns pointer to null terminator!
inline static char *axis_signals_tostring (char *buf, axes_signals_t signals)
{
    if(signals.x)
        *buf++ = 'X';

    if(signals.y)
        *buf++ = 'Y';

    if (signals.z)
        *buf++ = 'Z';

#ifdef A_AXIS
    if (signals.a)
        *buf++ = 'A';
#endif

#ifdef B_AXIS
    if (signals.b)
        *buf++ = 'B';
#endif

#ifdef C_AXIS
    if (signals.c)
        *buf++ = 'C';
#endif

    *buf = '\0';

    return buf;
}

void report_init (void)
{
    get_axis_value = settings.flags.report_inches ? get_axis_value_inches : get_axis_value_mm;
    get_axis_values = settings.flags.report_inches ? get_axis_values_inches : get_axis_values_mm;
    get_rate_value = settings.flags.report_inches ? get_rate_value_inch : get_rate_value_mm;
}

void report_init_fns (void)
{
    memcpy(&grbl.report, &report_fns, sizeof(report_t));
}

// Handles the primary confirmation protocol response for streaming interfaces and human-feedback.
// For every incoming line, this method responds with an 'ok' for a successful command or an
// 'error:'  to indicate some error event with the line or some critical system error during
// operation. Errors events can originate from the g-code parser, settings module, or asynchronously
// from a critical error, such as a triggered hard limit. Interface should always monitor for these
// responses.
status_code_t report_status_message (status_code_t status_code)
{
    switch(status_code) {

        case Status_OK: // STATUS_OK
            hal.stream.write("ok" ASCII_EOL);
            break;

        default:
            hal.stream.write(appendbuf(3, "error:", uitoa((uint32_t)status_code), ASCII_EOL));
            break;
    }

    return status_code;
}


// Prints alarm messages.
alarm_code_t report_alarm_message (alarm_code_t alarm_code)
{
    hal.stream.write_all(appendbuf(3, "ALARM:", uitoa((uint32_t)alarm_code), ASCII_EOL));
    hal.delay_ms(500, NULL); // Force delay to ensure message clears output stream buffer.

    return alarm_code;
}

// Prints feedback message, typically from gcode.
void report_message (const char *msg, message_type_t type)
{
    hal.stream.write("[MSG:");

    switch(type) {

        case Message_Info:
            hal.stream.write("Info: ");
            break;

        case Message_Warning:
            hal.stream.write("Warning: ");
            break;

        default:
            break;
    }

    hal.stream.write(msg);
    hal.stream.write("]" ASCII_EOL);
}

// Prints feedback messages. This serves as a centralized method to provide additional
// user feedback for things that are not of the status/alarm message protocol. These are
// messages such as setup warnings, switch toggling, and how to exit alarms.
// NOTE: For interfaces, messages are always placed within brackets. And if silent mode
// is installed, the message number codes are less than zero.
message_code_t report_feedback_message (message_code_t message_code)
{
    hal.stream.write_all("[MSG:");

    switch(message_code) {

        case Message_None:
            break;

        case Message_CriticalEvent:
            hal.stream.write_all("Reset to continue");
            break;

        case Message_AlarmLock:
            hal.stream.write_all("'$H'|'$X' to unlock");
            break;

        case Message_AlarmUnlock:
            hal.stream.write_all("Caution: Unlocked");
            break;

        case Message_Enabled:
            hal.stream.write_all("Enabled");
            break;

        case Message_Disabled:
            hal.stream.write_all("Disabled");
            break;

        case Message_SafetyDoorAjar:
            hal.stream.write_all("Check Door");
            break;

        case Message_CheckLimits:
            hal.stream.write_all("Check Limits");
            break;

        case Message_ProgramEnd:
            hal.stream.write_all("Pgm End");
            break;

        case Message_RestoreDefaults:
            hal.stream.write_all("Restoring defaults");
            break;

        case Message_SpindleRestore:
            hal.stream.write_all("Restoring spindle");
            break;

        case Message_SleepMode:
            hal.stream.write_all("Sleeping");
            break;

        case Message_EStop:
            hal.stream.write_all("Emergency stop");
            break;

        case Message_HomingCycleRequired:
            hal.stream.write_all("Homing cycle required");
            break;

        case Message_CycleStartToRerun:
            hal.stream.write_all("Press cycle start to rerun job");
            break;

        case Message_ReferenceTLOEstablished:
            hal.stream.write_all("Reference tool length offset established");
            break;

        case Message_MotorFault:
            hal.stream.write_all("Motor fault");
            break;

        default:
            if(grbl.on_unknown_feedback_message)
                grbl.on_unknown_feedback_message(hal.stream.write_all);
            break;
    }

    hal.stream.write_all("]" ASCII_EOL);

    return message_code;
}


// Welcome message
void report_init_message (void)
{
    override_counter = wco_counter = 0;
#if COMPATIBILITY_LEVEL == 0
    hal.stream.write_all(ASCII_EOL "GrblHAL " GRBL_VERSION " ['$' or '$HELP' for help]" ASCII_EOL);
#else
    hal.stream.write_all(ASCII_EOL "Grbl " GRBL_VERSION " ['$' for help]" ASCII_EOL);
#endif
}

// Grbl help message
void report_grbl_help (void)
{
    hal.stream.write("[HLP:$$ $# $G $I $N $x=val $Nx=line $J=line $SLP $C $X $H $B ~ ! ? ctrl-x]" ASCII_EOL);
}

#define CAPS(c) ((c >= 'a' && c <= 'z') ? c & 0x5F : c)

static void report_group_settings (const setting_group_detail_t *groups, const uint_fast8_t n_groups, char *lcargs)
{
    uint_fast8_t idx;
    uint_fast8_t len = strlen(lcargs);

    for(idx = 0; idx < n_groups; idx++) {
        if(strlen(groups[idx].name) == len) {
            char *s1 = lcargs, *s2 = (char *)groups[idx].name;
            while(*s1 && CAPS(*s1) == CAPS(*s2)) {
                s1++;
                s2++;
            }
            if(*s1 == '\0') {
                report_settings_details(true, Setting_SettingsAll, groups[idx].id);
                break;
            }
        }
    }
}

status_code_t report_help (char *args, char *lcargs)
{
    setting_details_t *settings_info = settings_get_details();

    // Strip leading spaces
    while(*args == ' ')
        args++;

    if(*args == '\0') {

        hal.stream.write("Help arguments:" ASCII_EOL);
        hal.stream.write(" Commands" ASCII_EOL);
        hal.stream.write(" Settings" ASCII_EOL);
        report_setting_group_details(false, " ");
    } else {
        if(!strncmp(args, "COMMANDS", 8)) {
            hal.stream.write("$I - list system information" ASCII_EOL);
            hal.stream.write("$$ - list settings" ASCII_EOL);
            hal.stream.write("$# - list offsets, tool table, probing and home position" ASCII_EOL);
            hal.stream.write("$G - list parser state" ASCII_EOL);
            hal.stream.write("$N - list startup lines" ASCII_EOL);
            if(settings.homing.flags.enabled)
                hal.stream.write("$H - home configured axes" ASCII_EOL);
            if(settings.homing.flags.single_axis_commands)
                hal.stream.write("$H<axisletter> - home single axis" ASCII_EOL);
            hal.stream.write("$X - unlock machine" ASCII_EOL);
            hal.stream.write("$SLP - enter sleep mode" ASCII_EOL);
            hal.stream.write("$HELP <arg> - help" ASCII_EOL);
            hal.stream.write("$RST=* - restore/reset all" ASCII_EOL);
            hal.stream.write("$RST=$ - restore default settings" ASCII_EOL);
            if(settings_info->on_get_settings)
                hal.stream.write("$RST=& - restore driver and plugin default settings" ASCII_EOL);
#ifdef N_TOOLS
            hal.stream.write("$RST=# - reset offsets and tool data" ASCII_EOL);
#else
            hal.stream.write("$RST=# - reset offsets" ASCII_EOL);
#endif
            if(grbl.on_report_command_help)
                grbl.on_report_command_help();
        } else if(!strncmp(args, "SETTINGS", 8))
            report_settings_details(true, Setting_SettingsAll, Group_All);
        else {

            // Strip leading spaces from lowercase version
            while(*lcargs == ' ')
                lcargs++;

            report_group_settings(settings_info->groups, settings_info->n_groups, lcargs);

            if(grbl.on_get_settings) {

                on_get_settings_ptr on_get_settings = grbl.on_get_settings;

                while(on_get_settings) {
                    settings_info = on_get_settings();
                    if(settings_info->groups)
                        report_group_settings(settings_info->groups, settings_info->n_groups, lcargs);
                    on_get_settings = settings_info->on_get_settings;
                }
            }
        }
    }

    return Status_OK;
}


// Grbl settings print out.

static int cmp_settings (const void *a, const void *b)
{
  return (*(setting_detail_t **)(a))->id - (*(setting_detail_t **)(b))->id;
}

static bool report_setting (const setting_detail_t *setting, uint_fast16_t offset, void *data)
{
    appendbuf(3, "$", uitoa(setting->id + offset), "=");

    char *value = setting_get_value(setting, offset);

    if(value) {
        hal.stream.write(buf);
        hal.stream.write(value);
        hal.stream.write(ASCII_EOL);
    }

    return true;
}

status_code_t report_grbl_setting (setting_id_t id, void *data)
{
    status_code_t status = Status_OK;

    const setting_detail_t *setting = setting_get_details(id, NULL);

    if(setting)
        grbl.report.setting(setting, id - setting->id, data);
    else
        status = Status_SettingDisabled;

    return status;
}

static bool print_setting (const setting_detail_t *setting, uint_fast16_t offset, void *data)
{
    if(setting->value != NULL)
        grbl.report.setting(setting, offset, data);
    else {
        hal.stream.write("$");
        hal.stream.write(uitoa(setting->id));
        hal.stream.write("=N/A" ASCII_EOL);
    }

    return true;
}

void report_grbl_settings (bool all, void *data)
{
    setting_details_t *details = settings_get_details();
    uint16_t n_settings = details->n_settings;
    const setting_detail_t *setting;

    while(details->on_get_settings) {
        details = details->on_get_settings();
        n_settings += details->n_settings;
    }

    setting_detail_t **all_settings, **psetting;

    if((all_settings = calloc(n_settings, sizeof(setting_detail_t *)))) {

        uint_fast16_t idx;

        details = settings_get_details();
        psetting = all_settings;
        n_settings = 0;

        for(idx = 0; idx < details->n_settings; idx++) {
            setting = &details->settings[idx];
            if((all || setting->type == Setting_IsLegacy || setting->type == Setting_IsLegacyFn) &&
                  (setting->is_available == NULL ||setting->is_available(setting))) {
                *psetting++ = (setting_detail_t *)setting;
                n_settings++;
            }
        }

        if(all) while(details->on_get_settings) {
            details = details->on_get_settings();
            for(idx = 0; idx < details->n_settings; idx++) {
                setting = &details->settings[idx];
                if(setting->is_available == NULL ||setting->is_available(setting)) {
                    *psetting++ = (setting_detail_t *)setting;
                    n_settings++;
                }
            }
        }

        qsort(all_settings, n_settings, sizeof(setting_detail_t *), cmp_settings);

        for(idx = 0; idx < n_settings; idx++)
            settings_iterator(all_settings[idx], print_setting, data);

        free(all_settings);
    }
}


// Prints current probe parameters. Upon a probe command, these parameters are updated upon a
// successful probe or upon a failed probe with the G38.3 without errors command (if supported).
// These values are retained until Grbl is power-cycled, whereby they will be re-zeroed.
void report_probe_parameters (void)
{
    // Report in terms of machine position.
    float print_position[N_AXIS];
    system_convert_array_steps_to_mpos(print_position, sys.probe_position);
    hal.stream.write("[PRB:");
    hal.stream.write(get_axis_values(print_position));
    hal.stream.write(sys.flags.probe_succeeded ? ":1" : ":0");
    hal.stream.write("]" ASCII_EOL);
}

// Prints current home position in terms of machine position.
// Bitmask for homed axes attached.
void report_home_position (void)
{
    hal.stream.write("[HOME:");
    hal.stream.write(get_axis_values(sys.home_position));
    hal.stream.write(":");
    hal.stream.write(uitoa(sys.homed.mask));
    hal.stream.write("]" ASCII_EOL);
}

// Prints current tool offsets.
void report_tool_offsets (void)
{
    hal.stream.write("[TLO:");
#ifdef TOOL_LENGTH_OFFSET_AXIS
    hal.stream.write(get_axis_value(gc_state.tool_length_offset[Z_AXIS]));
#else
    hal.stream.write(get_axis_values(gc_state.tool_length_offset));
#endif
    hal.stream.write("]" ASCII_EOL);
}

// Prints Grbl NGC parameters (coordinate offsets, probing, tool table)
void report_ngc_parameters (void)
{
    uint_fast8_t idx;
    float coord_data[N_AXIS];

    if(gc_state.modal.scaling_active) {
        hal.stream.write("[G51:");
        hal.stream.write(get_axis_values(gc_get_scaling()));
        hal.stream.write("]" ASCII_EOL);
    }

    for (idx = 0; idx < N_CoordinateSystems; idx++) {

        if (!(settings_read_coord_data((coord_system_id_t)idx, &coord_data))) {
            grbl.report.status_message(Status_SettingReadFail);
            return;
        }

        hal.stream.write("[G");

        switch (idx) {

            case CoordinateSystem_G28:
                hal.stream.write("28");
                break;

            case CoordinateSystem_G30:
                hal.stream.write("30");
                break;

            case CoordinateSystem_G92:
                break;

            default: // G54-G59
                hal.stream.write(map_coord_system((coord_system_id_t)idx));
                break;
        }

        if(idx != CoordinateSystem_G92) {
            hal.stream.write(":");
            hal.stream.write(get_axis_values(coord_data));
            hal.stream.write("]" ASCII_EOL);
        }
    }

    // Print G92, G92.1 which are not persistent in memory
    hal.stream.write("92:");
    hal.stream.write(get_axis_values(gc_state.g92_coord_offset));
    hal.stream.write("]" ASCII_EOL);

#ifdef N_TOOLS
    for (idx = 1; idx <= N_TOOLS; idx++) {
        hal.stream.write("[T:");
        hal.stream.write(uitoa((uint32_t)idx));
        hal.stream.write("|");
        hal.stream.write(get_axis_values(tool_table[idx].offset));
        hal.stream.write("|");
        hal.stream.write(get_axis_value(tool_table[idx].radius));
        hal.stream.write("]" ASCII_EOL);
    }
#endif

#if COMPATIBILITY_LEVEL < 10
    if(settings.homing.flags.enabled)
        report_home_position();
#endif

    report_tool_offsets();      // Print tool length offset value.
    report_probe_parameters();  // Print probe parameters. Not persistent in memory.
    if(sys.tlo_reference_set.mask) { // Print tool length reference offset. Not persistent in memory.
        plane_t plane;
        gc_get_plane_data(&plane, gc_state.modal.plane_select);
        hal.stream.write("[TLR:");
        hal.stream.write(get_axis_value(sys.tlo_reference[plane.axis_linear] / settings.axis[plane.axis_linear].steps_per_mm));
        hal.stream.write("]" ASCII_EOL);
    }
}

static inline bool is_g92_active (void)
{
    bool active = false;
    uint_fast32_t idx = N_AXIS;

    do {
        idx--;
        active = !(gc_state.g92_coord_offset[idx] == 0.0f || gc_state.g92_coord_offset[idx] == -0.0f);
    } while(idx && !active);

    return active;
}

// Print current gcode parser mode state
void report_gcode_modes (void)
{
    hal.stream.write("[GC:G");
    if (gc_state.modal.motion >= MotionMode_ProbeToward) {
        hal.stream.write("38.");
        hal.stream.write(uitoa((uint32_t)(gc_state.modal.motion - (MotionMode_ProbeToward - 2))));
    } else
        hal.stream.write(uitoa((uint32_t)gc_state.modal.motion));

    hal.stream.write(" G");
    hal.stream.write(map_coord_system(gc_state.modal.coord_system.id));

#if COMPATIBILITY_LEVEL < 10

    if(is_g92_active())
        hal.stream.write(" G92");

#endif

    if(settings.mode == Mode_Lathe)
        hal.stream.write(gc_state.modal.diameter_mode ? " G7" : " G8");

    hal.stream.write(" G");
    hal.stream.write(uitoa((uint32_t)(gc_state.modal.plane_select + 17)));

    hal.stream.write(gc_state.modal.units_imperial ? " G20" : " G21");

    hal.stream.write(gc_state.modal.distance_incremental ? " G91" : " G90");

    hal.stream.write(" G");
    hal.stream.write(uitoa((uint32_t)(94 - gc_state.modal.feed_mode)));

    if(settings.mode == Mode_Lathe && hal.driver_cap.variable_spindle)
        hal.stream.write(gc_state.modal.spindle_rpm_mode == SpindleSpeedMode_RPM ? " G97" : " G96");

#if COMPATIBILITY_LEVEL < 10

    if(gc_state.modal.tool_offset_mode == ToolLengthOffset_Cancel)
        hal.stream.write(" G49");
    else {
        hal.stream.write(" G43");
        if(gc_state.modal.tool_offset_mode != ToolLengthOffset_Enable)
            hal.stream.write(gc_state.modal.tool_offset_mode == ToolLengthOffset_EnableDynamic ? ".1" : ".2");
    }

    hal.stream.write(gc_state.canned.retract_mode == CCRetractMode_RPos ? " G99" : " G98");

    if(gc_state.modal.scaling_active) {
        hal.stream.write(" G51:");
        axis_signals_tostring(buf, gc_get_g51_state());
        hal.stream.write(buf);
    } else
        hal.stream.write(" G50");

#endif

    if (gc_state.modal.program_flow) {

        switch (gc_state.modal.program_flow) {

            case ProgramFlow_Paused:
                hal.stream.write(" M0");
                break;

            case ProgramFlow_OptionalStop:
                hal.stream.write(" M1");
                break;

            case ProgramFlow_CompletedM2:
                hal.stream.write(" M2");
                break;

            case ProgramFlow_CompletedM30:
                hal.stream.write(" M30");
                break;

            case ProgramFlow_CompletedM60:
                hal.stream.write(" M60");
                break;

            default:
                break;
        }
    }

    hal.stream.write(gc_state.modal.spindle.on ? (gc_state.modal.spindle.ccw ? " M4" : " M3") : " M5");

    if(gc_state.tool_change)
        hal.stream.write(" M6");

    if (gc_state.modal.coolant.value) {

        if (gc_state.modal.coolant.mist)
             hal.stream.write(" M7");

        if (gc_state.modal.coolant.flood)
            hal.stream.write(" M8");

    } else
        hal.stream.write(" M9");

    if (sys.override.control.feed_rate_disable)
        hal.stream.write(" M50");

    if (sys.override.control.spindle_rpm_disable)
        hal.stream.write(" M51");

    if (sys.override.control.feed_hold_disable)
        hal.stream.write(" M53");

    if (settings.parking.flags.enable_override_control && sys.override.control.parking_disable)
        hal.stream.write(" M56");

    hal.stream.write(appendbuf(2, " T", uitoa((uint32_t)gc_state.tool->tool)));

    hal.stream.write(appendbuf(2, " F", get_rate_value(gc_state.feed_rate)));

    if(hal.driver_cap.variable_spindle)
        hal.stream.write(appendbuf(2, " S", ftoa(gc_state.spindle.rpm, N_DECIMAL_RPMVALUE)));

    hal.stream.write("]" ASCII_EOL);
}

// Prints specified startup line
void report_startup_line (uint8_t n, char *line)
{
    hal.stream.write(appendbuf(3, "$N", uitoa((uint32_t)n), "="));
    hal.stream.write(line);
    hal.stream.write(ASCII_EOL);
}

void report_execute_startup_message (char *line, status_code_t status_code)
{
    hal.stream.write(">");
    hal.stream.write(line);
    hal.stream.write(":");
    grbl.report.status_message(status_code);
}

// Prints build info line
void report_build_info (char *line, bool extended)
{
    hal.stream.write("[VER:" GRBL_VERSION "." GRBL_VERSION_BUILD ":");
    hal.stream.write(line);
    hal.stream.write("]" ASCII_EOL);

#if COMPATIBILITY_LEVEL == 0
    extended = true;
#endif

    // Generate compile-time build option list

    char *append = &buf[5];

    strcpy(buf, "[OPT:");

    if(hal.driver_cap.variable_spindle)
        *append++ = 'V';

    *append++ = 'N';

    if(hal.driver_cap.mist_control)
        *append++ = 'M';

#ifdef COREXY
    *append++ = 'C';
#endif

    if(settings.parking.flags.enabled)
        *append++ = 'P';

    if(settings.homing.flags.force_set_origin)
        *append++ = 'Z';

    if(settings.homing.flags.single_axis_commands)
        *append++ = 'H';

    if(settings.limits.flags.two_switches)
        *append++ = 'T';

    if(settings.probe.allow_feed_override)
        *append++ = 'A';

    if(settings.spindle.flags.pwm_action == SpindleAction_DisableWithZeroSPeed)
        *append++ = '0';

    if(hal.driver_cap.software_debounce)
        *append++ = 'S';

    if(settings.parking.flags.enable_override_control)
        *append++ = 'R';

    if(!settings.homing.flags.init_lock)
        *append++ = 'L';

    if(hal.signals_cap.safety_door_ajar)
        *append++ = '+';

  #ifdef DISABLE_RESTORE_NVS_WIPE_ALL // NOTE: Shown when disabled.
    *append++ = '*';
  #endif

  #ifdef DISABLE_RESTORE_NVS_DEFAULT_SETTINGS // NOTE: Shown when disabled.
    *append++ = '$';
  #endif

  #ifdef DISABLE_RESTORE_NVS_CLEAR_PARAMETERS // NOTE: Shown when disabled.
    *append++ = '#';
  #endif

  #ifdef DISABLE_BUILD_INFO_WRITE_COMMAND // NOTE: Shown when disabled.
    *append++ = 'I';
  #endif

    if(!settings.status_report.sync_on_wco_change) // NOTE: Shown when disabled.
        *append++ = 'W';

    if(hal.stepper.get_auto_squared)
        *append++ = '2';

    *append++ = ',';
    *append = '\0';
    hal.stream.write(buf);

    // NOTE: Compiled values, like override increments/max/min values, may be added at some point later.
    hal.stream.write(uitoa((uint32_t)(BLOCK_BUFFER_SIZE - 1)));
    hal.stream.write(",");
    hal.stream.write(uitoa(hal.rx_buffer_size));
    if(extended) {
        hal.stream.write(",");
        hal.stream.write(uitoa((uint32_t)N_AXIS));
        hal.stream.write(",");
  #ifdef N_TOOLS
        hal.stream.write(uitoa((uint32_t)N_TOOLS));
  #else
        hal.stream.write("0");
  #endif
    }
    hal.stream.write("]" ASCII_EOL);

    if(extended) {

        nvs_io_t *nvs = nvs_buffer_get_physical();

        strcpy(buf, "[NEWOPT:ENUMS,RT");
        strcat(buf, settings.flags.legacy_rt_commands ? "+," : "-,");

        if(settings.homing.flags.enabled)
            strcat(buf, "HOME,");

        if(!hal.probe.get_state)
            strcat(buf, "NOPROBE,");
        else if(hal.signals_cap.probe_disconnected)
            strcat(buf, "PC,");

        if(hal.signals_cap.stop_disable)
            strcat(buf, "OS,");

        if(hal.signals_cap.block_delete)
            strcat(buf, "BD,");

        if(hal.signals_cap.e_stop)
            strcat(buf, "ES,");

        if(hal.driver_cap.mpg_mode)
            strcat(buf, "MPG,");

        if(settings.mode == Mode_Lathe)
            strcat(buf, "LATHE,");

    #ifdef N_TOOLS
        if(hal.driver_cap.atc && hal.tool.change)
            strcat(buf, "ATC,");
        else
    #endif
        if(hal.stream.suspend_read)
            strcat(buf, "TC,"); // Manual tool change supported (M6)

        if(hal.driver_cap.spindle_sync)
            strcat(buf, "SS,");

    #ifdef PID_LOG
        strcat(buf, "PID,");
    #endif

        append = &buf[strlen(buf) - 1];
        if(*append == ',')
            *append = '\0';

        hal.stream.write(buf);
        grbl.on_report_options(true);
        hal.stream.write("]" ASCII_EOL);

        hal.stream.write("[FIRMWARE:grblHAL]" ASCII_EOL);

        if(!(nvs->type == NVS_None || nvs->type == NVS_Emulated)) {
            hal.stream.write("[NVS STORAGE:");
            *buf = '\0';
            if(hal.nvs.type == NVS_Emulated)
                strcat(buf, "*");
            strcat(buf, nvs->type == NVS_Flash ? "FLASH" : (nvs->type == NVS_FRAM ? "FRAM" : "EEPROM"));
            hal.stream.write(buf);
            hal.stream.write("]" ASCII_EOL);
        }

        if(hal.info) {
            hal.stream.write("[DRIVER:");
            hal.stream.write(hal.info);
            hal.stream.write("]" ASCII_EOL);
        }

        if(hal.driver_version) {
            hal.stream.write("[DRIVER VERSION:");
            hal.stream.write(hal.driver_version);
            hal.stream.write("]" ASCII_EOL);
        }

        if(hal.driver_options) {
            hal.stream.write("[DRIVER OPTIONS:");
            hal.stream.write(hal.driver_options);
            hal.stream.write("]" ASCII_EOL);
        }

        if(hal.board) {
            hal.stream.write("[BOARD:");
            hal.stream.write(hal.board);
            hal.stream.write("]" ASCII_EOL);
        }

        if(hal.max_step_rate) {
            hal.stream.write("[MAX STEP RATE:");
            hal.stream.write(uitoa(hal.max_step_rate));
            hal.stream.write(" Hz]" ASCII_EOL);
        }

#if COMPATIBILITY_LEVEL > 0
        hal.stream.write("[COMPATIBILITY LEVEL:");
        hal.stream.write(uitoa(COMPATIBILITY_LEVEL));
        hal.stream.write("]" ASCII_EOL);
#endif

        grbl.on_report_options(false);
    }
}


// Prints the character string line Grbl has received from the user, which has been pre-parsed,
// and has been sent into protocol_execute_line() routine to be executed by Grbl.
void report_echo_line_received (char *line)
{
    hal.stream.write("[echo: ");
    hal.stream.write(line);
    hal.stream.write("]" ASCII_EOL);
}


 // Prints real-time data. This function grabs a real-time snapshot of the stepper subprogram
 // and the actual location of the CNC machine. Users may change the following function to their
 // specific needs, but the desired real-time data report must be as short as possible. This is
 // requires as it minimizes the computational overhead and allows grbl to keep running smoothly,
 // especially during g-code programs with fast, short line segments and high frequency reports (5-20Hz).
void report_realtime_status (void)
{
    static bool probing = false;

    int32_t current_position[N_AXIS]; // Copy current state of the system position variable
    float print_position[N_AXIS];
    probe_state_t probe_state = {
        .connected = On,
        .triggered = Off
    };

    memcpy(current_position, sys.position, sizeof(sys.position));
    system_convert_array_steps_to_mpos(print_position, current_position);

    if(hal.probe.get_state)
        probe_state = hal.probe.get_state();

    // Report current machine state and sub-states
    hal.stream.write_all("<");

    sys_state_t state = state_get();

    switch (gc_state.tool_change && state == STATE_CYCLE ? STATE_TOOL_CHANGE : state) {

        case STATE_IDLE:
            hal.stream.write_all("Idle");
            break;

        case STATE_CYCLE:
            hal.stream.write_all("Run");
            if(sys.probing_state == Probing_Active && settings.status_report.run_substate)
                probing = true;
            else if (probing)
                probing = probe_state.triggered;
            if(sys.flags.feed_hold_pending)
                hal.stream.write_all(":1");
            else if(probing)
                hal.stream.write_all(":2");
            break;

        case STATE_HOLD:
            hal.stream.write_all(appendbuf(2, "Hold:", uitoa((uint32_t)(sys.holding_state - 1))));
            break;

        case STATE_JOG:
            hal.stream.write_all("Jog");
            break;

        case STATE_HOMING:
            hal.stream.write_all("Home");
            break;

        case STATE_ESTOP:
        case STATE_ALARM:
            if((sys.report.all || settings.status_report.alarm_substate) && sys.alarm)
                hal.stream.write_all(appendbuf(2, "Alarm:", uitoa((uint32_t)sys.alarm)));
            else
                hal.stream.write_all("Alarm");
            break;

        case STATE_CHECK_MODE:
            hal.stream.write_all("Check");
            break;

        case STATE_SAFETY_DOOR:
            hal.stream.write_all(appendbuf(2, "Door:", uitoa((uint32_t)sys.parking_state)));
            break;

        case STATE_SLEEP:
            hal.stream.write_all("Sleep");
            break;

        case STATE_TOOL_CHANGE:
            hal.stream.write_all("Tool");
            break;
    }

    uint_fast8_t idx;
    float wco[N_AXIS];
    if (!settings.status_report.machine_position || sys.report.wco) {
        for (idx = 0; idx < N_AXIS; idx++) {
            // Apply work coordinate offsets and tool length offset to current position.
            wco[idx] = gc_get_offset(idx);
            if (!settings.status_report.machine_position)
                print_position[idx] -= wco[idx];
        }
    }

    // Report position
    hal.stream.write_all(settings.status_report.machine_position ? "|MPos:" : "|WPos:");
    hal.stream.write_all(get_axis_values(print_position));

    // Returns planner and output stream buffer states.

    if (settings.status_report.buffer_state) {
        hal.stream.write_all("|Bf:");
        hal.stream.write_all(uitoa((uint32_t)plan_get_block_buffer_available()));
        hal.stream.write_all(",");
        hal.stream.write_all(uitoa(hal.stream.get_rx_buffer_available()));
    }

    if(settings.status_report.line_numbers) {
        // Report current line number
        plan_block_t *cur_block = plan_get_current_block();
        if (cur_block != NULL && cur_block->line_number > 0)
            hal.stream.write_all(appendbuf(2, "|Ln:", uitoa((uint32_t)cur_block->line_number)));
    }

    spindle_state_t sp_state = hal.spindle.get_state();

    // Report realtime feed speed
    if(settings.status_report.feed_speed) {
        if(hal.driver_cap.variable_spindle) {
            hal.stream.write_all(appendbuf(2, "|FS:", get_rate_value(st_get_realtime_rate())));
            hal.stream.write_all(appendbuf(2, ",", uitoa(sp_state.on ? lroundf(sys.spindle_rpm) : 0)));
            if(hal.spindle.get_data /* && sys.mpg_mode */)
                hal.stream.write_all(appendbuf(2, ",", uitoa(lroundf(hal.spindle.get_data(SpindleData_RPM)->rpm))));
        } else
            hal.stream.write_all(appendbuf(2, "|F:", get_rate_value(st_get_realtime_rate())));
    }

    if(settings.status_report.pin_state) {

        axes_signals_t lim_pin_state = hal.limits.get_state();
        control_signals_t ctrl_pin_state = hal.control.get_state();

        if (lim_pin_state.value | ctrl_pin_state.value | probe_state.triggered | !probe_state.connected | sys.flags.block_delete_enabled) {

            char *append = &buf[4];

            strcpy(buf, "|Pn:");

            if (probe_state.triggered)
                *append++ = 'P';

            if(!probe_state.connected)
                *append++ = 'O';

            if (lim_pin_state.value && !hal.control.get_state().limits_override)
                append = axis_signals_tostring(append, lim_pin_state);

            if (ctrl_pin_state.value) {
                if (ctrl_pin_state.safety_door_ajar && hal.signals_cap.safety_door_ajar)
                    *append++ = 'D';
                if (ctrl_pin_state.reset)
                    *append++ = 'R';
                if (ctrl_pin_state.feed_hold)
                    *append++ = 'H';
                if (ctrl_pin_state.cycle_start)
                    *append++ = 'S';
                if (ctrl_pin_state.e_stop)
                    *append++ = 'E';
                if (ctrl_pin_state.block_delete && sys.flags.block_delete_enabled)
                    *append++ = 'L';
                if (hal.signals_cap.stop_disable ? ctrl_pin_state.stop_disable : sys.flags.optional_stop_disable)
                    *append++ = 'T';
                if (ctrl_pin_state.motor_warning)
                    *append++ = 'W';
                if (ctrl_pin_state.motor_fault)
                    *append++ = 'M';
            }
            *append = '\0';
            hal.stream.write_all(buf);
        }
    }

    if(settings.status_report.work_coord_offset) {

        if (wco_counter > 0 && !sys.report.wco)
            wco_counter--;
        else
            wco_counter = state_get() & (STATE_HOMING|STATE_CYCLE|STATE_HOLD|STATE_JOG|STATE_SAFETY_DOOR)
                            ? (REPORT_WCO_REFRESH_BUSY_COUNT - 1) // Reset counter for slow refresh
                            : (REPORT_WCO_REFRESH_IDLE_COUNT - 1);
    } else
        sys.report.wco = Off;

    if(settings.status_report.overrides) {

        if (override_counter > 0 && !sys.report.overrides)
            override_counter--;
        else {
            sys.report.overrides = On;
            sys.report.spindle = sys.report.spindle || hal.spindle.get_state().on;
            sys.report.coolant = sys.report.coolant || hal.coolant.get_state().value != 0;
            override_counter = state_get() & (STATE_HOMING|STATE_CYCLE|STATE_HOLD|STATE_JOG|STATE_SAFETY_DOOR)
                                 ? (REPORT_OVERRIDE_REFRESH_BUSY_COUNT - 1) // Reset counter for slow refresh
                                 : (REPORT_OVERRIDE_REFRESH_IDLE_COUNT - 1);
        }
    } else
        sys.report.overrides = Off;

    if(sys.report.value || gc_state.tool_change) {

        if(sys.report.wco) {
            hal.stream.write_all("|WCO:");
            hal.stream.write_all(get_axis_values(wco));
        }

        if(sys.report.gwco) {
            hal.stream.write_all("|WCS:G");
            hal.stream.write_all(map_coord_system(gc_state.modal.coord_system.id));
        }

        if(sys.report.overrides) {
            hal.stream.write_all(appendbuf(2, "|Ov:", uitoa((uint32_t)sys.override.feed_rate)));
            hal.stream.write_all(appendbuf(2, ",", uitoa((uint32_t)sys.override.rapid_rate)));
            hal.stream.write_all(appendbuf(2, ",", uitoa((uint32_t)sys.override.spindle_rpm)));
        }

        if(sys.report.spindle || sys.report.coolant || sys.report.tool || gc_state.tool_change) {

            coolant_state_t cl_state = hal.coolant.get_state();

            char *append = &buf[3];

            strcpy(buf, "|A:");

            if (sp_state.on)

                *append++ = sp_state.ccw ? 'C' : 'S';

#if COMPATIBILITY_LEVEL == 0
            if(sp_state.encoder_error && hal.driver_cap.spindle_sync)
                *append++ = 'E';
#endif

            if (cl_state.flood)
                *append++ = 'F';

            if (cl_state.mist)
                *append++ = 'M';

            if(gc_state.tool_change && !sys.report.tool)
                *append++ = 'T';

            *append = '\0';
            hal.stream.write_all(buf);
        }

        if(sys.report.scaling) {
            axis_signals_tostring(buf, gc_get_g51_state());
            hal.stream.write_all("|Sc:");
            hal.stream.write_all(buf);
        }

        if(sys.report.mpg_mode && hal.driver_cap.mpg_mode)
            hal.stream.write_all(sys.mpg_mode ? "|MPG:1" : "|MPG:0");

        if(sys.report.homed && (sys.homing.mask || settings.homing.flags.single_axis_commands || settings.homing.flags.manual)) {
            axes_signals_t homing = {sys.homing.mask ? sys.homing.mask : AXES_BITMASK};
            hal.stream.write_all(appendbuf(2, "|H:", (homing.mask & sys.homed.mask) == homing.mask ? "1" : "0"));
            if(settings.homing.flags.single_axis_commands)
                hal.stream.write_all(appendbuf(2, ",", uitoa(sys.homed.mask)));
        }

        if(sys.report.xmode && settings.mode == Mode_Lathe)
            hal.stream.write_all(gc_state.modal.diameter_mode ? "|D:1" : "|D:0");

        if(sys.report.tool)
            hal.stream.write_all(appendbuf(2, "|T:", uitoa(gc_state.tool->tool)));

        if(sys.report.tlo_reference)
            hal.stream.write_all(appendbuf(2, "|TLR:", uitoa(sys.tlo_reference_set.mask != 0)));
    }

    if(grbl.on_realtime_report)
        grbl.on_realtime_report(hal.stream.write_all, sys.report);

#if COMPATIBILITY_LEVEL <= 1
    if(sys.report.all)
        hal.stream.write_all("|FW:grblHAL");
    else
#endif
    if(settings.status_report.parser_state) {

        static uint32_t tool;
        static float feed_rate, spindle_rpm;
        static gc_modal_t last_state;
        static bool g92_active;

        bool is_changed = feed_rate != gc_state.feed_rate || spindle_rpm != gc_state.spindle.rpm || tool != gc_state.tool->tool;

        if(is_changed) {
            feed_rate = gc_state.feed_rate;
            tool = gc_state.tool->tool;
            spindle_rpm = gc_state.spindle.rpm;
        } else if ((is_changed = g92_active != is_g92_active()))
            g92_active = !g92_active;
        else if(memcmp(&last_state, &gc_state.modal, sizeof(gc_modal_t))) {
            last_state = gc_state.modal;
            is_changed = true;
        }

        if (is_changed)
            system_set_exec_state_flag(EXEC_GCODE_REPORT);

        if(sys.report.tool_offset)
            system_set_exec_state_flag(EXEC_TLO_REPORT);
    }

    hal.stream.write_all(">" ASCII_EOL);

    sys.report.value = 0;
    sys.report.wco = settings.status_report.work_coord_offset && wco_counter == 0; // Set to report on next request
}

static void report_bitfield (const char *format, bool bitmap)
{
    char *s;
    uint_fast8_t bit = 0;
    uint_fast16_t val = 1;

    // Copy string from Flash to RAM, strtok cannot be used unless doing so.
    if((s = (char *)malloc(strlen(format) + 1))) {

        strcpy(s, format);
        char *element = strtok(s, ",");

        while(element) {
            hal.stream.write(ASCII_EOL);
            hal.stream.write("    ");
            hal.stream.write(uitoa(bit++));
            hal.stream.write(" - ");
            hal.stream.write(element);
            if(bitmap) {
                hal.stream.write(" (");
                hal.stream.write(uitoa(val));
                hal.stream.write(")");
                val <<= 1;
            }
            element = strtok(NULL, ",");
        }

        free(s);
    }
}

static void report_settings_detail (bool human_readable, const setting_detail_t *setting, uint_fast8_t offset)
{
    if(human_readable)
        hal.stream.write("$");
    else
        hal.stream.write("[SETTING:");

    hal.stream.write(uitoa(setting->id + offset));

    if(human_readable) {
        hal.stream.write(": ");
        if(setting->group == Group_Axis0)
            hal.stream.write(axis_letter[offset]);
        hal.stream.write(setting->name[0] == '?' ? &setting->name[1] : setting->name); // temporary hack for ? prefix...

        switch(setting_datatype_to_external(setting->datatype)) {

            case Format_AxisMask:
                hal.stream.write(" as axismask");
                break;

            case Format_Bool:
                hal.stream.write(" as boolean");
                break;

            case Format_Bitfield:
                hal.stream.write(" as bitfield:");
                report_bitfield(setting->format, true);
                break;

            case Format_XBitfield:
                hal.stream.write(" as bitfield where setting bit 0 enables the rest:");
                report_bitfield(setting->format, true);
                break;

            case Format_RadioButtons:
                hal.stream.write(":");
                report_bitfield(setting->format, false);
                break;

            case Format_IPv4:
                hal.stream.write(" as IP address");
                break;

            default:
                if(setting->unit) {
                    hal.stream.write(" in ");
                    hal.stream.write(setting->unit);
                }
                break;
        }

        if(setting->min_value && setting->max_value) {
            hal.stream.write(", range: ");
            hal.stream.write(setting->min_value);
            hal.stream.write(" - ");
            hal.stream.write(setting->max_value);
        } else if(!setting_is_list(setting)) {
            if(setting->min_value) {
                hal.stream.write(", min: ");
                hal.stream.write(setting->min_value);
            }
            if(setting->max_value) {
                hal.stream.write(", max: ");
                hal.stream.write(setting->max_value);
            }
        }
    } else {
        hal.stream.write(vbar);
        hal.stream.write(uitoa(setting->group + (setting->group == Group_Axis0 ? offset : 0)));
        hal.stream.write(vbar);
        if(setting->group == Group_Axis0)
            hal.stream.write(axis_letter[offset]);
        hal.stream.write(setting->name[0] == '?' ? &setting->name[1] : setting->name); // temporary hack for ? prefix...
        hal.stream.write(vbar);
        if(setting->unit)
            hal.stream.write(setting->unit);
        hal.stream.write(vbar);
        hal.stream.write(uitoa(setting_datatype_to_external(setting->datatype)));
        hal.stream.write(vbar);
        if(setting->format)
            hal.stream.write(setting->format);
        hal.stream.write(vbar);
        if(setting->min_value && !setting_is_list(setting))
            hal.stream.write(setting->min_value);
        hal.stream.write(vbar);
        if(setting->max_value)
            hal.stream.write(setting->max_value);
    }

    if(!human_readable)
        hal.stream.write("]");

    hal.stream.write(ASCII_EOL);
}

typedef struct {
    bool human_readable;
    setting_group_t group;
    uint_fast16_t offset;
} report_args_t;

static bool print_sorted (const setting_detail_t *setting, uint_fast16_t offset, void *args)
{
    if(!(((report_args_t *)args)->group == setting->group && ((report_args_t *)args)->offset != offset))
        report_settings_detail (((report_args_t *)args)->human_readable, setting, offset);

    return true;
}

static status_code_t sort_settings_details (bool human_readable, setting_group_t group)
{
    bool reported = group == Group_All;

    setting_details_t *details = settings_get_details();
    uint16_t n_settings = details->n_settings;
    const setting_detail_t *setting;
    report_args_t args;

    args.group = settings_normalize_group(group);
    args.offset = group - args.group;
    args.human_readable = human_readable;

    while(details->on_get_settings) {
        details = details->on_get_settings();
        n_settings += details->n_settings;
    }

    setting_detail_t **all_settings, **psetting;

    if((all_settings = calloc(n_settings, sizeof(setting_detail_t *)))) {

        uint_fast16_t idx;

        details = settings_get_details();

        psetting = all_settings;
        n_settings = 0;

        for(idx = 0; idx < details->n_settings; idx++) {
            setting = &details->settings[idx];
            if((group == Group_All || setting->group == args.group) && (setting->is_available == NULL ||setting->is_available(setting))) {
                *psetting++ = (setting_detail_t *)setting;
                n_settings++;
            }
        }

        while(details->on_get_settings) {
            details = details->on_get_settings();
            for(idx = 0; idx < details->n_settings; idx++) {
                setting = &details->settings[idx];
                if((group == Group_All || setting->group == args.group) && (setting->is_available == NULL ||setting->is_available(setting))) {
                    *psetting++ = (setting_detail_t *)setting;
                    n_settings++;
                }
            }
        }

        qsort(all_settings, n_settings, sizeof(setting_detail_t *), cmp_settings);

        for(idx = 0; idx < n_settings; idx++) {
            if(settings_iterator(all_settings[idx], print_sorted, &args))
                reported = true;
        }

        free(all_settings);
    }

    return all_settings == NULL ? Status_Unhandled : (reported ? Status_OK : Status_SettingDisabled);
}

static bool print_unsorted (const setting_detail_t *setting, uint_fast16_t offset, void *args)
{
    if(!(((report_args_t *)args)->group == setting->group && ((report_args_t *)args)->offset != offset) &&
       (setting->is_available == NULL ||setting->is_available(setting)))
        report_settings_detail(((report_args_t *)args)->human_readable, setting, offset);

    return true;
}

static status_code_t print_settings_details (bool human_readable, setting_group_t group, uint_fast16_t axis_rpt)
{
    status_code_t status;

    if((status = sort_settings_details(human_readable, group)) != Status_Unhandled)
        return status;

    bool reported = group == Group_All;
    uint_fast16_t idx;
    const setting_detail_t *setting;
    setting_details_t *settings = settings_get_details();
    report_args_t args;

    args.group = settings_normalize_group(group);
    args.offset = group - args.group;
    args.human_readable = human_readable;

    do {
        for(idx = 0; idx < settings->n_settings; idx++) {

            setting = &settings->settings[idx];

            if(group == Group_All || setting->group == args.group) {
                if(settings_iterator(setting, print_unsorted, &args))
                    reported = true;
            }
        }
        settings = settings->on_get_settings ? settings->on_get_settings() : NULL;
    } while(settings);

    return reported ? Status_OK : Status_SettingDisabled;
}

status_code_t report_settings_details (bool human_readable, setting_id_t id, setting_group_t group)
{
    uint_fast16_t axis_rpt = 0;

    if(id != Setting_SettingsAll) {
        status_code_t status = Status_OK;

        const setting_detail_t *setting = setting_get_details(id, NULL);

        if(setting)
            report_settings_detail(human_readable, setting, id - setting->id);
        else
            status = Status_SettingDisabled;

        return status;
    }

    return print_settings_details(human_readable, group, axis_rpt);
}

status_code_t report_alarm_details (void)
{
    uint_fast16_t idx, n_alarms = sizeof(alarm_detail) / sizeof(alarm_detail_t);

    for(idx = 0; idx < n_alarms; idx++) {

        hal.stream.write("[ALARMCODE:");

        hal.stream.write(uitoa(alarm_detail[idx].id));
        hal.stream.write(vbar);
        hal.stream.write(alarm_detail[idx].name);
        hal.stream.write(vbar);
        if(alarm_detail[idx].description)
            hal.stream.write(alarm_detail[idx].description);
        hal.stream.write("]" ASCII_EOL);
    }

    return Status_OK;
}

status_code_t report_error_details (void)
{
    uint_fast16_t idx, n_alarms = sizeof(status_detail) / sizeof(status_detail_t);

    for(idx = 0; idx < n_alarms; idx++) {

        hal.stream.write("[ERRORCODE:");

        hal.stream.write(uitoa(status_detail[idx].id));
        hal.stream.write(vbar);
        hal.stream.write(status_detail[idx].name);
        hal.stream.write(vbar);
        if(status_detail[idx].description)
            hal.stream.write(status_detail[idx].description);
        hal.stream.write("]" ASCII_EOL);
    }

    return Status_OK;
}

static void print_setting_group (const setting_group_detail_t *group, char *prefix)
{
    if(settings_is_group_available(group->id)) {
        if(!prefix) {
            hal.stream.write("[SETTINGGROUP:");
            hal.stream.write(uitoa(group->id));
            hal.stream.write(vbar);
            hal.stream.write(uitoa(group->parent));
            hal.stream.write(vbar);
            hal.stream.write(group->name);
            hal.stream.write("]" ASCII_EOL);
        } else if(group->id != Group_Root && settings_is_group_available(group->id)) {
            hal.stream.write(prefix);
            hal.stream.write(group->name);
            hal.stream.write(ASCII_EOL);
        }
    }
}

static int cmp_setting_group_id (const void *a, const void *b)
{
    return (*(setting_detail_t **)(a))->id - (*(setting_detail_t **)(b))->id;
}

static int cmp_setting_group_name (const void *a, const void *b)
{
    return strcmp((*(setting_detail_t **)(a))->name, (*(setting_detail_t **)(b))->name);
}

static status_code_t sort_setting_group_details (bool by_id, char *prefix)
{
    setting_details_t *details = settings_get_details();
    uint8_t n_groups = details->n_groups;

    while(details->on_get_settings) {
        details = details->on_get_settings();
        n_groups += details->n_groups;
    }

    setting_group_detail_t **all_groups, **group;

    if((all_groups = calloc(n_groups, sizeof(setting_group_detail_t *)))) {

        uint_fast16_t idx;

        group = all_groups;
        details = settings_get_details();

        do {
            for(idx = 0; idx < details->n_groups; idx++)
                *group++ = (setting_group_detail_t *)&details->groups[idx];
            details = details->on_get_settings ? details->on_get_settings() : NULL;
        } while(details);

        qsort(all_groups, n_groups, sizeof(setting_group_detail_t *), by_id ? cmp_setting_group_id : cmp_setting_group_name);

        for(idx = 0; idx < n_groups; idx++)
            print_setting_group(all_groups[idx], prefix);

        free(all_groups);
    }

    return all_groups == NULL ? Status_Unhandled : Status_OK;
}

status_code_t report_setting_group_details (bool by_id, char *prefix)
{
    if(sort_setting_group_details(by_id, prefix) != Status_Unhandled)
        return Status_OK;

    uint_fast16_t idx;
    setting_details_t *details = settings_get_details();

    do {
        for(idx = 0; idx < details->n_groups; idx++)
            print_setting_group(&details->groups[idx], prefix);
        details = details->on_get_settings ? details->on_get_settings() : NULL;
    } while(details);

    return Status_OK;
}

status_code_t report_spindle_data (sys_state_t state, char *args)
{
    if(hal.spindle.get_data) {

        float apos = hal.spindle.get_data(SpindleData_AngularPosition)->angular_position;
        spindle_data_t *spindle = hal.spindle.get_data(SpindleData_Counters);

        hal.stream.write("[SPINDLE:");
        hal.stream.write(uitoa(spindle->index_count));
        hal.stream.write(",");
        hal.stream.write(uitoa(spindle->pulse_count));
//        hal.stream.write(",");
//        hal.stream.write(ftoa((float)spindle->pulse_count / (float)settings.spindle.ppr, 3));
        hal.stream.write(",");
        hal.stream.write(ftoa(apos, 3));
        hal.stream.write("]" ASCII_EOL);

//        hal.spindle.reset_data();
    }

    return hal.spindle.get_data ? Status_OK : Status_InvalidStatement;
}

void report_pid_log (void)
{
#ifdef PID_LOG
    uint_fast16_t idx = 0;

    hal.stream.write("[PID:");
    hal.stream.write(ftoa(sys.pid_log.setpoint, N_DECIMAL_PIDVALUE));
    hal.stream.write(",");
    hal.stream.write(ftoa(sys.pid_log.t_sample, N_DECIMAL_PIDVALUE));
    hal.stream.write(",2|"); // 2 is number of values per sample!

    if(sys.pid_log.idx) do {
        hal.stream.write(ftoa(sys.pid_log.target[idx], N_DECIMAL_PIDVALUE));
        hal.stream.write(",");
        hal.stream.write(ftoa(sys.pid_log.actual[idx], N_DECIMAL_PIDVALUE));
        idx++;
        if(idx != sys.pid_log.idx)
            hal.stream.write(",");
    } while(idx != sys.pid_log.idx);

    hal.stream.write("]" ASCII_EOL);
    grbl.report.status_message(Status_OK);
#else
    grbl.report.status_message(Status_GcodeUnsupportedCommand);
#endif
}