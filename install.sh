#!/bin/bash

######## prerequisite ########
# (1) systemd
# Check if systemd is running
if [[ "$(ps -p 1 -o comm=)" != "systemd" ]]; then
    echo "Error: systemd is not running on this system."
    exit 1
fi

# (2) python >= 3.6
PYTHON_VERSION=$(python3 --version 2>/dev/null)

if [[ $? -ne 0 ]]; then
    echo "Error: Python3 is not installed."
    exit 1
fi

VERSION_MAJOR=$(python3 -c "import sys; print(sys.version_info.major)")
VERSION_MINOR=$(python3 -c "import sys; print(sys.version_info.minor)")

if [[ $VERSION_MAJOR -ne 3 || $VERSION_MINOR -lt 6 ]]; then
    echo "Error: Python version is less than 3.6. Found: $PYTHON_VERSION"
    exit 1
else
    echo "Python version is 3.6 or higher: $PYTHON_VERSION"
fi

######### INSTALL ########
# register pingweave service to systemd
sudo cp ./scripts/pingweave.service /etc/systemd/system/

# pingweavectl
sudo cp ./scripts/pingweavectl /usr/local/bin

# start pingweave service
sudo systemctl daemon-reload                
sudo systemctl enable pingweave.service     
sudo systemctl start pingweave.service 

# check service status
sudo systemctl status pingweave.service