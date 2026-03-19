# system_monitor

Used to record CPU/RAM/DISK usage and generate daily reports and Telegram push notifications.

## Project structure
```text
system_monitor/ 
├── src/ # reporter, collector, service 
├── config/ # configuration files
├── data/ # runtime data (csv, plots)
├── logs/ # event logs
├── Makefile
├── README.md
└── .gitignore
```

## Step 1 -> Build

```bash
make
```
## Step 2 -> Create telegram.conf

```bash
touch ./config/telegram.conf
echo "BOT_TOKEN=${your_bot_token}" >> ./config/telegram.conf
echo "CHAT_ID=${your_chat_id}" >> ./config/telegram.conf
```

## Step 3 -> Set collector to daemon

Start system-monitor.service and check it
```bash
cp ./src/system-monitor.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable system-monitor
sudo systemctl start system-monitor
sudo systemctl status system-monitor
```

## Step 4 -> Set reporter schelue in corntab 

```bash
crontab -e
>> 0 23 * * * /home/${username}/system_monitor/reporter 0
```
