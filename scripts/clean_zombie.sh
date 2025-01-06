#!/bin/bash
for pid in $(ps aux | awk '{ if ($8 == "Z") print $2 }'); do
    ppid=$(ps -o ppid= -p $pid)
    echo "Killing parent process $ppid of zombie $pid"
    kill -9 $ppid
done