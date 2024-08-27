
export SCRIPT_PATH="docker-android"
export WORK_PATH="/home/androidusr"
export APP_PATH=${WORK_PATH}/${SCRIPT_PATH}
export LOG_PATH=${WORK_PATH}/logs
export SUPERVISORD_CONFIG_PATH="${APP_PATH}/mixins/configs/process"
export DEVICE_TYPE=emulator

/usr/bin/supervisord --configuration ${SUPERVISORD_CONFIG_PATH}/supervisord-port.conf & \
/usr/bin/supervisord --configuration ${SUPERVISORD_CONFIG_PATH}/supervisord-base.conf & \

bash $(dirname $0)/../tests/run.sh