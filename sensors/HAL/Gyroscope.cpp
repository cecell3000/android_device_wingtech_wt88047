/*
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/select.h>
#include <log/log.h>
#include <cutils/properties.h>

#include "GyroSensor.h"
#include "sensors.h"

#define GYRO_INPUT_DEV_NAME 	"gyroscope"

#define FETCH_FULL_EVENT_BEFORE_RETURN 	1
#define IGNORE_EVENT_TIME 				350000000
#define SAMPLE_DROP_PERIOD_NS		30000000

#define	EVENT_TYPE_GYRO_X	ABS_RX
#define	EVENT_TYPE_GYRO_Y	ABS_RY
#define	EVENT_TYPE_GYRO_Z	ABS_RZ

#define GYROSCOPE_CONVERT		(M_PI / (180 * 16.4))
#define CONVERT_GYRO_X		(-GYROSCOPE_CONVERT)
#define CONVERT_GYRO_Y		( GYROSCOPE_CONVERT)
#define CONVERT_GYRO_Z		(-GYROSCOPE_CONVERT)

/*****************************************************************************/

GyroSensor::GyroSensor()
	: SensorBase(NULL, GYRO_INPUT_DEV_NAME),
	  mInputReader(4),
	  mHasPendingEvent(false),
	  mIsFirstTimestamp(false),
	  mEnabledTime(0)
{
	mPendingEvent.version = sizeof(sensors_event_t);
	mPendingEvent.sensor = SENSORS_GYROSCOPE_HANDLE;
	mPendingEvent.type = SENSOR_TYPE_GYROSCOPE;
	memset(mPendingEvent.data, 0, sizeof(mPendingEvent.data));
	mPendingEvent.gyro.status = SENSOR_STATUS_ACCURACY_HIGH;

	if (data_fd) {
		strlcpy(input_sysfs_path, "/sys/class/input/", sizeof(input_sysfs_path));
		strlcat(input_sysfs_path, input_name, sizeof(input_sysfs_path));
		strlcat(input_sysfs_path, "/device/device/", sizeof(input_sysfs_path));
		input_sysfs_path_len = strlen(input_sysfs_path);
		enable(0, 1);
	}
}

GyroSensor::GyroSensor(struct SensorContext *context)
	: SensorBase(NULL, NULL, context),
	  mInputReader(4),
	  mHasPendingEvent(false),
	  mIsFirstTimestamp(false),
	  mEnabledTime(0)
{
	mPendingEvent.version = sizeof(sensors_event_t);
	mPendingEvent.sensor = context->sensor->handle;
	mPendingEvent.type = SENSOR_TYPE_GYROSCOPE;
	memset(mPendingEvent.data, 0, sizeof(mPendingEvent.data));
	mPendingEvent.gyro.status = SENSOR_STATUS_ACCURACY_HIGH;

	data_fd = context->data_fd;
	strlcpy(input_sysfs_path, context->enable_path, sizeof(input_sysfs_path));
	input_sysfs_path_len = strlen(input_sysfs_path);
	mUseAbsTimeStamp = false;
	mSensor = *(context->sensor);
	read_dynamic_calibrate_params(&mSensor);

	enable(0, 1);
}

GyroSensor::GyroSensor(char *name)
	: SensorBase(NULL, GYRO_INPUT_DEV_NAME),
	  mInputReader(4),
	  mHasPendingEvent(false),
	  mIsFirstTimestamp(false),
	  mEnabledTime(0)
{
	mPendingEvent.version = sizeof(sensors_event_t);
	mPendingEvent.sensor = SENSORS_GYROSCOPE_HANDLE;
	mPendingEvent.type = SENSOR_TYPE_GYROSCOPE;
	memset(mPendingEvent.data, 0, sizeof(mPendingEvent.data));
	mPendingEvent.gyro.status = SENSOR_STATUS_ACCURACY_HIGH;

	if (data_fd) {
		strlcpy(input_sysfs_path, SYSFS_CLASS, sizeof(input_sysfs_path));
		strlcat(input_sysfs_path, name, sizeof(input_sysfs_path));
		strlcat(input_sysfs_path, "/", sizeof(input_sysfs_path));
		input_sysfs_path_len = strlen(input_sysfs_path);
		ALOGI("The gyroscope sensor path is %s",input_sysfs_path);
		enable(0, 1);
	}
}

GyroSensor::~GyroSensor() {
	if (mEnabled) {
		enable(0, 0);
	}
}

int GyroSensor::setInitialState() {
	struct input_absinfo absinfo_x;
	struct input_absinfo absinfo_y;
	struct input_absinfo absinfo_z;
	float value;
	if (!ioctl(data_fd, EVIOCGABS(EVENT_TYPE_GYRO_X), &absinfo_x) &&
		!ioctl(data_fd, EVIOCGABS(EVENT_TYPE_GYRO_Y), &absinfo_y) &&
		!ioctl(data_fd, EVIOCGABS(EVENT_TYPE_GYRO_Z), &absinfo_z)) {
		value = absinfo_x.value;
		mPendingEvent.data[0] = value * CONVERT_GYRO_X;
		value = absinfo_y.value;
		mPendingEvent.data[1] = value * CONVERT_GYRO_Y;
		value = absinfo_z.value;
		mPendingEvent.data[2] = value * CONVERT_GYRO_Z;
		mHasPendingEvent = true;
	}
	return 0;
}

int GyroSensor::enable(int32_t, int en) {
	int flags = en ? 1 : 0;
	char propBuf[PROPERTY_VALUE_MAX];
	property_get("sensors.gyro.loopback", propBuf, "0");
	if (strcmp(propBuf, "1") == 0) {
		mEnabled = flags;
		mEnabledTime = 0;
		ALOGE("sensors.gyro.loopback is set");
		return 0;
	}
	if (flags != mEnabled) {
		int fd;
		strlcpy(&input_sysfs_path[input_sysfs_path_len],
				SYSFS_ENABLE, SYSFS_MAXLEN);
		fd = open(input_sysfs_path, O_RDWR);
		if (fd >= 0) {
			char buf[2];
			int err;
			buf[1] = 0;
			if (flags) {
				buf[0] = '1';
				mEnabledTime = getTimestamp() + IGNORE_EVENT_TIME;
				sysclk_sync_offset = getClkOffset();
			} else {
				buf[0] = '0';
			}
			err = write(fd, buf, sizeof(buf));
			close(fd);
			mEnabled = flags;
			return 0;
		}
		return -1;
	}
	return 0;
}

bool GyroSensor::hasPendingEvents() const {
	return mHasPendingEvent || mHasPendingMetadata;
}

int GyroSensor::setDelay(int32_t, int64_t delay_ns)
{
	int fd;
	char propBuf[PROPERTY_VALUE_MAX];
	property_get("sensors.gyro.loopback", propBuf, "0");
	if (strcmp(propBuf, "1") == 0) {
		ALOGE("sensors.gyro.loopback is set");
		return 0;
	}
	int delay_ms = delay_ns / 1000000;
	strlcpy(&input_sysfs_path[input_sysfs_path_len],
			SYSFS_POLL_DELAY, SYSFS_MAXLEN);
	fd = open(input_sysfs_path, O_RDWR);
	if (fd >= 0) {
		char buf[80];
		snprintf(buf, sizeof(buf), "%d", delay_ms);
		write(fd, buf, strlen(buf)+1);
		mIsFirstTimestamp = false;
		close(fd);
		return 0;
	}
	return -1;
}

int GyroSensor::readEvents(sensors_event_t* data, int count)
{
	if (count < 1)
		return -EINVAL;

	if (mHasPendingEvent) {
		mHasPendingEvent = false;
		mPendingEvent.timestamp = getTimestamp();
		*data = mPendingEvent;
		return mEnabled ? 1 : 0;
	}

	if (mHasPendingMetadata) {
		mHasPendingMetadata--;
		meta_data.timestamp = getTimestamp();
		*data = meta_data;
		return mEnabled ? 1 : 0;
	}

	ssize_t n = mInputReader.fill(data_fd);
	if (n < 0)
		return n;

	int numEventReceived = 0;
	input_event const* event;
	sensors_event_t raw, result;
	int64_t first_timestamp;

#if FETCH_FULL_EVENT_BEFORE_RETURN
again:
#endif
	while (count && mInputReader.readEvent(&event)) {
		int type = event->type;
		if (type == EV_ABS) {
			float value = event->value;
			if (event->code == EVENT_TYPE_GYRO_X) {
				mPendingEvent.data[0] = value * CONVERT_GYRO_X;
			} else if (event->code == EVENT_TYPE_GYRO_Y) {
				mPendingEvent.data[1] = value * CONVERT_GYRO_Y;
			} else if (event->code == EVENT_TYPE_GYRO_Z) {
				mPendingEvent.data[2] = value * CONVERT_GYRO_Z;
			}
		} else if (type == EV_SYN) {
			switch ( event->code ){
				case SYN_TIME_SEC:
					{
						mUseAbsTimeStamp = true;
						report_time = event->value*1000000000LL;
					}
				break;
				case SYN_TIME_NSEC:
					{
						mUseAbsTimeStamp = true;
						mPendingEvent.timestamp = report_time+event->value;
					}
				break;
				case SYN_REPORT:
					if(mUseAbsTimeStamp != true) {
						mPendingEvent.timestamp = timevalToNano(event->time);
					}
					if (!mEnabled) {
						break;
					}

					mPendingEvent.timestamp -= sysclk_sync_offset;
					raw = mPendingEvent;
					if (algo != NULL) {
						if (algo->methods->convert(&raw, &result, NULL)) {
							ALOGE("Calibrated failed\n");
							result = raw;
						}
					} else {
						result = raw;
					}
					*data = result;
					data->version = sizeof(sensors_event_t);
					data->sensor = mPendingEvent.sensor;
					data->type = SENSOR_TYPE_GYROSCOPE;
					data->timestamp = mPendingEvent.timestamp;
					if (!mIsFirstTimestamp) {
						first_timestamp = data->timestamp;
						mIsFirstTimestamp = true;
					}
					/* The raw data is stored inside sensors_event_t.data after
					 * sensors_event_t.gyroscope. Notice that the raw data is
					 * required to composite the virtual sensor uncalibrated
					 * gyroscope field sensor.
					 *
					 * data[0~2]: calibrated gyroscope field data.
					 * data[3]: gyroscope field data accuracy.
					 * data[4~6]: uncalibrated gyroscope field data.
					 */
					data->data[4] = mPendingEvent.data[0];
					data->data[5] = mPendingEvent.data[1];
					data->data[6] = mPendingEvent.data[2];
					if ((data->timestamp - first_timestamp) > SAMPLE_DROP_PERIOD_NS) {
						data++;
						numEventReceived++;
						count--;
					}
				break;
			}
		} else {
			ALOGE("GyroSensor: unknown event (type=%d, code=%d)",
					type, event->code);
		}
		mInputReader.next();
	}

#if FETCH_FULL_EVENT_BEFORE_RETURN
	/* if we didn't read a complete event, see if we can fill and
	   try again instead of returning with nothing and redoing poll. */
	if (numEventReceived == 0 && mEnabled == 1) {
		n = mInputReader.fill(data_fd);
		if (n)
			goto again;
	}
#endif

	return numEventReceived;
}

int GyroSensor::read_dynamic_calibrate_params(struct sensor_t *sensor)
{
	sensors_XML& sensor_XML(sensors_XML :: getInstance());
	struct cal_result_t cal_result;
	int err = 0;

	err = sensor_XML.read_sensors_params(sensor, &cal_result, 1);
	if (err < 0) {
		ALOGE("read dynamic calibrate %s sensor error\n", sensor->name);
		cal_result.offset[0] = 0;
		cal_result.offset[1] = 0;
		cal_result.offset[2] = 0;
	}

	gyro_algo_args arg;

	arg.bias[0] = cal_result.offset[0];
	arg.bias[1] = cal_result.offset[1];
	arg.bias[2] = cal_result.offset[2];
	arg.common.sensor = *sensor;

	if (algo != NULL) {
		if (algo->methods->config(CMD_INIT, (sensor_algo_args*)&arg)) {
			ALOGE("Init gyro calibration parameters failed\n");
			return -1;
		}
	} else {
		ALOGE("Init gyro algo error\n");
	}

	return 0;
}
