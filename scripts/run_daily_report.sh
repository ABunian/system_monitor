#!/bin/bash
set -e
export HOME="/home/abu/system_monitor"
echo "Go to ${HOME}"
cd ${HOME}
echo "----------Start reporter----------"
echo "Log at logs/reporter.log"
./reporter 0 >> ./logs/reporter.log 2>&1
echo "----------End of reporter---------"
