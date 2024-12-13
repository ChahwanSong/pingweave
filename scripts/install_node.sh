#!/bin/bash

cecho(){  # source: https://stackoverflow.com/a/53463162/2886168
    RED="\033[0;31m"
    GREEN="\033[0;32m"
    YELLOW="\033[0;33m"
    NC="\033[0m" # No Color

    printf "${!1}${2} ${NC}\n"
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

######## prerequisite ########
# (1) Check systemd
cecho "YELLOW" "Checking if systemd is running..."
if [[ "$(ps -p 1 -o comm=)" != "systemd" ]]; then
    cecho "RED" "Error: systemd is not running on this system."
    exit 1
fi
cecho "GREEN" "Systemd is running."

# (2) Check Python version (>= 3.6)
cecho "YELLOW" "Checking Python version..."
PYTHON_VERSION=$(python3 --version 2>/dev/null)

if [[ $? -ne 0 ]]; then
    cecho "RED" "Error: Python3 is not installed."
    exit 1
fi

VERSION_MAJOR=$(python3 -c "import sys; print(sys.version_info.major)")
VERSION_MINOR=$(python3 -c "import sys; print(sys.version_info.minor)")

if [[ $VERSION_MAJOR -ne 3 || $VERSION_MINOR -lt 6 ]]; then
    cecho "RED" "Error: Python version is less than 3.6. Found: $PYTHON_VERSION"
    exit 1
else
    cecho "GREEN" "Python version is 3.6 or higher: $PYTHON_VERSION"
fi

###### RDMA CORE ######
check_install_rdma_core() {
    cecho "YELLOW" "Checking RDMA Core installation..."
    if [[ -f /etc/redhat-release ]]; then
        # RHEL-based system
        if ! rpm -q rdma-core &>/dev/null; then
            cecho "YELLOW" "RDMA Core is not installed. Installing..."
            sudo yum install -y rdma-core || {
                cecho "RED" "Error: Failed to install RDMA Core."
                exit 1
            }
        else
            cecho "GREEN" "RDMA Core is already installed."
        fi
    elif [[ -f /etc/lsb-release || -f /etc/debian_version ]]; then
        # Ubuntu-based system
        if ! dpkg -l | grep -q rdma-core; then
            cecho "YELLOW" "RDMA Core is not installed. Installing..."
            sudo apt update && sudo apt install -y rdma-core || {
                cecho "RED" "Error: Failed to install RDMA Core."
                exit 1
            }
        else
            cecho "GREEN" "RDMA Core is already installed."
        fi
    else
        cecho "RED" "Unsupported OS. Please manually install RDMA Core."
        exit 1
    fi
}

check_install_rdma_core

######### Make ########
cecho "YELLOW" "Navigating to source directory and cleaning up..."
cd "$SCRIPT_DIR/../src" || {
    cecho "RED" "Error: Directory '$SCRIPT_DIR/../src' not found or not accessible."
    exit 1
}

if [[ -x "./clear.sh" ]]; then
    bash ./clear.sh
    cecho "GREEN" "Cleanup completed."
else
    cecho "RED" "Error: './clear.sh' is not executable or not found."
    exit 1
fi

# make
cecho "YELLOW" "Running make..."
make || {
    cecho "RED" "Error: 'make' command failed."
    exit 1
}
cecho "GREEN" "Make completed successfully."

######### INSTALL ########
cecho "YELLOW" "Installing pingweave service..."
# Register pingweave service to systemd
sudo cp "$SCRIPT_DIR/pingweave.service" /etc/systemd/system/ || {
    cecho "RED" "Error: Failed to copy pingweave.service to /etc/systemd/system/."
    exit 1
}

# Start pingweave service
sudo systemctl daemon-reload
sudo systemctl enable pingweave.service
sudo systemctl start pingweave.service || {
    cecho "RED" "Error: Failed to start pingweave.service."
    exit 1
}

# Check service status
cecho "YELLOW" "Checking pingweave service status..."
sudo systemctl status pingweave.service || {
    cecho "RED" "Error: Failed to check pingweave service status."
    exit 1
}
cecho "GREEN" "Pingweave service installed and running successfully."

