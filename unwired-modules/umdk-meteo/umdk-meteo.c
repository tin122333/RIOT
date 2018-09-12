/*
 * Copyright (C) 2016 Unwired Devices [info@unwds.com]
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @defgroup    
 * @ingroup     
 * @brief       
 * @{
 * @file		umdk-meteo.c
 * @brief       umdk-meteo module implementation
 * @author      Eugene Ponomarev
 * @author		Oleg Artamonov
 */

#ifdef __cplusplus
extern "C" {
#endif

/* define is autogenerated, do not change */
#undef _UMDK_MID_
#define _UMDK_MID_ UNWDS_METEO_MODULE_ID

/* define is autogenerated, do not change */
#undef _UMDK_NAME_
#define _UMDK_NAME_ "meteo"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "periph/i2c.h"

#include "board.h"

#include "bmx280.h"
#include "sht21.h"
#include "lps331ap.h"
#include "lm75.h"

#include "unwds-common.h"
#include "umdk-meteo.h"

#include "thread.h"
#include "rtctimers-millis.h"

static bmx280_t dev_bmx280;
static sht21_t dev_sht21;
static lps331ap_t dev_lps331;
static lm75_t dev_lm75;

static uwnds_cb_t *callback;

static kernel_pid_t timer_pid;

static msg_t timer_msg = {};
static rtctimers_millis_t timer;

static bool is_polled = false;

typedef enum {
    UMDK_METEO_BME280   = 1,
    UMDK_METEO_SHT21    = 1 << 1,
    UMDK_METEO_LPS331   = 1 << 2,
    UMDK_METEO_LM75     = 1 << 3,
} umdk_meteo_active_sensors_t;

static uint8_t active_sensors = 0;

static struct {
	uint8_t publish_period_min;
} meteo_config;

static bool init_sensor(void) {
    bmx280_params_t bme280_params[] = { METEO_PARAMS_BOARD };
    
	printf("[umdk-" _UMDK_NAME_ "] Initializing METEO on I2C #%d\n", bme280_params[0].i2c_dev);
    
    if (bmx280_init(&dev_bmx280, &bme280_params[0]) == BMX280_OK) {
        active_sensors |= UMDK_METEO_BME280;
        puts("[umdk-" _UMDK_NAME_ "] Bosch BME280 sensor found");      
    }
    
    dev_sht21.i2c = bme280_params[0].i2c_dev;
    if (sht21_init(&dev_sht21) == 0) {
        active_sensors |= UMDK_METEO_SHT21;
        puts("[umdk-" _UMDK_NAME_ "] Sensirion SHT21 sensor found");
    }
    
    dev_lps331.i2c = bme280_params[0].i2c_dev;
    if (lps331ap_init(&dev_lps331, dev_lps331.i2c, 0x5D, LPS331AP_RATE_1HZ) == 0) {
        active_sensors |= UMDK_METEO_LPS331;
        puts("[umdk-" _UMDK_NAME_ "] STMicro LPS331 sensor found");
    }
    
    dev_lm75.params.i2c = bme280_params[0].i2c_dev;
    dev_lm75.params.a1 = 0;
    dev_lm75.params.a2 = 0;
    dev_lm75.params.a3 = 0;
    
    if (lm75_init(&dev_lm75) == 0) {
        active_sensors |= UMDK_METEO_LM75;
        puts("[umdk-" _UMDK_NAME_ "] LM75 sensor found");
    }
    
	return (active_sensors != 0);
}

static void prepare_result(module_data_t *data) {
    int16_t measurements[3] = { SHRT_MAX };
    
    if (active_sensors & UMDK_METEO_BME280) {
        measurements[0] = (5 + bmx280_read_temperature(&dev_bmx280))/10; /* degrees C * 100 -> degrees C * 10 */
        measurements[1] = (5 + bme280_read_humidity(&dev_bmx280))/10; /* percents * 100 -> percents * 10 */
        measurements[2] = bmx280_read_pressure(&dev_bmx280)/100; /* Pa -> mbar */
    } else {
        /* if there's BME280, no need in additional sensors */
        
        if ((!(active_sensors & UMDK_METEO_LPS331)) && !(active_sensors & UMDK_METEO_SHT21)) {
            if (active_sensors & UMDK_METEO_LM75) {
                measurements[0] = lm75_get_ambient_temperature(&dev_lm75)/100; /* degrees C * 1000 -> degrees C * 10 */
            }
        }
        
        if (active_sensors & UMDK_METEO_LPS331) {
            if (! (active_sensors & UMDK_METEO_SHT21)) {
                /* SHT21 has better temperature sensor */
                measurements[0] = (lps331ap_read_temp(&dev_lps331) + 50) / 100;
            }
            measurements[2] = lps331ap_read_pres(&dev_lps331);
        }
        
        if (active_sensors & UMDK_METEO_SHT21) {
            sht21_measure_t measure = { 0 };
            sht21_measure(&dev_sht21, &measure);
            
            measurements[0] = (measure.temperature + 50) / 100;
            measurements[1] = (measure.humidity + 50) / 100;
        }
    }
    
    char buf[2][10];
    int_to_float_str(buf[0], measurements[0], 1);
    int_to_float_str(buf[1], measurements[1], 1);
    
	printf("[umdk-" _UMDK_NAME_ "] Temperature %s C, humidity: %s%%, pressure: %d mbar\n", buf[0], buf[1], measurements[2]);
    
    for (int i = 0; i < 3; i++) {
        convert_to_be_sam((void *)&measurements[i], sizeof(measurements[i]));
    }

    if (data) {
        /* 8 bytes total */
        data->length = 16;

        data->data[0] = _UMDK_MID_;
        data->data[1] = UMDK_METEO_DATA;

        /* Copy measurements into response */
        memcpy(data->data + 2, (uint8_t *)measurements, sizeof(measurements));
    }
}

static void *timer_thread(void *arg) {
    (void)arg;
    
    msg_t msg;
    
    puts("[umdk-" _UMDK_NAME_ "] Periodic publisher thread started");

    while (1) {
        msg_receive(&msg);

        module_data_t data = {};
        data.as_ack = is_polled;
        is_polled = false;

        prepare_result(&data);

        /* Notify the application */
        callback(&data);
        /* Restart after delay */
        rtctimers_millis_set_msg(&timer, 60000 * meteo_config.publish_period_min, &timer_msg, timer_pid);
    }

    return NULL;
}

static void reset_config(void) {
	meteo_config.publish_period_min = UMDK_METEO_PUBLISH_PERIOD_MIN;
}

static void init_config(void) {
	reset_config();

	if (!unwds_read_nvram_config(_UMDK_MID_, (uint8_t *) &meteo_config, sizeof(meteo_config))) {
        reset_config();
    }

}

static inline void save_config(void) {
	unwds_write_nvram_config(_UMDK_MID_, (uint8_t *) &meteo_config, sizeof(meteo_config));
}

static void set_period (int period) {
    rtctimers_millis_remove(&timer);
    meteo_config.publish_period_min = period;
	save_config();

	/* Don't restart timer if new period is zero */
	if (meteo_config.publish_period_min) {
		rtctimers_millis_set_msg(&timer, 60000 * meteo_config.publish_period_min, &timer_msg, timer_pid);
		printf("[umdk-" _UMDK_NAME_ "] Period set to %d minute (s)\n", meteo_config.publish_period_min);
	} else {
		puts("[umdk-" _UMDK_NAME_ "] Timer stopped");
	}
}

int umdk_meteo_shell_cmd(int argc, char **argv) {
    if (argc == 1) {
        puts (_UMDK_NAME_ " get - get results now");
        puts (_UMDK_NAME_ " send - get and send results now");
        puts (_UMDK_NAME_ " period <N> - set period to N minutes");
        puts (_UMDK_NAME_ " - reset settings to default");
        return 0;
    }
    
    char *cmd = argv[1];
	
    if (strcmp(cmd, "get") == 0) {
        prepare_result(NULL);
    }
    
    if (strcmp(cmd, "send") == 0) {
		/* Send signal to publisher thread */
		msg_send(&timer_msg, timer_pid);
    }
    
    if (strcmp(cmd, "period") == 0) {
        char *val = argv[2];
        set_period(atoi(val));
    }
    
    if (strcmp(cmd, "reset") == 0) {
        reset_config();
        save_config();
    }
    
    return 1;
}

static void btn_connect(void* arg) {
    (void)arg;
    
    is_polled = false;
    msg_send(&timer_msg, timer_pid);
}


void umdk_meteo_init(uint32_t *non_gpio_pin_map, uwnds_cb_t *event_callback) {
	(void) non_gpio_pin_map;

	callback = event_callback;

	init_config();
	printf("[umdk-" _UMDK_NAME_ "] Publish period: %d min\n", meteo_config.publish_period_min);

	if (!init_sensor()) {
		puts("[umdk-" _UMDK_NAME_ "] No sensors found");
        return;
	}

	/* Create handler thread */
	char *stack = (char *) allocate_stack(UMDK_METEO_STACK_SIZE);
	if (!stack) {
		return;
	}
    
    unwds_add_shell_command(_UMDK_NAME_, "type '" _UMDK_NAME_ "' for commands list", umdk_meteo_shell_cmd);
    
#ifdef UNWD_CONNECT_BTN
    if (UNWD_USE_CONNECT_BTN) {
        gpio_init_int(UNWD_CONNECT_BTN, GPIO_IN_PU, GPIO_FALLING, btn_connect, NULL);
    }
#endif
    
	timer_pid = thread_create(stack, UMDK_METEO_STACK_SIZE, THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST, timer_thread, NULL, "bme280 thread");

    /* Start publishing timer */
	rtctimers_millis_set_msg(&timer, 60000 * meteo_config.publish_period_min, &timer_msg, timer_pid);
}

static void reply_fail(module_data_t *reply) {
	reply->length = 2;
	reply->data[0] = _UMDK_MID_;
	reply->data[1] = UMDK_METEO_FAIL;
}

static void reply_ok(module_data_t *reply) {
	reply->length = 2;
	reply->data[0] = _UMDK_MID_;
	reply->data[1] = UMDK_METEO_COMMAND;
}

bool umdk_meteo_cmd(module_data_t *cmd, module_data_t *reply) {
	if (cmd->length < 1) {
		reply_fail(reply);
		return true;
	}

	umdk_meteo_cmd_t c = cmd->data[0];
	switch (c) {
	case UMDK_METEO_COMMAND: {
		if (cmd->length != 2) {
			reply_fail(reply);
			break;
		}

		uint8_t period = cmd->data[1];
		set_period(period);

		reply_ok(reply);
		break;
	}

	case UMDK_METEO_POLL:
		is_polled = true;

		/* Send signal to publisher thread */
		msg_send(&timer_msg, timer_pid);

		return false; /* Don't reply */

	default:
		reply_fail(reply);
		break;
	}

	return true;
}

#ifdef __cplusplus
}
#endif
