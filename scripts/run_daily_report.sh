#!/bin/bash
set -e

cd /home/abu/system_monitor
./reporter 0 >> ./logs/reporter.log 2>&1
